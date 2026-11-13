#include "SwarmBossManager.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Swarm/SwarmShapeSampler.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwarmBossManager, Log, All);

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

// InitialParticleCount를 몸통:왼팔 2:1로 나눠 각 도형 중심 기준의 구체 좌표를 계산한다.
// 왼팔은 전체의 1/3을 정수로 버림하고 나머지는 몸통에 배정한다. 초기 설정값은 생존 수와 별도로 유지한다.
// 양쪽 계산이 성공하면 구체를 생성하고, 생성 크기와 PartGap으로 왼팔 위치를 정해 두 도형을 분리한다.
// 성공한 설정은 부위별로 보관한다. 잘못된 설정이나 생성 실패는 기존 상태를 유지하고 false를 반환한다.
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

	const int32 LeftArmCount = InitialParticleCount / 3;
	const int32 TorsoCount = InitialParticleCount - LeftArmCount;

	FSwarmSamplingSettings TorsoSettings;
	TorsoSettings.Shape = ESwarmPrimitive::Box;
	TorsoSettings.BoxHalfExtents = FVector(1.0, 0.75, 1.5);
	TorsoSettings.ParticleCount = TorsoCount;

	FSwarmSamplingSettings LeftArmSettings = TorsoSettings;
	LeftArmSettings.BoxHalfExtents = FVector(0.45, 0.45, 1.0);
	LeftArmSettings.ParticleCount = LeftArmCount;

	const FSwarmSamplingResult Torso = FSwarmShapeSampler::GenerateTargets(TorsoSettings);
	const FSwarmSamplingResult LeftArm = FSwarmShapeSampler::GenerateTargets(LeftArmSettings);

	if (!Torso.bSuccess || !LeftArm.bSuccess)
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("Initial sampling failed for %s. Torso=%s LeftArm=%s"),
			*GetName(), *Torso.Message, *LeftArm.Message);
		return false;
	}

	const FSwarmPartPlacement Placements[] =
	{
		{ESwarmPartId::Torso, Torso.Points},
		{ESwarmPartId::LeftArm, LeftArm.Points}
	};

	if (!InitializeParticles(Placements, TorsoSettings.ParticleRadius))
	{
		return false;
	}

	Parts.FindChecked(ESwarmPartId::Torso).SamplingSettings = TorsoSettings;
	Parts.FindChecked(ESwarmPartId::LeftArm).SamplingSettings = LeftArmSettings;

	const double CenterDistance = Torso.HalfExtents.Y + LeftArm.HalfExtents.Y
		+ TorsoSettings.ParticleRadius + LeftArmSettings.ParticleRadius + PartGap;
	LeftArmInstances->SetRelativeLocation(FVector(0.0, -CenterDistance, 15.0));

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
// 두 번째 부위의 목표 개수는 정수로 버림하고 나머지는 첫 번째 부위에 배정한다. 비율은 모두 양수여야 한다.
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

	if (FirstPartId == SecondPartId || FirstWeight <= 0 || SecondWeight <= 0
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
