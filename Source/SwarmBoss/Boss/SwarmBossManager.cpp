#include "SwarmBossManager.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Boss/SwarmFormDefinition.h"
#include "Swarm/SwarmShapeSampler.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwarmBossManager, Log, All);

namespace
{
	// TotalCount를 Definition.Parts의 AllocationWeight 비율로 나눈 부위별 구체 수를 반환한다.
	// 각 부위는 TotalCount × 가중치 / 가중치 합을 정수로 버림한 수를 받고, 버림으로 남은 수는 RemainderPartId 부위에 더한다.
	// 가중치 합이 0보다 크고 RemainderPartId가 Parts 안에 있다는 검증을 마친 데이터만 전달해야 한다.
	TMap<ESwarmPartId, int32> AllocatePartCounts(const USwarmFormDefinition& Definition, int32 TotalCount)
	{
		int64 WeightSum = 0;

		for (const FSwarmPartDefinition& Part : Definition.Parts)
		{
			WeightSum += Part.AllocationWeight;
		}

		TMap<ESwarmPartId, int32> Counts;
		int32 AllocatedCount = 0;

		for (const FSwarmPartDefinition& Part : Definition.Parts)
		{
			const int32 Count = static_cast<int32>(static_cast<int64>(TotalCount) * Part.AllocationWeight / WeightSum);
			Counts.Add(Part.PartId, Count);
			AllocatedCount += Count;
		}

		Counts.FindChecked(Definition.RemainderPartId) += TotalCount - AllocatedCount;

		return Counts;
	}
}

// 몸통과 왼팔의 구체 메시를 표시하고 충돌을 조회할 ISM(UInstancedStaticMeshComponent)을 각각 만든다.
// Parts에 부위별 ISM을 등록하고, 이동 목표가 있을 때만 Actor Tick을 사용하도록 설정한다.
ASwarmBossManager::ASwarmBossManager()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	USceneComponent* BossRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BossRoot"));
	SetRootComponent(BossRoot);

	TorsoInstances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TorsoInstances"));
	LeftArmInstances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("LeftArmInstances"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	for (UInstancedStaticMeshComponent* PartInstances : {TorsoInstances.Get(), LeftArmInstances.Get()})
	{
		PartInstances->SetupAttachment(BossRoot);
		PartInstances->SetMobility(EComponentMobility::Movable);
		PartInstances->SetCanEverAffectNavigation(false);

		if (Sphere.Succeeded())
		{
			PartInstances->SetStaticMesh(Sphere.Object);
		}
	}

	Parts.FindOrAdd(ESwarmPartId::Torso).Instances = TorsoInstances;
	Parts.FindOrAdd(ESwarmPartId::LeftArm).Instances = LeftArmInstances;
}

// Play 시작 시 GenerateInitialParts()를 호출해 몸통과 왼팔의 구체를 생성한다.
void ASwarmBossManager::BeginPlay()
{
	Super::BeginPlay();

	GenerateInitialParts();
}

// FormDefinition의 부위 목록에 따라 InitialParticleCount를 가중치 비율로 나누고, 부위마다 도형 중심 기준의 구체 좌표를 계산한다.
// 정수 배분의 나머지는 RemainderPartId 부위가 받는다. 초기 설정값은 생존 수와 별도로 유지한다.
// 모든 부위의 계산이 성공하면 구체를 생성하고, 생성 크기와 PartGap으로 왼팔 위치를 정해 두 도형을 분리한다.
// 데이터의 RelativeLocation·RelativeRotation은 아직 적용하지 않는다. 축소 후 연결 위치 계산 방식을 정한 뒤 사용한다.
// 성공한 설정은 부위별로 보관한다. 데이터 누락·잘못된 설정·생성 실패는 기존 상태를 유지하고 false를 반환한다.
bool ASwarmBossManager::GenerateInitialParts()
{
	if (InitialParticleCount < 0 || InitialParticleCount > 10000)
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("InitialParticleCount must be between 0 and 10000."));
		return false;
	}

	if (!FMath::IsFinite(PartGap) || PartGap < 0.0f)
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("PartGap must be finite and non-negative."));
		return false;
	}

	if (!FormDefinition)
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("FormDefinition is not assigned on %s."), *GetName());
		return false;
	}

	if (!ValidateFormDefinition(*FormDefinition))
	{
		return false;
	}

	const TMap<ESwarmPartId, int32> PartCounts = AllocatePartCounts(*FormDefinition, InitialParticleCount);

	// 모든 부위의 샘플링이 성공할 때까지 저장소와 ISM을 건드리지 않도록 결과를 먼저 모은다.
	TMap<ESwarmPartId, FSwarmSamplingSettings> PartSettings;
	TMap<ESwarmPartId, FSwarmSamplingResult> PartResults;

	for (const FSwarmPartDefinition& PartDefinition : FormDefinition->Parts)
	{
		FSwarmSamplingSettings Settings;
		Settings.Shape = PartDefinition.Shape;
		Settings.BoxHalfExtents = PartDefinition.BoxHalfExtentsRatio;
		Settings.ParticleCount = PartCounts.FindChecked(PartDefinition.PartId);

		FSwarmSamplingResult Result = FSwarmShapeSampler::GenerateTargets(Settings);

		if (!Result.bSuccess)
		{
			UE_LOG(LogSwarmBossManager, Warning, TEXT("Initial sampling failed for %s part %s: %s"),
				*GetName(), *UEnum::GetValueAsString(PartDefinition.PartId), *Result.Message);
			return false;
		}

		PartSettings.Add(PartDefinition.PartId, Settings);
		PartResults.Add(PartDefinition.PartId, MoveTemp(Result));
	}

	TArray<FSwarmPartPlacement> Placements;
	Placements.Reserve(PartResults.Num());

	for (const FSwarmPartDefinition& PartDefinition : FormDefinition->Parts)
	{
		Placements.Add(FSwarmPartPlacement{PartDefinition.PartId, PartResults.FindChecked(PartDefinition.PartId).Points});
	}

	const FSwarmSamplingSettings& TorsoSettings = PartSettings.FindChecked(ESwarmPartId::Torso);

	if (!InitializeParticles(Placements, TorsoSettings.ParticleRadius))
	{
		return false;
	}

	for (const TPair<ESwarmPartId, FSwarmSamplingSettings>& Pair : PartSettings)
	{
		Parts.FindChecked(Pair.Key).SamplingSettings = Pair.Value;
	}

	const FSwarmSamplingSettings& LeftArmSettings = PartSettings.FindChecked(ESwarmPartId::LeftArm);
	const double CenterDistance = PartResults.FindChecked(ESwarmPartId::Torso).HalfExtents.Y
		+ PartResults.FindChecked(ESwarmPartId::LeftArm).HalfExtents.Y
		+ TorsoSettings.ParticleRadius + LeftArmSettings.ParticleRadius + PartGap;
	LeftArmInstances->SetRelativeLocation(FVector(0.0, -CenterDistance, 15.0));

	return true;
}

// 초기 생성에 사용할 형태 데이터를 교체한다. 유효성은 다음 GenerateInitialParts() 호출 때 검사한다.
void ASwarmBossManager::SetFormDefinition(USwarmFormDefinition* NewFormDefinition)
{
	FormDefinition = NewFormDefinition;
}

// Definition을 이 Manager가 생성할 수 있는지 검사한다. 문제가 있으면 첫 번째 원인을 경고 로그로 남기고 false를 반환한다.
// 검사 항목: 부위 ID 유일성과 None/Preview 금지, 이 Manager에 ISM이 있는 부위를 빠짐없이 정의했는지,
// 가중치가 0 이상이고 합이 0보다 큰지, 종횡비·자세 값의 유효성, 부모가 같은 목록 안에 있는지,
// 부모 없는 최상위 부위가 몸통 하나뿐인지, 순환 연결이 없는지, 잔여 수신 부위가 목록 안에 있는지.
// 에디터의 Clamp 메타데이터는 코드·잘못된 에셋으로 들어온 값을 막지 못하므로 여기서 다시 검사한다.
bool ASwarmBossManager::ValidateFormDefinition(const USwarmFormDefinition& Definition) const
{
	auto Fail = [this, &Definition](const FString& Reason)
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("Invalid FormDefinition %s for %s: %s"),
			*Definition.GetName(), *GetName(), *Reason);
		return false;
	};

	TMap<ESwarmPartId, ESwarmPartId> ParentOf;
	int64 WeightSum = 0;

	for (const FSwarmPartDefinition& Part : Definition.Parts)
	{
		const FString PartName = UEnum::GetValueAsString(Part.PartId);

		if (Part.PartId == ESwarmPartId::None || Part.PartId == ESwarmPartId::Preview)
		{
			return Fail(FString::Printf(TEXT("%s cannot be used as a part."), *PartName));
		}

		if (ParentOf.Contains(Part.PartId))
		{
			return Fail(FString::Printf(TEXT("%s is defined more than once."), *PartName));
		}

		if (!Parts.Contains(Part.PartId))
		{
			return Fail(FString::Printf(TEXT("%s has no instance component in this manager."), *PartName));
		}

		if (Part.AllocationWeight < 0)
		{
			return Fail(FString::Printf(TEXT("%s has a negative allocation weight."), *PartName));
		}

		if (Part.BoxHalfExtentsRatio.ContainsNaN() || Part.BoxHalfExtentsRatio.GetMin() <= 0.0)
		{
			return Fail(FString::Printf(TEXT("%s box ratio must be finite and positive."), *PartName));
		}

		if (Part.RelativeLocation.ContainsNaN() || Part.RelativeRotation.ContainsNaN())
		{
			return Fail(FString::Printf(TEXT("%s relative pose must be finite."), *PartName));
		}

		if (Part.ParentPartId == Part.PartId)
		{
			return Fail(FString::Printf(TEXT("%s cannot be its own parent."), *PartName));
		}

		ParentOf.Add(Part.PartId, Part.ParentPartId);
		WeightSum += Part.AllocationWeight;
	}

	if (ParentOf.Num() != Parts.Num())
	{
		return Fail(TEXT("Every part owned by the manager must be defined."));
	}

	if (WeightSum <= 0)
	{
		return Fail(TEXT("Allocation weights must sum to a positive value."));
	}

	if (!ParentOf.Contains(Definition.RemainderPartId))
	{
		return Fail(TEXT("RemainderPartId must be one of the defined parts."));
	}

	int32 RootCount = 0;

	for (const TPair<ESwarmPartId, ESwarmPartId>& Pair : ParentOf)
	{
		if (Pair.Value == ESwarmPartId::None)
		{
			++RootCount;

			if (Pair.Key != ESwarmPartId::Torso)
			{
				return Fail(TEXT("Only the torso can have no parent."));
			}
		}
		else if (!ParentOf.Contains(Pair.Value))
		{
			return Fail(FString::Printf(TEXT("%s refers to an undefined parent."), *UEnum::GetValueAsString(Pair.Key)));
		}
	}

	if (RootCount != 1)
	{
		return Fail(TEXT("Exactly one root part (the torso) is required."));
	}

	// 각 부위에서 부모를 따라 올라가 부위 수 이내에 최상위에 도달하지 못하면 순환 연결로 판단한다.
	for (const TPair<ESwarmPartId, ESwarmPartId>& Pair : ParentOf)
	{
		ESwarmPartId Current = Pair.Key;
		int32 Steps = 0;

		while (Current != ESwarmPartId::None && Steps <= ParentOf.Num())
		{
			Current = ParentOf.FindChecked(Current);
			++Steps;
		}

		if (Current != ESwarmPartId::None)
		{
			return Fail(FString::Printf(TEXT("%s is part of a parent cycle."), *UEnum::GetValueAsString(Pair.Key)));
		}
	}

	return true;
}

// 중앙 Particles 배열을 앞에서부터 읽어, 활성 상태이며 소속이 PartId인 구체를 최대 MaxCount개 선택한다.
// 반환값은 중앙 배열의 인덱스(ParticleId) 목록이다. 부족하면 있는 만큼, MaxCount가 0 이하이면 빈 배열을 반환한다.
// 구체의 소속이나 위치는 바꾸지 않으며, 가까운 순서가 아닌 중앙 배열 순서로 선택한다.
// TODO: 관절을 통한 부위 간 전달을 구현할 때, 부위별 공간 자료구조에서 관절 근처 구체를 검색하도록 전환한다.
TArray<int32> ASwarmBossManager::SelectPartParticleIds(ESwarmPartId PartId, int32 MaxCount) const
{
	TArray<int32> SelectedParticleIds;

	if (MaxCount <= 0)
	{
		return SelectedParticleIds;
	}

	const TArray<FSwarmParticle>& Particles = ParticleStore.GetParticles();
	SelectedParticleIds.Reserve(FMath::Min(MaxCount, ParticleStore.GetActiveParticleCount()));

	for (int32 ParticleId = 0; ParticleId < Particles.Num(); ++ParticleId)
	{
		const FSwarmParticle& Particle = Particles[ParticleId];

		if (!Particle.bActive || Particle.PartId != PartId)
		{
			continue;
		}

		SelectedParticleIds.Add(ParticleId);

		if (SelectedParticleIds.Num() == MaxCount)
		{
			break;
		}
	}

	return SelectedParticleIds;
}

// Manager가 소유한 중앙 저장소에서 모든 부위의 생존 구체 수를 합친 값을 반환한다.
int32 ASwarmBossManager::GetActiveParticleCount() const
{
	return ParticleStore.GetActiveParticleCount();
}

// Manager가 소유한 중앙 저장소의 Particles 배열을 읽기 전용 참조로 반환한다.
// 배열에는 제거되어 bActive가 false인 구체도 남아 있다.
const TArray<FSwarmParticle>& ASwarmBossManager::GetParticles() const
{
	return ParticleStore.GetParticles();
}

// 군체 시작 시 도형별 초기 좌표로 구체 데이터와 메시 인스턴스를 구성한다.
// Placements에는 각 도형의 ID와 해당 도형 중심 기준의 좌표 목록을 전달한다.
// 입력을 확인한 뒤 저장소와 인스턴스를 초기화하므로, 다시 호출하면 기존 구체도 새로 생성된다.
// 초기화에 성공하면 true, 도형별 입력이 누락·중복되거나 ISM이 유효하지 않으면 false를 반환한다.
bool ASwarmBossManager::InitializeParticles(TConstArrayView<FSwarmPartPlacement> Placements, double ParticleRadius)
{
	if (Placements.Num() != Parts.Num())
	{
		return false;
	}

	int32 TotalCount = 0;
	TSet<ESwarmPartId> PlacedParts;

	for (const FSwarmPartPlacement& Placement : Placements)
	{
		const FSwarmPart* Part = Parts.Find(Placement.PartId);

		if (!Part || !IsValid(Part->Instances) || PlacedParts.Contains(Placement.PartId))
		{
			return false;
		}

		PlacedParts.Add(Placement.PartId);
		TotalCount += Placement.LocalPositions.Num();
	}

	ResetParticles(TotalCount);

	for (const FSwarmPartPlacement& Placement : Placements)
	{
		AddPartParticles(Placement, ParticleRadius);
	}

	return true;
}

// Placement의 로컬 좌표마다 해당 도형의 메시 인스턴스와 Particles 배열 원소를 추가한다.
// 구체에는 InstanceIndex를, 도형의 InstanceToParticle 배열에는 ParticleId를 기록한다.
// 이 연결로 구체를 이동시킬 인스턴스와 피격 인스턴스가 가리키는 구체를 서로 찾을 수 있다.
void ASwarmBossManager::AddPartParticles(const FSwarmPartPlacement& Placement, double ParticleRadius)
{
	FSwarmPart& Part = Parts.FindChecked(Placement.PartId);
	UInstancedStaticMeshComponent* Instances = Part.Instances.Get();

	TArray<FTransform> Transforms;
	Transforms.Reserve(Placement.LocalPositions.Num());

	// Engine BasicShapes/Sphere의 반지름은 50cm이다.
	const FVector ParticleScale(ParticleRadius / 50.0);

	for (const FVector& Point : Placement.LocalPositions)
	{
		Transforms.Emplace(FQuat::Identity, Point, ParticleScale);
	}

	Instances->SetCollisionProfileName(TEXT("SwarmSpherePreset"));
	Instances->SetRemoveSwap();

	const TArray<int32> NewInstanceIndices = Instances->AddInstances(
		Transforms,
		/*bShouldReturnIndices=*/true,
		/*bWorldSpace=*/false,
		/*bUpdateNavigation=*/false);

	check(NewInstanceIndices.Num() == Placement.LocalPositions.Num());
	const int32 FirstParticleId = ParticleStore.AddParticles(Placement.PartId, Placement.LocalPositions);

	Part.InstanceToParticle.Init(INDEX_NONE, Instances->GetInstanceCount());

	for (int32 PointIndex = 0; PointIndex < NewInstanceIndices.Num(); ++PointIndex)
	{
		const int32 ParticleId = FirstParticleId + PointIndex;
		const int32 InstanceIndex = NewInstanceIndices[PointIndex];

		ParticleStore.SetInstanceIndex(ParticleId, InstanceIndex);
		Part.InstanceToParticle[InstanceIndex] = ParticleId;
	}
}

// 이동 Tick을 끄고 각 도형의 메시 인스턴스와 InstanceToParticle 배열을 비운다.
// 중앙 Particles 배열도 비우고 ExpectedParticleCount개를 담을 용량을 확보한다.
void ASwarmBossManager::ResetParticles(int32 ExpectedParticleCount)
{
	SetActorTickEnabled(false);

	for (TPair<ESwarmPartId, FSwarmPart>& Entry : Parts)
	{
		FSwarmPart& Part = Entry.Value;

		if (IsValid(Part.Instances))
		{
			Part.Instances->ClearInstances();
		}

		Part.InstanceToParticle.Reset();
	}

	ParticleStore.Reset(ExpectedParticleCount);
}

// HitComponent와 같은 메시 컴포넌트를 가진 Parts 항목을 찾아 반환한다.
// 반환된 항목에는 해당 도형의 ISM과 인스턴스 번호를 ParticleId로 바꾸는 배열이 있다. 없으면 nullptr를 반환한다.
FSwarmPart* ASwarmBossManager::FindPartByComponent(const UPrimitiveComponent* HitComponent)
{
	for (TPair<ESwarmPartId, FSwarmPart>& Entry : Parts)
	{
		if (Entry.Value.Instances.Get() == HitComponent)
		{
			return &Entry.Value;
		}
	}

	return nullptr;
}

// HitComponent가 가리키는 도형에서 InstanceIndex번 구체를 제거한다.
// 마지막 메시 인스턴스가 삭제된 자리를 채우면, 그 구체의 InstanceIndex와 InstanceToParticle을 함께 갱신한다.
// 제거한 구체는 Particles 배열에 비활성 상태로 남기며, 실제 삭제가 성공했을 때 true를 반환한다.
bool ASwarmBossManager::RemoveInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex)
{
	if (!IsValid(HitComponent))
	{
		return false;
	}

	FSwarmPart* Part = FindPartByComponent(HitComponent);

	if (!Part)
	{
		return false;
	}

	UInstancedStaticMeshComponent* Instances = Part->Instances.Get();
	TArray<int32>& InstanceToParticle = Part->InstanceToParticle;

	if (InstanceToParticle.Num() != Instances->GetInstanceCount()
		|| !InstanceToParticle.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	const int32 RemovedParticleId = InstanceToParticle[InstanceIndex];
	const int32 LastInstanceIndex = InstanceToParticle.Num() - 1;
	const int32 LastParticleId = InstanceToParticle[LastInstanceIndex];

	if (!Instances->RemoveInstance(InstanceIndex))
	{
		return false;
	}

	ParticleStore.DeactivateParticle(RemovedParticleId);

	// ISM과 같은 방식으로 마지막 항목을 삭제한 자리에 옮긴다.
	InstanceToParticle.RemoveAtSwap(InstanceIndex, EAllowShrinking::No);

	if (InstanceIndex != LastInstanceIndex)
	{
		ParticleStore.SetInstanceIndex(LastParticleId, InstanceIndex);
	}

	UE_LOG(LogSwarmBossManager, Display,
		TEXT("Removed Instance=%d Remaining=%d"),
		InstanceIndex,
		GetActiveParticleCount());

	return true;
}

// 중앙 배열의 활성 ParticleId 하나를 DestinationPartId 부위로 옮기고, 양쪽 ISM과 연결 정보를 갱신한다.
// 현재 월드 Transform과 기존 목표의 월드 위치를 유지하도록 목적지 ISM 기준 좌표로 변환한다.
// 중앙 ParticleId와 전체 생존 수는 유지하며, 새 이동 목표를 배정하거나 Tick을 켜지 않는다.
// 잘못된 ID·부위, 비활성 구체, 동일 부위 요청은 false를 반환한다. 소속 변경이 완료되면 true를 반환한다.
bool ASwarmBossManager::TransferParticle(int32 ParticleId, ESwarmPartId DestinationPartId)
{
	const TArray<FSwarmParticle>& Particles = ParticleStore.GetParticles();

	if (!Particles.IsValidIndex(ParticleId))
	{
		return false;
	}

	const FSwarmParticle& Particle = Particles[ParticleId];

	if (!Particle.bActive || Particle.PartId == DestinationPartId)
	{
		return false;
	}

	FSwarmPart* SourcePart = Parts.Find(Particle.PartId);
	FSwarmPart* DestinationPart = Parts.Find(DestinationPartId);

	if (!SourcePart || !DestinationPart
		|| !IsValid(SourcePart->Instances) || !IsValid(DestinationPart->Instances)
		|| SourcePart->Instances == DestinationPart->Instances)
	{
		return false;
	}

	UInstancedStaticMeshComponent* SourceInstances = SourcePart->Instances.Get();
	UInstancedStaticMeshComponent* DestinationInstances = DestinationPart->Instances.Get();
	const int32 SourceInstanceIndex = Particle.InstanceIndex;

	if (SourcePart->InstanceToParticle.Num() != SourceInstances->GetInstanceCount()
		|| DestinationPart->InstanceToParticle.Num() != DestinationInstances->GetInstanceCount()
		|| !SourcePart->InstanceToParticle.IsValidIndex(SourceInstanceIndex)
		|| SourcePart->InstanceToParticle[SourceInstanceIndex] != ParticleId)
	{
		return false;
	}

	FTransform WorldTransform;
	const FTransform DestinationTransform = DestinationInstances->GetComponentTransform();

	if (!SourceInstances->GetInstanceTransform(SourceInstanceIndex, WorldTransform, /*bWorldSpace=*/true)
		|| DestinationTransform.GetScale3D().GetAbsMin() <= UE_SMALL_NUMBER)
	{
		return false;
	}

	const FTransform DestinationLocalTransform = WorldTransform.GetRelativeTransform(DestinationTransform);
	const FVector DestinationLocalPosition = DestinationLocalTransform.GetLocation();
	const FVector WorldTarget = SourceInstances->GetComponentTransform().TransformPosition(Particle.TargetLocalPosition);
	const FVector DestinationLocalTarget = Particle.LocalPosition == Particle.TargetLocalPosition
		? DestinationLocalPosition
		: DestinationTransform.InverseTransformPosition(WorldTarget);

	if (DestinationLocalTransform.ContainsNaN() || DestinationLocalTarget.ContainsNaN())
	{
		return false;
	}

	const int32 LastSourceIndex = SourcePart->InstanceToParticle.Num() - 1;
	const int32 LastSourceParticleId = SourcePart->InstanceToParticle.Last();

	// 목적지에 먼저 추가하고, 출발지에서 제거하지 못하면 새로 추가한 인스턴스만 되돌린다.
	const int32 DestinationInstanceIndex = DestinationInstances->AddInstance(DestinationLocalTransform);

	if (DestinationInstanceIndex == INDEX_NONE)
	{
		return false;
	}

	if (!SourceInstances->RemoveInstance(SourceInstanceIndex))
	{
		const bool bRolledBack = DestinationInstances->RemoveInstance(DestinationInstanceIndex);
		ensureMsgf(bRolledBack, TEXT("Failed to roll back particle transfer."));
		return false;
	}

	SourcePart->InstanceToParticle.RemoveAtSwap(SourceInstanceIndex, EAllowShrinking::No);

	if (SourceInstanceIndex != LastSourceIndex)
	{
		ParticleStore.SetInstanceIndex(LastSourceParticleId, SourceInstanceIndex);
	}

	// AddInstance는 끝에 추가하므로 목적지 연결 배열도 같은 번호에 ParticleId를 추가한다.
	check(DestinationInstanceIndex == DestinationPart->InstanceToParticle.Num());
	DestinationPart->InstanceToParticle.Add(ParticleId);
	ParticleStore.UpdateParticlePart(ParticleId, DestinationPartId, DestinationInstanceIndex,
		DestinationLocalPosition, DestinationLocalTarget);

	return true;
}

// SourcePartId 부위에서 목표 좌표 수만큼 활성 구체를 배열 순서로 선택해 DestinationPartId 부위로 보낸다.
// TargetLocalPositions는 목적지 ISM 기준이며, 전달에 성공한 구체에 앞에서부터 하나씩 배정한다.
// 목표가 비어 있거나 유효하지 않은 좌표가 포함되면 아무것도 변경하지 않고 0을 반환한다.
// 부위가 잘못됐거나 같아도 0을 반환한다. 개별 전달 실패는 건너뛰고 실제 소속 변경 성공 수를 반환한다.
// 목표 생성·공간 검색·겹침 검사는 수행하지 않으며, 전체 생존 수는 유지한다.
int32 ASwarmBossManager::TransferParticlesToTargets(ESwarmPartId SourcePartId, ESwarmPartId DestinationPartId,
	TConstArrayView<FVector> TargetLocalPositions)
{
	if (TargetLocalPositions.IsEmpty() || SourcePartId == DestinationPartId
		|| !Parts.Contains(SourcePartId) || !Parts.Contains(DestinationPartId))
	{
		return 0;
	}

	for (const FVector& Target : TargetLocalPositions)
	{
		if (Target.ContainsNaN())
		{
			return 0;
		}
	}

	const TArray<int32> ParticleIds = SelectPartParticleIds(SourcePartId, TargetLocalPositions.Num());
	int32 TransferredCount = 0;

	for (int32 ParticleId : ParticleIds)
	{
		if (!TransferParticle(ParticleId, DestinationPartId))
		{
			continue;
		}

		const FVector TargetLocalPosition = TargetLocalPositions[TransferredCount];
		++TransferredCount;

		if (!AssignParticleTarget(ParticleId, TargetLocalPosition))
		{
			UE_LOG(LogSwarmBossManager, Warning,
				TEXT("Transfer: target assignment failed for ParticleId=%d."), ParticleId);
		}
	}

	return TransferredCount;
}

// 두 부위의 현재 생존 수 합계를 FirstWeight:SecondWeight로 나눠 초과 부위에서 부족 부위로 전달한다.
// 두 번째 부위의 목표 개수는 정수로 버림하고 나머지는 첫 번째 부위에 배정한다.
// 가중치는 0 이상이고 합이 양수여야 한다. 가중치 0인 부위는 구체를 모두 다른 부위로 보낸다.
// 각 부위의 초기 생성 설정에서 개수만 바꿔 새 좌표를 생성한 뒤, 소속 변경과 양쪽 전체 목표 배정을 수행한다.
// 새 목표는 배정 시점의 현재 위치에서 ReconstructionDuration초 동안 이동하도록 설정한다.
// 재구성 시간이 유효한 양수가 아니면 거부한다. 현재 개수가 이미 목표와 같으면 아무것도 바꾸지 않는다.
// 생성 실패 전에는 상태를 변경하지 않는다.
// 전달이 일부만 성공하면 성공한 변경은 유지하고 false를 반환한다. 전체 완료 또는 변경 불필요 시 true를 반환한다.
// 관절 근처 선택과 연결 관계 탐색은 아직 수행하지 않으며, 선택은 기존 중앙 배열 순서를 사용한다.
bool ASwarmBossManager::RebalanceParts(ESwarmPartId FirstPartId, ESwarmPartId SecondPartId,
	int32 FirstWeight, int32 SecondWeight)
{
	if (!FMath::IsFinite(ReconstructionDuration) || ReconstructionDuration <= 0.0f)
	{
		return false;
	}

	FSwarmPart* PartPair[] = {Parts.Find(FirstPartId), Parts.Find(SecondPartId)};

	if (FirstPartId == SecondPartId || FirstWeight < 0 || SecondWeight < 0 || int64(FirstWeight) + SecondWeight <= 0
		|| !PartPair[0] || !PartPair[1]
		|| !IsValid(PartPair[0]->Instances) || !IsValid(PartPair[1]->Instances))
	{
		return false;
	}

	const int32 CurrentCounts[] = {PartPair[0]->InstanceToParticle.Num(), PartPair[1]->InstanceToParticle.Num()};
	const int32 TotalCount = CurrentCounts[0] + CurrentCounts[1];
	const int64 TotalWeight = int64(FirstWeight) + SecondWeight;
	const int32 SecondTargetCount = int32(int64(TotalCount) * SecondWeight / TotalWeight);
	const int32 TargetCounts[] = {TotalCount - SecondTargetCount, SecondTargetCount};

	if (CurrentCounts[0] == TargetCounts[0])
	{
		UE_LOG(LogSwarmBossManager, Display, TEXT("Rebalance %s: already balanced, First=%d Second=%d Total=%d"),
			*GetName(), CurrentCounts[0], CurrentCounts[1], GetActiveParticleCount());
		return true;
	}

	FSwarmSamplingResult Results[2];

	for (int32 PartIndex = 0; PartIndex < 2; ++PartIndex)
	{
		FSwarmSamplingSettings Settings = PartPair[PartIndex]->SamplingSettings;
		Settings.ParticleCount = TargetCounts[PartIndex];
		Results[PartIndex] = FSwarmShapeSampler::GenerateTargets(Settings);

		if (!Results[PartIndex].bSuccess)
		{
			UE_LOG(LogSwarmBossManager, Warning, TEXT("Rebalance sampling failed: %s"), *Results[PartIndex].Message);
			return false;
		}
	}

	const ESwarmPartId PartIds[] = {FirstPartId, SecondPartId};
	const int32 SourceIndex = CurrentCounts[0] > TargetCounts[0] ? 0 : 1;
	const int32 DestinationIndex = 1 - SourceIndex;
	const int32 RequiredCount = CurrentCounts[SourceIndex] - TargetCounts[SourceIndex];
	const TConstArrayView<FVector> TransferTargets = MakeArrayView(Results[DestinationIndex].Points).Right(RequiredCount);
	const int32 TransferredCount = TransferParticlesToTargets(PartIds[SourceIndex], PartIds[DestinationIndex], TransferTargets);

	UE_LOG(LogSwarmBossManager, Display,
		TEXT("Rebalance %s: %s -> %s Requested=%d Transferred=%d First=%d Second=%d Total=%d"),
		*GetName(), *UEnum::GetValueAsString(PartIds[SourceIndex]), *UEnum::GetValueAsString(PartIds[DestinationIndex]),
		RequiredCount, TransferredCount, PartPair[0]->InstanceToParticle.Num(), PartPair[1]->InstanceToParticle.Num(),
		GetActiveParticleCount());

	if (TransferredCount != RequiredCount)
	{
		return false;
	}

	// 전달한 구체뿐 아니라 각 부위의 생존 구체 전체에 새 목표를 하나씩 배정해 최종 배치가 겹치지 않게 한다.
	for (int32 PartIndex = 0; PartIndex < 2; ++PartIndex)
	{
		const TArray<int32> ParticleIds = SelectPartParticleIds(PartIds[PartIndex], TargetCounts[PartIndex]);

		if (ParticleIds.Num() != TargetCounts[PartIndex])
		{
			return false;
		}

		for (int32 Index = 0; Index < ParticleIds.Num(); ++Index)
		{
			if (!AssignParticleTarget(ParticleIds[Index], Results[PartIndex].Points[Index], ReconstructionDuration))
			{
				return false;
			}
		}
	}

	return true;
}

// T 입력 시점의 FormDefinition에서 부위별 AllocationWeight를 읽어 RebalanceParts()에 전달한다.
// RemainderPartId 부위를 첫 번째 부위로 넘겨 버림 나머지를 받게 하므로 초기 생성과 같은 배분 규칙을 따른다.
// 가중치만 다시 읽으며, 도형 종류·종횡비는 마지막 초기 생성 때 보관한 설정을 그대로 사용한다.
// 데이터가 없거나 유효하지 않거나 부위가 둘이 아니면 경고를 남기고 상태를 바꾸지 않은 채 false를 반환한다.
// TODO: 거인 전체 부위를 추가할 때 연결 관계를 따라 여러 부위를 재분배하도록 확장한다.
bool ASwarmBossManager::RebalancePartsFromDefinition()
{
	if (!FormDefinition)
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("FormDefinition is not assigned on %s."), *GetName());
		return false;
	}

	if (!ValidateFormDefinition(*FormDefinition))
	{
		return false;
	}

	if (FormDefinition->Parts.Num() != 2)
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("Rebalance currently supports exactly two parts, but %s has %d."),
			*FormDefinition->GetName(), FormDefinition->Parts.Num());
		return false;
	}

	const ESwarmPartId RemainderPartId = FormDefinition->RemainderPartId;
	const FSwarmPartDefinition* RemainderPart = FormDefinition->Parts.FindByPredicate(
		[RemainderPartId](const FSwarmPartDefinition& Part) { return Part.PartId == RemainderPartId; });
	const FSwarmPartDefinition* OtherPart = FormDefinition->Parts.FindByPredicate(
		[RemainderPartId](const FSwarmPartDefinition& Part) { return Part.PartId != RemainderPartId; });

	return RebalanceParts(RemainderPart->PartId, OtherPart->PartId,
		RemainderPart->AllocationWeight, OtherPart->AllocationWeight);
}

// Particles[ParticleId]에 소속 도형 기준의 TargetLocalPosition을 새 이동 목표로 저장한다.
// MoveDuration이 양수이면 초반에 빠르고 끝에서 느려지는 이차 감속 곡선으로 그 시간(초)에 도착한다.
// MoveDuration이 0이면 기존 초당 200cm 이동을 사용한다.
// 목표 배정이 성공하면 필요한 경우 Tick을 켜고 true를 반환하며, 실제 위치는 이후 Tick에서 갱신한다.
bool ASwarmBossManager::AssignParticleTarget(int32 ParticleId, const FVector& TargetLocalPosition, float MoveDuration)
{
	if (!ParticleStore.AssignTarget(ParticleId, TargetLocalPosition, MoveDuration))
	{
		return false;
	}

	if (ParticleStore.GetParticles()[ParticleId].LocalPosition != TargetLocalPosition)
	{
		SetActorTickEnabled(true);
	}

	return true;
}

// Targets의 좌표 수가 전체 생존 수와 같고 모든 좌표가 유효한지 먼저 확인한다.
// Particles 배열을 앞에서부터 읽으며 살아 있는 구체에 목표를 하나씩 배정한다. 각 목표는 소속 도형 기준 좌표로 해석한다.
// 모든 목표를 ReconstructionDuration초 이동으로 배정한다. 시간은 유효한 양수여야 한다.
// 입력이나 시간 검증에 실패하면 false, 배정하면 true를 반환한다.
bool ASwarmBossManager::AssignReconstructionTargets(const TArray<FVector>& Targets)
{
	if (!FMath::IsFinite(ReconstructionDuration) || ReconstructionDuration <= 0.0f
		|| Targets.Num() != GetActiveParticleCount())
	{
		return false;
	}

	// 하나라도 잘못된 좌표가 있으면 기존 목표를 그대로 둔다.
	for (const FVector& Target : Targets)
	{
		if (Target.ContainsNaN())
		{
			return false;
		}
	}

	const TArray<FSwarmParticle>& Particles = GetParticles();
	int32 TargetIndex = 0;

	for (int32 ParticleId = 0; ParticleId < Particles.Num(); ++ParticleId)
	{
		if (!Particles[ParticleId].bActive)
		{
			continue;
		}

		AssignParticleTarget(ParticleId, Targets[TargetIndex], ReconstructionDuration);
		++TargetIndex;
	}

	return true;
}

// 프레임마다 UpdateParticleMovement()를 호출해 구체 위치를 갱신한다.
// 해당 함수가 이동을 계속할 구체가 없다고 반환하면 Manager의 Actor Tick을 끈다.
void ASwarmBossManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!UpdateParticleMovement(DeltaTime))
	{
		SetActorTickEnabled(false);
	}
}

// 시간 지정 구체는 남은 거리가 남은 시간의 제곱에 비례하도록 이동해 초반에 빠르고 끝에서 느려진다.
// 현재 대비 다음 프레임의 남은 시간 비율을 제곱하므로, 시작 위치를 별도 저장하지 않아도 같은 이차 감속 곡선을 따른다.
// 시간 지정이 없으면 초당 200cm로 이동한다. 도착 프레임에는 목표에 정확히 맞춰 초과 이동을 막는다.
// ISM 갱신이 성공한 경우에만 중앙 데이터의 현재 위치와 남은 시간을 함께 갱신한다.
// 성공적으로 갱신한 구체 중 아직 목표에 도착하지 않은 구체가 있으면 true를 반환한다.
bool ASwarmBossManager::UpdateParticleMovement(float DeltaTime)
{
	constexpr float MoveSpeed = 200.0f; // cm/s
	TArray<UInstancedStaticMeshComponent*, TInlineAllocator<2>> ChangedInstances;
	bool bHasMovingParticles = false;

	const TArray<FSwarmParticle>& Particles = ParticleStore.GetParticles();

	for (int32 ParticleId = 0; ParticleId < Particles.Num(); ++ParticleId)
	{
		const FSwarmParticle& Particle = Particles[ParticleId];
		if (!Particle.bActive || Particle.LocalPosition == Particle.TargetLocalPosition)
		{
			continue;
		}

		const FSwarmPart* Part = Parts.Find(Particle.PartId);

		if (!Part || !IsValid(Part->Instances))
		{
			continue;
		}

		UInstancedStaticMeshComponent* Instances = Part->Instances.Get();
		FTransform InstanceTransform;

		if (!Instances->GetInstanceTransform(Particle.InstanceIndex, InstanceTransform, false))
		{
			continue;
		}

		FVector NextPosition;
		float RemainingMoveTime = 0.0f;

		if (Particle.RemainingMoveTime > 0.0f)
		{
			RemainingMoveTime = FMath::Max(0.0f, Particle.RemainingMoveTime - DeltaTime);
			if (RemainingMoveTime <= UE_SMALL_NUMBER)
			{
				NextPosition = Particle.TargetLocalPosition;
			}
			else
			{
				const double RemainingRatio = double(RemainingMoveTime) / Particle.RemainingMoveTime;
				const double MoveAlpha = 1.0 - FMath::Square(RemainingRatio);
				NextPosition = FMath::Lerp(Particle.LocalPosition, Particle.TargetLocalPosition, MoveAlpha);
			}

			if (NextPosition == Particle.TargetLocalPosition)
			{
				RemainingMoveTime = 0.0f;
			}
		}
		else
		{
			NextPosition = FMath::VInterpConstantTo(
				Particle.LocalPosition,
				Particle.TargetLocalPosition,
				DeltaTime,
				MoveSpeed);
		}

		InstanceTransform.SetLocation(NextPosition);

		if (Instances->UpdateInstanceTransform(
			Particle.InstanceIndex,
			InstanceTransform,
			/*bWorldSpace=*/false,
			/*bMarkRenderStateDirty=*/false,
			/*bTeleport=*/true))
		{
			ParticleStore.UpdateMovementProgress(ParticleId, NextPosition, RemainingMoveTime);
			ChangedInstances.AddUnique(Instances);
			bHasMovingParticles |= NextPosition != Particle.TargetLocalPosition;
		}
	}

	for (UInstancedStaticMeshComponent* Instances : ChangedInstances)
	{
		Instances->MarkRenderStateDirty();
	}

	return bHasMovingParticles;
}
