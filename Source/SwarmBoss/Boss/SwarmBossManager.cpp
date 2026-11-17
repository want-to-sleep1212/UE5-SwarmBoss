#include "SwarmBossManager.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Boss/SwarmFormDefinition.h"
#include "Boss/SwarmTransferPlanner.h"
#include "Swarm/SwarmShapeSampler.h"
#include "Swarm/SwarmTargetAssigner.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
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

	// 자식 부위 로컬에서 관절 피벗 점. ChildAnchor에서 X·Y를 0으로 둔 (0, 0, ChildAnchor.Z) 비율이다.
	// 거인 부위는 모두 자기 Z축 방향으로 길고 위·아래 끝에서 이어지므로, 팔처럼 옆면 위 모서리(0, ±1, 1)로 붙어도
	// 실제 관절처럼 윗면 가운데에서 돈다(사용자 결정, 2026-11-17). 검은 관절 없이 통째로 움직이므로 해당 없다.
	// 위치 계산은 GetAnchorPoint()와 같다. 구에서는 구 밖으로 나간 점만 표면으로 당기며 안쪽 점은 그대로 둔다.
	FVector GetPivotPoint(const FSwarmPartDefinition& Definition, const FVector& HalfExtents)
	{
		return GetAnchorPoint(Definition.Shape, FVector(0.0, 0.0, Definition.ChildAnchor.Z), HalfExtents);
	}

	// 부모 ISM 기준 자식 ISM의 상대 변환. 기본 위치 P0(ComputeChildLocation)·기본 회전 R0(RelativeRotation)에서
	// 피벗 J = P0 + R0·피벗점을 중심으로 Pose만큼 돌린다: 위치 = J + Pose·(P0 - J), 회전 = Pose * R0.
	// FQuat 곱 A * B는 B를 먼저 적용하므로 Pose * R0는 기본 회전 뒤에 부모 축 기준 자세를 더한 것이다. Pose가 회전 없음이면 기본 배치와 같다.
	FTransform ComputeChildRelativeTransform(const FSwarmPartDefinition& Child, const FVector& ChildHalfExtents,
		double ChildRadius, ESwarmPrimitive ParentShape, const FVector& ParentHalfExtents, double ParentRadius,
		const FQuat& Pose)
	{
		const FVector RestLocation = ComputeChildLocation(Child, ChildHalfExtents, ChildRadius, ParentShape,
			ParentHalfExtents, ParentRadius);
		const FQuat RestRotation = Child.RelativeRotation.Quaternion();
		const FVector Pivot = RestLocation + RestRotation.RotateVector(GetPivotPoint(Child, ChildHalfExtents));

		return FTransform(Pose * RestRotation, Pivot + Pose.RotateVector(RestLocation - Pivot));
	}

	// 자세가 쓸 수 있는 회전인지(NaN·무한대가 아니고 정규화할 수 있는지) 확인한다.
	bool IsValidPose(const FQuat& Pose)
	{
		return !Pose.ContainsNaN() && FMath::IsFinite(Pose.X) && FMath::IsFinite(Pose.Y) && FMath::IsFinite(Pose.Z)
			&& FMath::IsFinite(Pose.W) && Pose.SizeSquared() > UE_SMALL_NUMBER;
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

// 부위별 샘플링 시드. 샘플러 기본 시드에 PartId마다 다른 값을 더해, 도형·비율·개수가 같은 좌우 부위도 서로 다른 배치가 나오게 한다.
// 같은 부위는 항상 같은 시드이므로 같은 개수면 같은 배치를 재현한다(재분배 시 개수가 그대로인 부위는 움직이지 않는다).
int32 ASwarmBossManager::GetPartSamplingSeed(ESwarmPartId PartId)
{
	return int32(uint32(FSwarmSamplingSettings().Seed) + uint32(PartId) * 7919u);
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
// 이때 ISM 인스턴스는 직접 고치지 않고 중앙 데이터의 좌표만 바꾼다. 다시 이동하는 구체는 같은 Tick의 UpdateParticleMovement()가
// 새 좌표로 인스턴스를 쓰므로, 여기서도 쓰면 한 프레임에 같은 인스턴스를 두 번 쓰게 된다(인스턴스마다 렌더·충돌 바디 갱신 비용이
// 있어 구체 2000개에서 약 26ms, 2026-11-16 측정). 새 좌표가 곧 목표라 움직이지 않을 구체만 여기서 인스턴스를 쓴다.
// 자동 재분배는 Manager Tick 안에서 이동 갱신 직전에 실행되므로 화면이 어긋나지 않는다. T 입력처럼 Tick 밖에서 호출하면
// 그 프레임에 Manager Tick이 이미 지났을 경우 한 프레임 동안 부위 이동량만큼 어긋나 보일 수 있다.
void ASwarmBossManager::ApplyPartPlacement(bool bPreserveParticleWorldPositions)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Swarm_ApplyPlacement);
	TMap<ESwarmPartId, FTransform> OldWorldTransforms;

	if (bPreserveParticleWorldPositions)
	{
		for (const TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
		{
			OldWorldTransforms.Add(Pair.Key, Pair.Value.Instances->GetComponentTransform());
		}
	}

	// 부모 ISM 기준 상대 변환을 부위 정의·현재 도형 크기·저장된 자세로 계산한다. bWithPose가 false면 자세를 빼고 기본 자세로 계산한다.
	auto ComputeRelative = [this](const FSwarmPart& Part, bool bWithPose)
	{
		const FSwarmPartDefinition& Definition = Part.Definition;

		if (Definition.ParentPartId == ESwarmPartId::None)
		{
			return FTransform(Definition.RelativeRotation.Quaternion());
		}

		const FSwarmPart& Parent = Parts.FindChecked(Definition.ParentPartId);
		return ComputeChildRelativeTransform(Definition, Part.HalfExtents, Part.SamplingSettings.ParticleRadius,
			Parent.Definition.Shape, Parent.HalfExtents, Parent.SamplingSettings.ParticleRadius,
			bWithPose ? Part.Pose : FQuat::Identity);
	};

	FSwarmPart* RootPart = nullptr;

	for (TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
	{
		FSwarmPart& Part = Pair.Value;
		const FTransform Relative = ComputeRelative(Part, /*bWithPose=*/true);
		Part.Instances->SetRelativeLocationAndRotation(Relative.GetLocation(), Relative.GetRotation());

		if (Part.Definition.ParentPartId == ESwarmPartId::None)
		{
			RootPart = &Part;
		}
	}

	// 바닥은 자세를 뺀 기본 자세로 계산해, 같은 도형 크기에서는 팔·다리를 돌려도 몸 높이가 바뀌지 않게 한다.
	// 실제 컴포넌트 위치를 읽지 않고, 부위마다 자세 없는 상대 변환을 부모 쪽으로 누적해 루트 기준 변환을 구한다.
	// - 모든 조상의 자세를 빼고 각 부위의 기본 회전(RelativeRotation)은 넣는다.
	// - 최상위 부위의 높이 보정은 넣지 않는다(최상위는 루트 중심·기본 회전만).
	// - TMap 순회 순서와 무관하게 부모를 따라 재귀로 누적한다. FTransform 곱 A * B는 A를 먼저 적용하므로 자식상대 * 부모→루트다.
	TMap<ESwarmPartId, FTransform> RestToRoot;
	TFunction<FTransform(ESwarmPartId)> GetRestToRoot = [&](ESwarmPartId PartId) -> FTransform
	{
		if (const FTransform* Cached = RestToRoot.Find(PartId))
		{
			return *Cached;
		}

		const FSwarmPart& Part = Parts.FindChecked(PartId);
		const FTransform Relative = ComputeRelative(Part, /*bWithPose=*/false);
		const FTransform ToRoot = Part.Definition.ParentPartId == ESwarmPartId::None
			? Relative : Relative * GetRestToRoot(Part.Definition.ParentPartId);

		RestToRoot.Add(PartId, ToRoot);
		return ToRoot;
	};

	double LowestZ = TNumericLimits<double>::Max();

	for (TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
	{
		const FSwarmPart& Part = Pair.Value;

		// 목표 구체가 0개인 부위는 HalfExtents가 0이고 화면에 아무것도 없으므로 바닥 계산에서 뺀다.
		if (Part.HalfExtents.IsZero())
		{
			continue;
		}

		LowestZ = FMath::Min(LowestZ, GetLowestZ(Part.Definition.Shape, Part.HalfExtents,
			Part.SamplingSettings.ParticleRadius, GetRestToRoot(Pair.Key)));
	}

	// 가장 낮은 기본 자세 구체 표면이 Manager 루트 높이(Z=0)에 오도록 최상위 부위만 올린다. 자식 ISM은 붙어 있어 함께 올라간다.
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

		// 새 좌표가 곧 목표면 이동 갱신이 이 구체를 건너뛰므로 여기서 인스턴스를 직접 쓴다(드묾).
		if (NewLocalPosition == Particle.TargetLocalPosition)
		{
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

			ChangedInstances.AddUnique(Instances);
		}

		ParticleStore.UpdateMovementProgress(ParticleId, NewLocalPosition, Particle.RemainingMoveTime);
		AssignParticleTarget(ParticleId, Particle.TargetLocalPosition, ReconstructionDuration);
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
		Settings.PackingCoefficient = PackingCoefficient;
		Settings.Seed = GetPartSamplingSeed(PartDefinition.PartId);

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

	// 전체를 다시 만들 때는 자세도 초기화한다. SyncPartInstances()는 같은 부위의 FSwarmPart를 재사용하므로 이전 자세가 남는다.
	for (TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
	{
		Pair.Value.Pose = FQuat::Identity;
	}

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
// 재분배의 부위 간 전달은 관절에 가까운 구체를 고르는 SelectParticlesNearest()를 사용한다.
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

// 이웃한 두 부위 사이 관절의 월드 위치를 반환한다. 순서는 상관없으며, 둘 중 한쪽이 다른 쪽의 부모여야 한다.
// 관절 위치는 부모 연결점과 자식 연결점(둘 다 현재 도형 크기 기준 경계 위 점)의 가운데, 즉 Gap 사이의 중간이다.
// 이웃이 아니거나 부위가 없으면 첫 번째 부위의 ISM 위치를 반환한다.
FVector ASwarmBossManager::GetJointWorldLocation(ESwarmPartId FirstPartId, ESwarmPartId SecondPartId) const
{
	const FSwarmPart* First = Parts.Find(FirstPartId);
	const FSwarmPart* Second = Parts.Find(SecondPartId);

	if (!First || !Second || !IsValid(First->Instances) || !IsValid(Second->Instances))
	{
		return First && IsValid(First->Instances) ? First->Instances->GetComponentLocation() : GetActorLocation();
	}

	const FSwarmPart* Child = First->Definition.ParentPartId == SecondPartId ? First
		: Second->Definition.ParentPartId == FirstPartId ? Second : nullptr;

	if (!Child)
	{
		return First->Instances->GetComponentLocation();
	}

	const FSwarmPart* Parent = Child == First ? Second : First;
	const FSwarmPartDefinition& Definition = Child->Definition;
	const FVector ParentPoint = Parent->Instances->GetComponentTransform().TransformPosition(
		GetAnchorPoint(Parent->Definition.Shape, Definition.ParentAnchor, Parent->HalfExtents));
	const FVector ChildPoint = Child->Instances->GetComponentTransform().TransformPosition(
		GetAnchorPoint(Definition.Shape, Definition.ChildAnchor, Child->HalfExtents));

	return 0.5 * (ParentPoint + ChildPoint);
}

// PartId 부위의 활성 구체 중 현재 월드 위치가 WorldPoint에 가까운 순서로 최대 MaxCount개를 골라 ParticleId 목록을 반환한다.
// ExcludedParticleIds에 있는 구체는 고르지 않는다. 거리가 같으면 ParticleId가 작은 쪽을 먼저 고른다.
// 공간 자료구조 없이 부위 구체 전체를 정렬한다. 재분배처럼 가끔 호출하는 용도이며, 매 프레임 호출하지 않는다.
TArray<int32> ASwarmBossManager::SelectParticlesNearest(ESwarmPartId PartId, const FVector& WorldPoint, int32 MaxCount,
	const TSet<int32>& ExcludedParticleIds) const
{
	TArray<int32> SelectedParticleIds;
	const FSwarmPart* Part = Parts.Find(PartId);

	if (MaxCount <= 0 || !Part || !IsValid(Part->Instances))
	{
		return SelectedParticleIds;
	}

	const FTransform PartTransform = Part->Instances->GetComponentTransform();
	const TArray<FSwarmParticle>& Particles = ParticleStore.GetParticles();
	TArray<TPair<double, int32>> Candidates;
	Candidates.Reserve(Part->InstanceToParticle.Num());

	for (int32 ParticleId : Part->InstanceToParticle)
	{
		if (Particles.IsValidIndex(ParticleId) && Particles[ParticleId].bActive && !ExcludedParticleIds.Contains(ParticleId))
		{
			const FVector WorldPosition = PartTransform.TransformPosition(Particles[ParticleId].LocalPosition);
			Candidates.Emplace(FVector::DistSquared(WorldPosition, WorldPoint), ParticleId);
		}
	}

	Candidates.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B)
	{
		return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
	});

	const int32 SelectedCount = FMath::Min(MaxCount, Candidates.Num());
	SelectedParticleIds.Reserve(SelectedCount);

	for (int32 Index = 0; Index < SelectedCount; ++Index)
	{
		SelectedParticleIds.Add(Candidates[Index].Value);
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
	bRebalancePending = false;

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

// 투사체 Sweep처럼 한 번에 여러 구체를 맞힌 결과에서 이 Manager의 구체를 모두 제거하고 제거한 수를 반환한다.
// 먼저 맞은 (부위 ISM, 인스턴스 번호)를 ParticleId로 바꿔 모은 뒤 하나씩 제거한다. 교환 삭제로 다른 구체의 인스턴스 번호가
// 바뀌므로, 인스턴스 번호를 미리 모아 두지 않고 제거할 때마다 ParticleId로 현재 번호를 다시 읽는다.
// 다른 Actor·컴포넌트의 결과, 잘못된 인스턴스 번호, 같은 구체의 중복 결과는 건너뛴다.
int32 ASwarmBossManager::RemoveHitInstances(TConstArrayView<FHitResult> Hits)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Swarm_RemoveHits);
	TArray<int32> HitParticleIds;

	for (const FHitResult& Hit : Hits)
	{
		const FSwarmPart* Part = Hit.GetActor() == this ? FindPartByComponent(Hit.GetComponent()) : nullptr;

		if (Part && Part->InstanceToParticle.IsValidIndex(Hit.Item))
		{
			HitParticleIds.AddUnique(Part->InstanceToParticle[Hit.Item]);
		}
	}

	int32 RemovedCount = 0;

	for (int32 ParticleId : HitParticleIds)
	{
		const FSwarmParticle& Particle = ParticleStore.GetParticles()[ParticleId];
		const FSwarmPart* Part = Parts.Find(Particle.PartId);

		if (Particle.bActive && Part && RemoveInstance(Part->Instances, Particle.InstanceIndex))
		{
			++RemovedCount;
		}
	}

	return RemovedCount;
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

	// 관통 한 발에 수십 번 불리므로 기본 출력에서는 숨긴다(Verbose). 에디터 로그에 수십 줄씩 쓰는 비용도 프레임 튐에 더해진다.
	UE_LOG(LogSwarmBossManager, Verbose,
		TEXT("Removed Instance=%d Remaining=%d"),
		InstanceIndex,
		GetActiveParticleCount());

	// 손실 자동 재분배는 여기서 바로 하지 않고 Tick으로 미룬다. 같은 프레임의 여러 제거와
	// 피해 구간(관통 투사체 한 발 등)이 끝날 때까지의 제거를 한 번의 재분배로 묶기 위해서다.
	if (bAutoRebalanceOnLoss)
	{
		bRebalancePending = true;
		SetActorTickEnabled(true);
	}

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

	// 위치는 인스턴스가 아니라 중앙 데이터에서 읽는다. 재분배의 관절 재배치는 중앙 좌표만 고치고 인스턴스는 다음 이동 갱신에서
	// 쓰므로, 같은 재분배 안에서 이어지는 전달 시점에는 인스턴스 위치가 아직 이전 값일 수 있다(회전·크기는 인스턴스 값을 쓴다).
	WorldTransform.SetLocation(SourceInstances->GetComponentTransform().TransformPosition(Particle.LocalPosition));

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
// 1) 개수가 바뀌는 부위만 초기 생성 설정에서 개수만 바꿔 새 목표 좌표를 생성한다. 하나라도 실패하면 상태를 바꾸지 않는다.
//    개수가 그대로인 부위는 같은 시드·개수면 같은 배치가 나오므로 도형 크기도 그대로다.
// 2) 새 도형 크기로 모든 부위 ISM 위치를 다시 계산한다. 위치가 바뀐 부위의 구체는 화면 위치를 유지한 채 다시 이동한다.
// 3) 부위 트리에서 관절마다 지나갈 수를 계산해 이웃 부위끼리만 보낸다(FSwarmTransferPlanner). 모든 관절의 전달은 동시에 출발한다.
//    보낼 부위가 가진 수보다 많이 넘겨야 하면, 그 부위로 들어오는 구체 일부가 길목을 건너뛰어 다음 부위로 바로 간다.
//    보낼 구체는 첫 관절에 가장 가까운 것부터 고른다.
// 4) 개수가 바뀌었거나 전달로 구체가 드나든 부위마다, 생존 구체 전체를 새 목표에 가까운 것끼리 짝지어(FSwarmTargetAssigner)
//    ReconstructionDuration초 동안 이동시킨다. 원래 있던 구체는 현재 위치, 이번에 들어온 구체는 받는 부위 도형 표면의
//    연결점에서 출발한다고 본다. 그리디 뒤 남은 먼 이동은 도미노로 기준 이내 사슬로 고친다(FSwarmTargetAssigner).
//    개수가 그대로이고 드나든 구체도 없는 부위는 목표를 바꾸지 않는다.
// 재구성 시간이 유효한 양수가 아니면 거부한다. 모든 부위가 이미 목표 개수이면 아무것도 바꾸지 않고 true를 반환한다.
// 전달이 일부만 성공하면 성공한 변경은 유지하고 false를 반환한다.
bool ASwarmBossManager::RebalanceToCounts(TConstArrayView<TPair<ESwarmPartId, int32>> PartTargetCounts)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Swarm_Rebalance);

	// 단계별 소요 시간(ms). 성공하면 마지막에 한 줄로 남긴다. 프레임 튐의 원인을 찾기 위한 측정이다.
	const double RebalanceStart = FPlatformTime::Seconds();
	double SamplingMs = 0.0;
	double PlacementMs = 0.0;
	double TransferMs = 0.0;
	double AssignMs = 0.0;
	int32 ProcessedParts = 0;
	auto MillisecondsSince = [](double Since) { return (FPlatformTime::Seconds() - Since) * 1000.0; };

	if (!FMath::IsFinite(ReconstructionDuration) || ReconstructionDuration <= 0.0f || PartTargetCounts.Num() < 2)
	{
		return false;
	}

	const int32 PartCount = PartTargetCounts.Num();
	TArray<FSwarmPart*> TargetParts;
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

		// Surplus는 (현재 개수 - 목표 개수)다. 양수는 보내야 할 수, 음수는 받아야 할 수다.
		const int32 Surplus = Part->InstanceToParticle.Num() - Pair.Value;
		SeenPartIds.Add(Pair.Key);
		TargetParts.Add(Part);
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
	TArray<bool> bSampled;
	bSampled.Init(false, PartCount);

	// Index 부위의 새 목표 좌표를 생성한다. 실패하면 경고를 남기고 false를 반환한다.
	auto SamplePart = [&](int32 Index)
	{
		const double SampleStart = FPlatformTime::Seconds();
		FSwarmSamplingSettings Settings = TargetParts[Index]->SamplingSettings;
		Settings.ParticleCount = PartTargetCounts[Index].Value;
		Settings.CandidatesPerPoint = RebalanceCandidatesPerPoint;
		Results[Index] = FSwarmShapeSampler::GenerateTargets(Settings);
		bSampled[Index] = Results[Index].bSuccess;
		SamplingMs += MillisecondsSince(SampleStart);

		if (!Results[Index].bSuccess)
		{
			UE_LOG(LogSwarmBossManager, Warning, TEXT("Rebalance sampling failed for %s: %s"),
				*UEnum::GetValueAsString(PartTargetCounts[Index].Key), *Results[Index].Message);
		}

		return Results[Index].bSuccess;
	};

	for (int32 Index = 0; Index < PartCount; ++Index)
	{
		if (TargetParts[Index]->InstanceToParticle.Num() != PartTargetCounts[Index].Value && !SamplePart(Index))
		{
			return false;
		}
	}

	// 새 목표의 도형 크기로 관절 위치를 다시 계산한다. 구체는 현재 화면 위치에 남기므로 순간 이동하지 않는다.
	// 이후 전달과 목표 배정은 옮겨진 부위 ISM 기준으로 수행한다.
	for (int32 Index = 0; Index < PartCount; ++Index)
	{
		if (bSampled[Index])
		{
			TargetParts[Index]->HalfExtents = Results[Index].HalfExtents;
		}
	}

	const double PlacementStart = FPlatformTime::Seconds();
	ApplyPartPlacement(/*bPreserveParticleWorldPositions=*/true);
	PlacementMs = MillisecondsSince(PlacementStart);
	const double TransferStart = FPlatformTime::Seconds();

	// 부위 트리 위에서 이웃 부위끼리만 넘기는 전달 계획을 만든다. 목록에 없는 부위는 개수를 바꾸지 않고 길목 역할만 한다.
	TMap<ESwarmPartId, ESwarmPartId> Parents;
	TMap<ESwarmPartId, int32> CurrentCounts;
	TMap<ESwarmPartId, int32> TargetCounts;

	for (const TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
	{
		Parents.Add(Pair.Key, Pair.Value.Definition.ParentPartId);
		CurrentCounts.Add(Pair.Key, Pair.Value.InstanceToParticle.Num());
		TargetCounts.Add(Pair.Key, Pair.Value.InstanceToParticle.Num());
	}

	for (int32 Index = 0; Index < PartCount; ++Index)
	{
		TargetCounts[PartTargetCounts[Index].Key] = PartTargetCounts[Index].Value;
	}

	TArray<FSwarmTransferShipment> Shipments;

	if (!FSwarmTransferPlanner::Plan(Parents, CurrentCounts, TargetCounts, Shipments))
	{
		UE_LOG(LogSwarmBossManager, Warning, TEXT("Rebalance %s: transfer planning failed."), *GetName());
		return false;
	}

	// 묶음마다 출발 부위에서 첫 관절에 가장 가까운 구체를 골라 최종 부위로 보낸다. 모든 묶음은 동시에 출발한다.
	// 이번 재분배에서 이미 옮긴 구체는 다시 고르지 않으므로, 들어온 구체가 곧바로 다른 곳으로 다시 나가지 않는다.
	// 이동 목표는 바로 아래의 최종 배정에서 다시 정하므로 여기서는 소속만 바꾼다.
	// 옮긴 구체(MovedParticleIds)는 배정에서 출발점을 따로 계산한다.
	TSet<int32> MovedParticleIds;
	TSet<ESwarmPartId> TouchedPartIds;

	for (const FSwarmTransferShipment& Shipment : Shipments)
	{
		const ESwarmPartId SourcePartId = Shipment.Path[0];
		const ESwarmPartId DestinationPartId = Shipment.Path.Last();
		const FVector JointLocation = GetJointWorldLocation(SourcePartId, Shipment.Path[1]);
		TouchedPartIds.Add(SourcePartId);
		TouchedPartIds.Add(DestinationPartId);
		const TArray<int32> ParticleIds =
			SelectParticlesNearest(SourcePartId, JointLocation, Shipment.Count, MovedParticleIds);
		int32 TransferredCount = 0;

		for (int32 ParticleId : ParticleIds)
		{
			if (TransferParticle(ParticleId, DestinationPartId))
			{
				MovedParticleIds.Add(ParticleId);
				++TransferredCount;
			}
		}

		FString PathText;

		for (ESwarmPartId PartId : Shipment.Path)
		{
			PathText += (PathText.IsEmpty() ? TEXT("") : TEXT(" -> ")) + UEnum::GetValueAsString(PartId);
		}

		UE_LOG(LogSwarmBossManager, Display, TEXT("Rebalance %s: %s Requested=%d Transferred=%d Total=%d"),
			*GetName(), *PathText, Shipment.Count, TransferredCount, GetActiveParticleCount());

		if (TransferredCount != Shipment.Count)
		{
			return false;
		}
	}

	TransferMs = MillisecondsSince(TransferStart);

	// 전달한 구체뿐 아니라 부위의 생존 구체 전체를 새 목표와 1:1로 짝지어 최종 배치가 겹치지 않게 한다.
	// 가까운 목표끼리 짝지으므로 맞은 곳에서 먼 구체는 거의 움직이지 않고, 빈자리 주변 구체가 조금씩 메운다.
	for (int32 Index = 0; Index < PartCount; ++Index)
	{
		const ESwarmPartId PartId = PartTargetCounts[Index].Key;

		if (!bSampled[Index])
		{
			// 개수가 그대로이고 드나든 구체도 없으면 지금 목표를 유지한다.
			if (!TouchedPartIds.Contains(PartId))
			{
				continue;
			}

			// 개수가 그대로여도 구체가 드나들었다면 새 구성원까지 포함해 다시 짝지어야 한다. 같은 개수이므로 도형 크기는 그대로다.
			if (!SamplePart(Index))
			{
				return false;
			}
		}

		const double AssignStart = FPlatformTime::Seconds();
		++ProcessedParts;
		const int32 TargetCount = PartTargetCounts[Index].Value;
		const TArray<int32> ParticleIds = SelectPartParticleIds(PartId, TargetCount);

		if (ParticleIds.Num() != TargetCount || Results[Index].Points.Num() != TargetCount)
		{
			return false;
		}

		// 배정의 출발점은 부위 로컬 좌표다. 원래 있던 구체는 현재 위치(이동 중이면 지금 있는 곳), 이번에 들어온 구체는
		// 현재 위치(아직 보낸 부위 쪽, 관절 근처)를 이 부위 도형 안으로 붙인 점이다. 보낸 구체는 관절 근처에 퍼져 있으므로
		// 출발점도 연결 면 위에 조각 모양으로 퍼진다.
		// - 관절(Gap 가운데)을 쓰면 Gap이 클 때 출발점 근처에 목표가 없어 도미노가 사슬을 시작하지 못했다(100~400cm 이동).
		// - 연결점 한 점을 쓰면 한 번에 많이 받을 때(20~30개) 모두 같은 점에서 출발해, 그 근처 목표 몇 개를 차지한 뒤
		//   사슬이 바깥으로 뻗지 못하고 막혔다(Play: 왼다리 26개 유입 시 LongMoves=17).
		const TArray<FSwarmParticle>& Particles = ParticleStore.GetParticles();
		const FSwarmPart& Part = *TargetParts[Index];
		TArray<FVector> Starts;
		Starts.Reserve(TargetCount);

		for (int32 ParticleId : ParticleIds)
		{
			const FVector& Position = Particles[ParticleId].LocalPosition;
			Starts.Add(MovedParticleIds.Contains(ParticleId)
				? FSwarmShapeSampler::ClampInside(Position, Part.Definition.Shape, Part.HalfExtents) : Position);
		}

		const FSwarmSamplingSettings& Sampling = TargetParts[Index]->SamplingSettings;
		FSwarmAssignSettings AssignSettings;
		AssignSettings.CandidateCount = AssignCandidateCount;
		AssignSettings.CellSize = 2.0 * Sampling.ParticleRadius + Sampling.SpacingMargin;
		AssignSettings.LongMoveThreshold = AssignSettings.CellSize * LongMoveThresholdScale;
		AssignSettings.MaxLongMoveRepairs = MaxLongMoveRepairs;

		TArray<int32> Assignment;
		FSwarmAssignStats Stats;

		if (!FSwarmTargetAssigner::Assign(Starts, Results[Index].Points, AssignSettings, Assignment, &Stats))
		{
			return false;
		}

		UE_LOG(LogSwarmBossManager, Display,
			TEXT("Assign %s: N=%d MaxMove=%.1f AvgMove=%.1f LongMoves=%d (Greedy=%d Repaired=%d) Leftovers=%d Time=%.2fms"),
			*UEnum::GetValueAsString(PartId), TargetCount, Stats.MaxMove, Stats.AverageMove, Stats.LongMoves,
			Stats.GreedyLongMoves, Stats.Repaired, Stats.Leftovers, Stats.Milliseconds);

		for (int32 ParticleIndex = 0; ParticleIndex < ParticleIds.Num(); ++ParticleIndex)
		{
			if (!AssignParticleTarget(ParticleIds[ParticleIndex], Results[Index].Points[Assignment[ParticleIndex]],
				ReconstructionDuration))
			{
				return false;
			}
		}

		// 개수가 그대로인데 구체가 드나든 부위는 위에서 샘플링한 뒤 AssignStart를 재므로, 그 시간은 샘플링에만 들어간다.
		AssignMs += MillisecondsSince(AssignStart);
	}

	UE_LOG(LogSwarmBossManager, Display,
		TEXT("Rebalance timing %s: Total=%.2fms Sampling=%.2fms Placement=%.2fms Transfer=%.2fms Assign=%.2fms Parts=%d"),
		*GetName(), MillisecondsSince(RebalanceStart), SamplingMs, PlacementMs, TransferMs, AssignMs, ProcessedParts);

	return true;
}

// T 입력 시점의 FormDefinition 가중치로 생성된 모든 부위의 생존 구체를 재분배한다.
// 현재 생존 수 합계를 초기 생성과 같은 규칙(가중치 비율 버림, 나머지는 RemainderPartId)으로 나눈 뒤 RebalanceToCounts()로 맞춘다.
// 가중치만 다시 읽으며, 도형 종류·종횡비는 마지막 초기 생성 때 보관한 설정을 그대로 사용한다.
// 부위 사이 이동은 부위 트리의 이웃끼리만 넘기며, 모든 관절의 전달이 동시에 출발한다.
// 데이터가 없거나 유효하지 않거나, 생성된 부위와 목록이 다르면 경고를 남기고 상태를 바꾸지 않은 채 false를 반환한다.
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
// 손실 재분배를 기다리는 중이고 열린 피해 구간이 없으면 모인 손실을 한 번에 재분배한 뒤 구체를 이동시킨다.
// 재분배가 실패해도(샘플링 실패 등) 매 프레임 다시 시도하지 않도록 대기를 지운다.
// 이동할 구체도 없고 재분배 대기도 없으면 Actor Tick을 끈다.
void ASwarmBossManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bRebalancePending && OpenDamageWindowCount == 0)
	{
		bRebalancePending = false;

		if (bAutoRebalanceOnLoss && !RebalancePartsFromDefinition())
		{
			UE_LOG(LogSwarmBossManager, Warning, TEXT("Auto rebalance after loss failed on %s."), *GetName());
		}
	}

	const double MovementStart = FPlatformTime::Seconds();
	const bool bStillMoving = UpdateParticleMovement(DeltaTime);
	const double MovementMs = (FPlatformTime::Seconds() - MovementStart) * 1000.0;
	++MovementFrames;
	MovementTotalMs += MovementMs;
	MovementMaxMs = FMath::Max(MovementMaxMs, MovementMs);

	// 측정: 이동이 끝나면 이번 이동 구간의 프레임당 이동 갱신 시간을 한 줄로 남긴다.
	// 움직이는 구체 없이 피해 구간만 기다리던 Tick(1프레임짜리)은 남기지 않는다.
	if (!bStillMoving)
	{
		if (MovementFrames > 1)
		{
			UE_LOG(LogSwarmBossManager, Display, TEXT("Movement timing %s: Frames=%d Avg=%.2fms Max=%.2fms"),
				*GetName(), MovementFrames, MovementTotalMs / MovementFrames, MovementMaxMs);
		}

		MovementFrames = 0;
		MovementTotalMs = 0.0;
		MovementMaxMs = 0.0;
	}

	if (!bStillMoving && !bRebalancePending)
	{
		SetActorTickEnabled(false);
	}
}

// 피해 구간을 하나 연다. 열려 있는 동안 손실 자동 재분배를 미룬다.
void ASwarmBossManager::BeginDamageWindow()
{
	++OpenDamageWindowCount;
}

// 피해 구간을 하나 닫는다. 모두 닫혔고 재분배를 기다리는 중이면 Tick을 켜 다음 Tick에 재분배한다.
// Begin 없이 호출해도 0 아래로 내려가지 않는다.
void ASwarmBossManager::EndDamageWindow()
{
	OpenDamageWindowCount = FMath::Max(0, OpenDamageWindowCount - 1);

	if (OpenDamageWindowCount == 0 && bRebalancePending)
	{
		SetActorTickEnabled(true);
	}
}

// 절대 자세 지정. FRotator를 쿼터니언으로 바꿔 ApplyPartPose()로 넘긴다.
bool ASwarmBossManager::SetPartPose(ESwarmPartId PartId, const FRotator& Rotation)
{
	FSwarmPart* Part = Parts.Find(PartId);

	if (!Part || Rotation.ContainsNaN() || !FMath::IsFinite(Rotation.Pitch) || !FMath::IsFinite(Rotation.Yaw)
		|| !FMath::IsFinite(Rotation.Roll))
	{
		return false;
	}

	return ApplyPartPose(*Part, Rotation.Quaternion());
}

// 저장된 쿼터니언에 부모 축 기준 증분을 앞에서 곱한다(Delta * Current: 현재 자세를 적용한 뒤 Delta를 부모 축으로 더함).
bool ASwarmBossManager::AddPartPoseDelta(ESwarmPartId PartId, const FQuat& Delta)
{
	FSwarmPart* Part = Parts.Find(PartId);

	if (!Part || !IsValidPose(Delta))
	{
		return false;
	}

	return ApplyPartPose(*Part, (Delta.GetNormalized() * Part->Pose).GetNormalized());
}

FQuat ASwarmBossManager::GetPartPose(ESwarmPartId PartId) const
{
	const FSwarmPart* Part = Parts.Find(PartId);
	return Part ? Part->Pose : FQuat::Identity;
}

// 최상위 부위·잘못된 회전·ISM 없음은 거부하고 기존 자세를 유지한다. 통과하면 자세를 저장하고 이 부위 ISM 하나의 상대 변환만 설정한다.
// 자식 부위는 부착 계층으로 함께 돌고, 구체·목표는 부위 로컬이라 함께 돈다. 바닥 높이는 기본 자세 기준이라 다시 계산하지 않는다.
// 측정: 이 호출의 동기 실행 시간만 잰다. 자식 ISM 변환 전파 같은 엔진 내부 비용이 포함되지만, 프레임 전체 비용은 따로 확인해야 한다.
bool ASwarmBossManager::ApplyPartPose(FSwarmPart& Part, const FQuat& Pose)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Swarm_ApplyPose);

	if (Part.Definition.ParentPartId == ESwarmPartId::None || !IsValidPose(Pose) || !IsValid(Part.Instances))
	{
		return false;
	}

	const FSwarmPart* Parent = Parts.Find(Part.Definition.ParentPartId);

	if (!Parent)
	{
		return false;
	}

	const double Start = FPlatformTime::Seconds();
	Part.Pose = Pose.GetNormalized();
	const FTransform Relative = ComputeChildRelativeTransform(Part.Definition, Part.HalfExtents,
		Part.SamplingSettings.ParticleRadius, Parent->Definition.Shape, Parent->HalfExtents,
		Parent->SamplingSettings.ParticleRadius, Part.Pose);
	Part.Instances->SetRelativeLocationAndRotation(Relative.GetLocation(), Relative.GetRotation());

	UE_LOG(LogSwarmBossManager, Display, TEXT("Pose %s: %s Time=%.2fms"), *UEnum::GetValueAsString(Part.Definition.PartId),
		*Part.Pose.Rotator().ToString(), (FPlatformTime::Seconds() - Start) * 1000.0);
	return true;
}

// 부위마다 현재 도형 반크기에 구체 반지름을 더한 로컬 상자를 월드로 변환해 합친다. 구는 세 축 반크기가 반지름이라 같은 식을 쓴다.
// 구체가 0개인 부위(반크기 0)는 건너뛴다. 이동 중인 구체는 잠시 이 상자 밖에 있을 수 있다.
FBox ASwarmBossManager::GetSwarmBounds() const
{
	FBox Bounds(ForceInit);

	for (const TPair<ESwarmPartId, FSwarmPart>& Pair : Parts)
	{
		const FSwarmPart& Part = Pair.Value;

		if (!IsValid(Part.Instances) || Part.HalfExtents.IsZero())
		{
			continue;
		}

		const FVector Extent = Part.HalfExtents + FVector(Part.SamplingSettings.ParticleRadius);
		Bounds += FBox(-Extent, Extent).TransformBy(Part.Instances->GetComponentTransform());
	}

	return Bounds;
}

// 시간 지정 구체는 남은 거리가 남은 시간의 제곱에 비례하도록 이동해 초반에 빠르고 끝에서 느려진다.
// 현재 대비 다음 프레임의 남은 시간 비율을 제곱하므로, 시작 위치를 별도 저장하지 않아도 같은 이차 감속 곡선을 따른다.
// 시간 지정이 없으면 초당 200cm로 이동한다. 도착 프레임에는 목표에 정확히 맞춰 초과 이동을 막는다.
// ISM 갱신이 성공한 경우에만 중앙 데이터의 현재 위치와 남은 시간을 함께 갱신한다.
// 성공적으로 갱신한 구체 중 아직 목표에 도착하지 않은 구체가 있으면 true를 반환한다.
bool ASwarmBossManager::UpdateParticleMovement(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Swarm_UpdateMovement);
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
