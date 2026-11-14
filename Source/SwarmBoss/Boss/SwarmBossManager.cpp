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

	// 도형 중심 기준 좌표로 Anchor 비율이 가리키는 경계 위 점을 반환한다. Anchor 각 축에 HalfExtents를 곱한다.
	// 구형은 HalfExtents.X가 반지름이며, 비율 점이 구 밖에 있으면 같은 방향의 구 표면으로 당긴다.
	FVector GetAnchorPoint(ESwarmPrimitive Shape, const FVector& Anchor, const FVector& HalfExtents)
	{
		FVector Point = Anchor * HalfExtents;

		if (Shape == ESwarmPrimitive::Sphere && Point.SizeSquared() > FMath::Square(HalfExtents.X))
		{
			Point = Point.GetSafeNormal() * HalfExtents.X;
		}

		return Point;
	}

	// Anchor가 놓인 면의 바깥 방향 단위 벡터를 반환한다. Gap과 구체 돌출 거리를 이 방향으로 띄운다.
	// 박스는 비율이 ±1인 축들이 면을 정한다(모서리면 그 축들의 합 방향). 면 위가 아니거나 구형이면 중심에서 Anchor로 향하는 방향이다.
	// Anchor가 0이면 방향이 없으므로 0 벡터를 반환해 띄우지 않는다.
	FVector GetAnchorNormal(ESwarmPrimitive Shape, const FVector& Anchor)
	{
		if (Shape == ESwarmPrimitive::Box)
		{
			FVector FaceNormal = FVector::ZeroVector;

			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (FMath::Abs(Anchor[Axis]) >= 1.0 - KINDA_SMALL_NUMBER)
				{
					FaceNormal[Axis] = FMath::Sign(Anchor[Axis]);
				}
			}

			if (!FaceNormal.IsNearlyZero())
			{
				return FaceNormal.GetSafeNormal();
			}
		}

		return Anchor.GetSafeNormal();
	}

	// 부모 ISM 기준 좌표로 자식 부위 중심 위치를 계산한다.
	// 부모 연결점에서 면 바깥 방향으로 (Gap + 두 부위의 구체 반지름)만큼 띄운 점에 자식 연결점을 맞춘다.
	// 반지름을 더하는 이유는 도형 경계가 구체 중심 허용 영역이라 구체가 반지름만큼 밖으로 튀어나오기 때문이다.
	FVector ComputeChildLocation(const FSwarmPartDefinition& Child, const FVector& ChildHalfExtents, double ChildRadius,
		ESwarmPrimitive ParentShape, const FVector& ParentHalfExtents, double ParentRadius)
	{
		const FVector ParentPoint = GetAnchorPoint(ParentShape, Child.ParentAnchor, ParentHalfExtents);
		const FVector Normal = GetAnchorNormal(ParentShape, Child.ParentAnchor);
		const FVector ChildPoint = Child.RelativeRotation.RotateVector(
			GetAnchorPoint(Child.Shape, Child.ChildAnchor, ChildHalfExtents));

		return ParentPoint + Normal * (Child.Gap + ParentRadius + ChildRadius) - ChildPoint;
	}

	// 부위 기준 → 루트 기준 변환 PartToRoot로 놓인 부위가 차지하는 가장 낮은 Z(루트 기준)를 반환한다.
	// 도형 경계는 구체 중심 허용 영역이므로, 경계의 가장 낮은 점에서 구체 반지름만큼 더 내려간 곳이 구체 표면의 바닥이다.
	// 박스는 회전을 반영한 8개 꼭짓점 중 가장 낮은 점, 구는 중심에서 반지름만큼 아래 점을 경계의 바닥으로 쓴다.
	double GetLowestZ(ESwarmPrimitive Shape, const FVector& HalfExtents, double ParticleRadius, const FTransform& PartToRoot)
	{
		if (Shape == ESwarmPrimitive::Sphere)
		{
			return PartToRoot.GetLocation().Z - HalfExtents.X - ParticleRadius;
		}

		double LowestZ = TNumericLimits<double>::Max();

		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Sign((Corner & 1) ? 1.0 : -1.0, (Corner & 2) ? 1.0 : -1.0, (Corner & 4) ? 1.0 : -1.0);
			LowestZ = FMath::Min(LowestZ, PartToRoot.TransformPosition(Sign * HalfExtents).Z);
		}

		return LowestZ - ParticleRadius;
	}
}

// 부위 ISM을 붙일 루트 컴포넌트와 구체 메시를 준비하고, 이동 목표가 있을 때만 Actor Tick을 사용하도록 설정한다.
// 부위별 ISM은 생성자에서 만들지 않고 GenerateInitialParts()가 FormDefinition의 부위 목록에 맞춰 만든다.
ASwarmBossManager::ASwarmBossManager()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	USceneComponent* BossRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BossRoot"));
	SetRootComponent(BossRoot);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	if (Sphere.Succeeded())
	{
		SphereMesh = Sphere.Object;
	}
}

// PartId 부위의 ISM을 반환한다. 아직 생성하지 않았거나 마지막 생성 데이터에 없는 부위이면 nullptr를 반환한다.
UInstancedStaticMeshComponent* ASwarmBossManager::GetPartInstances(ESwarmPartId PartId) const
{
	const FSwarmPart* Part = Parts.Find(PartId);

	return Part ? Part->Instances.Get() : nullptr;
}

// Definition의 부위 목록과 Parts의 ISM 구성을 일치시킨다.
// 목록에 없는 부위의 ISM은 파괴하고 Parts에서 제거하며, 목록에 새로 생긴 부위는 루트에 붙인 ISM을 만들어 등록한다.
// 이미 있는 부위의 ISM은 재사용한다. 인스턴스와 대응표는 비우지 않으므로 호출 직후 InitializeParticles()로 다시 채워야 한다.
void ASwarmBossManager::SyncPartInstances(const USwarmFormDefinition& Definition)
{
	TSet<ESwarmPartId> DefinedPartIds;

	for (const FSwarmPartDefinition& PartDefinition : Definition.Parts)
	{
		DefinedPartIds.Add(PartDefinition.PartId);
	}

	for (auto It = Parts.CreateIterator(); It; ++It)
	{
		if (DefinedPartIds.Contains(It.Key()))
		{
			continue;
		}

		if (IsValid(It.Value().Instances))
		{
			It.Value().Instances->DestroyComponent();
		}

		It.RemoveCurrent();
	}

	for (const ESwarmPartId PartId : DefinedPartIds)
	{
		FSwarmPart& Part = Parts.FindOrAdd(PartId);

		if (IsValid(Part.Instances))
		{
			continue;
		}

		// 컴포넌트 이름은 "부위명Instances"(예: TorsoInstances)이며, 파괴 직후 같은 이름이 남아 있으면 번호를 붙인다.
		const FString BaseName = StaticEnum<ESwarmPartId>()->GetNameStringByValue(static_cast<int64>(PartId)) + TEXT("Instances");
		FName ComponentName(*BaseName);

		if (StaticFindObjectFast(nullptr, this, ComponentName))
		{
			ComponentName = MakeUniqueObjectName(this, UInstancedStaticMeshComponent::StaticClass(), ComponentName);
		}

		UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(this, ComponentName);

		Instances->SetupAttachment(GetRootComponent());
		Instances->SetMobility(EComponentMobility::Movable);
		Instances->SetCanEverAffectNavigation(false);
		Instances->SetCollisionProfileName(TEXT("SwarmSpherePreset"));
		Instances->SetStaticMesh(SphereMesh);
		Instances->RegisterComponent();
		AddInstanceComponent(Instances);

		Part.Instances = Instances;
	}

	// 부모가 바뀐 경우 일시적인 순환 부착을 피하려고 모든 부위 ISM을 먼저 루트에 붙인 뒤 데이터의 부모 ISM에 다시 붙인다.
	// 최상위 부위는 Manager 루트에, 나머지는 부모 부위 ISM에 붙여 부모가 움직이거나 회전하면 자식이 따라가게 한다.
	for (TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
	{
		if (Pair.Value.Instances->GetAttachParent() != GetRootComponent())
		{
			Pair.Value.Instances->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		}
	}

	for (const FSwarmPartDefinition& PartDefinition : Definition.Parts)
	{
		FSwarmPart& Part = Parts.FindChecked(PartDefinition.PartId);
		Part.Definition = PartDefinition;

		if (PartDefinition.ParentPartId != ESwarmPartId::None)
		{
			Part.Instances->AttachToComponent(Parts.FindChecked(PartDefinition.ParentPartId).Instances,
				FAttachmentTransformRules::KeepRelativeTransform);
		}
	}
}

// 각 부위 ISM을 부모 ISM 기준 위치·회전에 놓는다. 최상위 부위는 데이터의 회전으로 놓고,
// 구체가 있는 모든 부위 중 가장 낮은 구체 표면이 Manager 루트 높이(Z=0)에 오도록 최상위 부위의 높이만 올린다.
// 즉 Manager 위치가 거인의 발바닥이며, 다리가 길어지거나 짧아지면 몸 전체의 높이가 따라 바뀐다.
// 바닥은 실제 남은 구체가 아니라 현재 도형 범위로 잡으므로, 피격으로는 높이가 바뀌지 않고 재분배·생성 때만 바뀐다.
// 위치는 Parts의 현재 HalfExtents·구체 반지름과 생성 당시 부위 정의의 연결점·Gap으로 계산하므로, 도형 크기가 바뀐 뒤 호출하면 관절이 표면을 따라간다.
// bPreserveParticleWorldPositions가 true이면 ISM이 움직인 부위의 활성 구체를 같은 월드 위치에 남기도록 로컬 좌표를 다시 계산하고,
// 기존 로컬 목표로 ReconstructionDuration초 동안 다시 이동시킨다. 초기 생성처럼 구체 좌표가 새 배치 기준이면 false로 호출한다.
void ASwarmBossManager::ApplyPartPlacement(bool bPreserveParticleWorldPositions)
{
	TMap<ESwarmPartId, FTransform> OldWorldTransforms;

	if (bPreserveParticleWorldPositions)
	{
		for (const TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
		{
			OldWorldTransforms.Add(Pair.Key, Pair.Value.Instances->GetComponentTransform());
		}
	}

	for (TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
	{
		FSwarmPart& Part = Pair.Value;
		const FSwarmPartDefinition& Definition = Part.Definition;
		FVector Location = FVector::ZeroVector;

		if (Definition.ParentPartId != ESwarmPartId::None)
		{
			const FSwarmPart& Parent = Parts.FindChecked(Definition.ParentPartId);
			Location = ComputeChildLocation(Definition, Part.HalfExtents, Part.SamplingSettings.ParticleRadius,
				Parent.Definition.Shape, Parent.HalfExtents, Parent.SamplingSettings.ParticleRadius);
		}

		Part.Instances->SetRelativeLocationAndRotation(Location, Definition.RelativeRotation);
	}

	// 최상위 부위가 루트 중심에 있는 상태에서 바닥을 재고, 그만큼 최상위 부위를 올린다. 자식 ISM은 붙어 있어 함께 올라간다.
	const FTransform RootTransform = GetRootComponent()->GetComponentTransform();
	double LowestZ = TNumericLimits<double>::Max();
	FSwarmPart* RootPart = nullptr;

	for (TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
	{
		FSwarmPart& Part = Pair.Value;

		if (Part.Definition.ParentPartId == ESwarmPartId::None)
		{
			RootPart = &Part;
		}

		// 목표 구체가 0개인 부위는 HalfExtents가 0이고 화면에 아무것도 없으므로 바닥 계산에서 뺀다.
		if (Part.HalfExtents.IsZero())
		{
			continue;
		}

		const FTransform PartToRoot = Part.Instances->GetComponentTransform().GetRelativeTransform(RootTransform);
		LowestZ = FMath::Min(LowestZ, GetLowestZ(Part.Definition.Shape, Part.HalfExtents,
			Part.SamplingSettings.ParticleRadius, PartToRoot));
	}

	if (RootPart && LowestZ < TNumericLimits<double>::Max())
	{
		RootPart->Instances->SetRelativeLocation(FVector(0.0, 0.0, -LowestZ));
	}

	if (!bPreserveParticleWorldPositions)
	{
		return;
	}

	TArray<UInstancedStaticMeshComponent*, TInlineAllocator<16>> ChangedInstances;
	const TArray<FSwarmParticle>& Particles = ParticleStore.GetParticles();

	for (int32 ParticleId = 0; ParticleId < Particles.Num(); ++ParticleId)
	{
		const FSwarmParticle Particle = Particles[ParticleId];
		const FSwarmPart* Part = Parts.Find(Particle.PartId);
		const FTransform* OldTransform = OldWorldTransforms.Find(Particle.PartId);

		if (!Particle.bActive || !Part || !OldTransform)
		{
			continue;
		}

		UInstancedStaticMeshComponent* Instances = Part->Instances.Get();
		const FTransform& NewTransform = Instances->GetComponentTransform();

		if (OldTransform->Equals(NewTransform))
		{
			continue;
		}

		// 이전 ISM 기준 로컬 → 월드 → 새 ISM 기준 로컬로 바꿔 화면 위치를 유지한다.
		const FVector NewLocalPosition = NewTransform.InverseTransformPosition(
			OldTransform->TransformPosition(Particle.LocalPosition));
		FTransform InstanceTransform;

		if (!Instances->GetInstanceTransform(Particle.InstanceIndex, InstanceTransform, /*bWorldSpace=*/false))
		{
			continue;
		}

		InstanceTransform.SetLocation(NewLocalPosition);

		if (!Instances->UpdateInstanceTransform(Particle.InstanceIndex, InstanceTransform,
			/*bWorldSpace=*/false, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true))
		{
			continue;
		}

		ParticleStore.UpdateMovementProgress(ParticleId, NewLocalPosition, Particle.RemainingMoveTime);
		AssignParticleTarget(ParticleId, Particle.TargetLocalPosition, ReconstructionDuration);
		ChangedInstances.AddUnique(Instances);
	}

	for (UInstancedStaticMeshComponent* Instances : ChangedInstances)
	{
		Instances->MarkRenderStateDirty();
	}
}

// Play 시작 시 GenerateInitialParts()를 호출해 몸통과 왼팔의 구체를 생성한다.
void ASwarmBossManager::BeginPlay()
{
	Super::BeginPlay();

	GenerateInitialParts();
}

// FormDefinition의 부위 목록에 따라 InitialParticleCount를 가중치 비율로 나누고, 부위마다 도형 중심 기준의 구체 좌표를 계산한다.
// 정수 배분의 나머지는 RemainderPartId 부위가 받는다. 초기 설정값은 생존 수와 별도로 유지한다.
// 모든 부위의 계산이 성공하면 부위 ISM을 데이터 목록·부모 관계에 맞춰 만들거나 제거하고 다시 붙인 뒤 구체를 생성한다.
// 마지막으로 생성된 도형 크기와 데이터의 연결점·Gap·회전으로 각 부위 ISM을 부모 ISM 기준 위치에 놓는다.
// 성공한 설정은 부위별로 보관한다. 데이터 누락·잘못된 설정·생성 실패는 기존 상태를 유지하고 false를 반환한다.
bool ASwarmBossManager::GenerateInitialParts()
{
	if (InitialParticleCount < 0 || InitialParticleCount > 10000)
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("InitialParticleCount must be between 0 and 10000."));
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

	// 샘플링이 모두 성공한 뒤에 ISM 구성을 데이터에 맞춘다. 이후 InitializeParticles()가 모든 부위를 다시 채운다.
	SyncPartInstances(*FormDefinition);

	if (!InitializeParticles(Placements, TorsoSettings.ParticleRadius))
	{
		return false;
	}

	for (const TPair<ESwarmPartId, FSwarmSamplingSettings>& Pair : PartSettings)
	{
		FSwarmPart& Part = Parts.FindChecked(Pair.Key);
		Part.SamplingSettings = Pair.Value;
		Part.HalfExtents = PartResults.FindChecked(Pair.Key).HalfExtents;
	}

	// 새로 만든 구체 좌표는 이미 각 부위 ISM 기준이므로 월드 위치 보존 없이 부위 위치만 정한다.
	ApplyPartPlacement(/*bPreserveParticleWorldPositions=*/false);

	return true;
}

// 초기 생성에 사용할 형태 데이터를 교체한다. 유효성은 다음 GenerateInitialParts() 호출 때 검사한다.
void ASwarmBossManager::SetFormDefinition(USwarmFormDefinition* NewFormDefinition)
{
	FormDefinition = NewFormDefinition;
}

// Definition을 이 Manager가 생성할 수 있는지 검사한다. 문제가 있으면 첫 번째 원인을 경고 로그로 남기고 false를 반환한다.
// 검사 항목: 부위 ID 유일성과 None/Preview 금지, 가중치가 0 이상이고 합이 0보다 큰지, 종횡비 유효성, 연결점이 -1~1인지, Gap이 0 이상인지, 회전 유효성, 부모가 같은 목록 안에 있는지,
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

		if (Part.AllocationWeight < 0)
		{
			return Fail(FString::Printf(TEXT("%s has a negative allocation weight."), *PartName));
		}

		if (Part.BoxHalfExtentsRatio.ContainsNaN() || Part.BoxHalfExtentsRatio.GetMin() <= 0.0)
		{
			return Fail(FString::Printf(TEXT("%s box ratio must be finite and positive."), *PartName));
		}

		if (Part.ParentAnchor.ContainsNaN() || Part.ChildAnchor.ContainsNaN()
			|| Part.ParentAnchor.GetAbsMax() > 1.0 || Part.ChildAnchor.GetAbsMax() > 1.0)
		{
			return Fail(FString::Printf(TEXT("%s anchors must be finite and within -1 to 1."), *PartName));
		}

		if (!FMath::IsFinite(Part.Gap) || Part.Gap < 0.0f)
		{
			return Fail(FString::Printf(TEXT("%s gap must be finite and non-negative."), *PartName));
		}

		if (Part.RelativeRotation.ContainsNaN())
		{
			return Fail(FString::Printf(TEXT("%s relative rotation must be finite."), *PartName));
		}

		if (Part.ParentPartId == Part.PartId)
		{
			return Fail(FString::Printf(TEXT("%s cannot be its own parent."), *PartName));
		}

		ParentOf.Add(Part.PartId, Part.ParentPartId);
		WeightSum += Part.AllocationWeight;
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

// 두 부위의 현재 생존 수 합계를 FirstWeight:SecondWeight로 나눠 RebalanceToCounts()로 맞춘다.
// 두 번째 부위의 목표 개수는 정수로 버림하고 나머지는 첫 번째 부위에 배정한다.
// 가중치는 0 이상이고 합이 양수여야 한다. 가중치 0인 부위는 구체를 모두 다른 부위로 보낸다.
// 부위가 같거나 없거나 가중치가 잘못되면 상태를 바꾸지 않고 false를 반환한다. 그 밖의 동작과 반환값은 RebalanceToCounts()와 같다.
bool ASwarmBossManager::RebalanceParts(ESwarmPartId FirstPartId, ESwarmPartId SecondPartId,
	int32 FirstWeight, int32 SecondWeight)
{
	const FSwarmPart* FirstPart = Parts.Find(FirstPartId);
	const FSwarmPart* SecondPart = Parts.Find(SecondPartId);

	if (FirstPartId == SecondPartId || FirstWeight < 0 || SecondWeight < 0 || int64(FirstWeight) + SecondWeight <= 0
		|| !FirstPart || !SecondPart)
	{
		return false;
	}

	const int32 TotalCount = FirstPart->InstanceToParticle.Num() + SecondPart->InstanceToParticle.Num();
	const int32 SecondTargetCount = int32(int64(TotalCount) * SecondWeight / (int64(FirstWeight) + SecondWeight));
	const TPair<ESwarmPartId, int32> TargetCounts[] =
	{
		{FirstPartId, TotalCount - SecondTargetCount},
		{SecondPartId, SecondTargetCount}
	};

	return RebalanceToCounts(TargetCounts);
}

// PartTargetCounts에 적힌 각 부위의 생존 구체 수를 목표 개수로 맞춘다. 목록에 없는 부위는 구체 수를 바꾸지 않는다.
// 목표 개수 합은 목록 부위의 현재 개수 합과 같아야 하며, 부위가 중복되거나 없거나 둘 미만이면 상태를 바꾸지 않고 false를 반환한다.
// 1) 각 부위의 초기 생성 설정에서 개수만 바꿔 새 목표 좌표를 생성한다. 하나라도 실패하면 상태를 바꾸지 않는다.
// 2) 새 도형 크기로 모든 부위 ISM 위치를 다시 계산한다. 위치가 바뀐 부위의 구체는 화면 위치를 유지한 채 다시 이동한다.
// 3) 목록 순서대로 초과 부위에서 부족 부위로 필요한 수를 직접 보낸다. 부위 연결 관계는 따지지 않는다.
// 4) 각 부위의 생존 구체 전체에 새 목표를 순서대로 배정하고 ReconstructionDuration초 동안 이동시킨다.
// 재구성 시간이 유효한 양수가 아니면 거부한다. 모든 부위가 이미 목표 개수이면 아무것도 바꾸지 않고 true를 반환한다.
// 전달이 일부만 성공하면 성공한 변경은 유지하고 false를 반환한다. 관절 근처 선택은 하지 않으며 선택은 중앙 배열 순서를 사용한다.
bool ASwarmBossManager::RebalanceToCounts(TConstArrayView<TPair<ESwarmPartId, int32>> PartTargetCounts)
{
	if (!FMath::IsFinite(ReconstructionDuration) || ReconstructionDuration <= 0.0f || PartTargetCounts.Num() < 2)
	{
		return false;
	}

	const int32 PartCount = PartTargetCounts.Num();
	TArray<FSwarmPart*> TargetParts;
	TArray<int32> Remaining;
	TSet<ESwarmPartId> SeenPartIds;
	int64 CountDifference = 0;
	bool bAlreadyBalanced = true;

	for (const TPair<ESwarmPartId, int32>& Pair : PartTargetCounts)
	{
		FSwarmPart* Part = Parts.Find(Pair.Key);

		if (!Part || !IsValid(Part->Instances) || Pair.Value < 0 || SeenPartIds.Contains(Pair.Key))
		{
			return false;
		}

		// Remaining은 (현재 개수 - 목표 개수)다. 양수는 보내야 할 수, 음수는 받아야 할 수다.
		const int32 Surplus = Part->InstanceToParticle.Num() - Pair.Value;
		SeenPartIds.Add(Pair.Key);
		TargetParts.Add(Part);
		Remaining.Add(Surplus);
		CountDifference += Surplus;
		bAlreadyBalanced &= Surplus == 0;
	}

	if (CountDifference != 0)
	{
		return false;
	}

	if (bAlreadyBalanced)
	{
		UE_LOG(LogSwarmBossManager, Display, TEXT("Rebalance %s: already balanced, Parts=%d Total=%d"),
			*GetName(), PartCount, GetActiveParticleCount());
		return true;
	}

	TArray<FSwarmSamplingResult> Results;
	Results.SetNum(PartCount);

	for (int32 Index = 0; Index < PartCount; ++Index)
	{
		FSwarmSamplingSettings Settings = TargetParts[Index]->SamplingSettings;
		Settings.ParticleCount = PartTargetCounts[Index].Value;
		Results[Index] = FSwarmShapeSampler::GenerateTargets(Settings);

		if (!Results[Index].bSuccess)
		{
			UE_LOG(LogSwarmBossManager, Warning, TEXT("Rebalance sampling failed for %s: %s"),
				*UEnum::GetValueAsString(PartTargetCounts[Index].Key), *Results[Index].Message);
			return false;
		}
	}

	// 새 목표의 도형 크기로 관절 위치를 다시 계산한다. 구체는 현재 화면 위치에 남기므로 순간 이동하지 않는다.
	// 이후 전달과 목표 배정은 옮겨진 부위 ISM 기준으로 수행한다.
	for (int32 Index = 0; Index < PartCount; ++Index)
	{
		TargetParts[Index]->HalfExtents = Results[Index].HalfExtents;
	}

	ApplyPartPlacement(/*bPreserveParticleWorldPositions=*/true);

	// 앞에서부터 초과 부위마다 아직 부족한 부위를 차례로 채운다. 받는 부위의 임시 목표는 새 목표 중 아직 쓰지 않은 구간을 쓴다.
	int32 DestinationIndex = 0;

	for (int32 SourceIndex = 0; SourceIndex < PartCount; ++SourceIndex)
	{
		while (Remaining[SourceIndex] > 0)
		{
			while (DestinationIndex < PartCount && Remaining[DestinationIndex] >= 0)
			{
				++DestinationIndex;
			}

			check(DestinationIndex < PartCount);

			const int32 RequiredCount = FMath::Min(Remaining[SourceIndex], -Remaining[DestinationIndex]);
			const int32 DestinationCurrentCount = PartTargetCounts[DestinationIndex].Value + Remaining[DestinationIndex];
			const TConstArrayView<FVector> TransferTargets =
				MakeArrayView(Results[DestinationIndex].Points).Slice(DestinationCurrentCount, RequiredCount);
			const ESwarmPartId SourcePartId = PartTargetCounts[SourceIndex].Key;
			const ESwarmPartId DestinationPartId = PartTargetCounts[DestinationIndex].Key;
			const int32 TransferredCount = TransferParticlesToTargets(SourcePartId, DestinationPartId, TransferTargets);

			UE_LOG(LogSwarmBossManager, Display, TEXT("Rebalance %s: %s -> %s Requested=%d Transferred=%d Total=%d"),
				*GetName(), *UEnum::GetValueAsString(SourcePartId), *UEnum::GetValueAsString(DestinationPartId),
				RequiredCount, TransferredCount, GetActiveParticleCount());

			if (TransferredCount != RequiredCount)
			{
				return false;
			}

			Remaining[SourceIndex] -= RequiredCount;
			Remaining[DestinationIndex] += RequiredCount;
		}
	}

	// 전달한 구체뿐 아니라 각 부위의 생존 구체 전체에 새 목표를 하나씩 배정해 최종 배치가 겹치지 않게 한다.
	for (int32 Index = 0; Index < PartCount; ++Index)
	{
		const int32 TargetCount = PartTargetCounts[Index].Value;
		const TArray<int32> ParticleIds = SelectPartParticleIds(PartTargetCounts[Index].Key, TargetCount);

		if (ParticleIds.Num() != TargetCount)
		{
			return false;
		}

		for (int32 ParticleIndex = 0; ParticleIndex < ParticleIds.Num(); ++ParticleIndex)
		{
			if (!AssignParticleTarget(ParticleIds[ParticleIndex], Results[Index].Points[ParticleIndex], ReconstructionDuration))
			{
				return false;
			}
		}
	}

	return true;
}

// T 입력 시점의 FormDefinition 가중치로 생성된 모든 부위의 생존 구체를 재분배한다.
// 현재 생존 수 합계를 초기 생성과 같은 규칙(가중치 비율 버림, 나머지는 RemainderPartId)으로 나눈 뒤 RebalanceToCounts()로 맞춘다.
// 가중치만 다시 읽으며, 도형 종류·종횡비는 마지막 초기 생성 때 보관한 설정을 그대로 사용한다.
// 부위 사이 이동은 연결 관계와 상관없이 초과 부위에서 부족 부위로 직접 보낸다.
// 데이터가 없거나 유효하지 않거나, 생성된 부위와 목록이 다르면 경고를 남기고 상태를 바꾸지 않은 채 false를 반환한다.
// TODO: 필요성이 확인되면 이웃 부위끼리만 전달하도록 연결 관계를 따르는 방식으로 교체한다.
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

	// 생성 후 데이터의 부위 목록이 바뀌었다면 ISM 구성과 맞지 않으므로 다시 생성하기 전까지 재분배하지 않는다.
	bool bPartsMatch = FormDefinition->Parts.Num() == Parts.Num();

	for (const FSwarmPartDefinition& Part : FormDefinition->Parts)
	{
		bPartsMatch &= Parts.Contains(Part.PartId);
	}

	if (!bPartsMatch)
	{
		UE_LOG(LogSwarmBossManager, Warning,
			TEXT("FormDefinition %s parts differ from the generated parts of %s. Regenerate before rebalancing."),
			*FormDefinition->GetName(), *GetName());
		return false;
	}

	int32 TotalCount = 0;

	for (const FSwarmPartDefinition& Part : FormDefinition->Parts)
	{
		TotalCount += Parts.FindChecked(Part.PartId).InstanceToParticle.Num();
	}

	const TMap<ESwarmPartId, int32> PartCounts = AllocatePartCounts(*FormDefinition, TotalCount);
	TArray<TPair<ESwarmPartId, int32>> TargetCounts;
	TargetCounts.Reserve(FormDefinition->Parts.Num());

	for (const FSwarmPartDefinition& Part : FormDefinition->Parts)
	{
		TargetCounts.Emplace(Part.PartId, PartCounts.FindChecked(Part.PartId));
	}

	return RebalanceToCounts(TargetCounts);
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
