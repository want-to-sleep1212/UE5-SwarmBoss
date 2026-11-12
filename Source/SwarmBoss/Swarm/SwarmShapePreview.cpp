// SwarmShapePreview.cpp
// ASwarmShapePreview 구현.
// - 생성자: SwarmSpherePreset을 사용하는 ISM을 루트로 만들고 엔진 기본 구체 메시를 지정한다.
// - OnConstruction(): Actor Scale을 1로 고정한다. 도형 크기는 Settings로만 결정한다.
// - GeneratePreview(): 샘플러를 호출해 보고 문자열과 로그(LogSwarmShape)를 남기고, 성공 시에만 인스턴스를 교체한다.
//   실패하면 기존 미리보기를 유지한다. 구체 스케일은 ParticleRadius로만 정한다. BeginPlay에서도 한 번 실행한다.
// - ClearPreview(): 모든 인스턴스와 결과 값을 초기화한다.
// - ShowBounds(): 적용된 중심 허용 영역을 10초간 디버그 도형으로 표시한다.
// - HighlightInstance(): 인스턴스 번호로 조회한 월드 위치에 녹색 테두리를 2초간 표시한다.

#include "SwarmShapePreview.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwarmShape, Log, All);

ASwarmShapePreview::ASwarmShapePreview()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Particles"));
	SetRootComponent(Instances);

	Instances->SetMobility(EComponentMobility::Movable);
	Instances->SetCollisionProfileName(TEXT("SwarmSpherePreset"));
	Instances->SetCanEverAffectNavigation(false);
	Instances->SetRemoveSwap();

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded())
	{
		Instances->SetStaticMesh(Sphere.Object);
	}
}

void ASwarmShapePreview::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 크기는 Settings로 결정한다. Actor Scale로 구체까지 축소하지 않는다.
	SetActorScale3D(FVector::OneVector);
}

void ASwarmShapePreview::BeginPlay()
{
	Super::BeginPlay();

	GeneratePreview();
}

void ASwarmShapePreview::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!UpdateParticleMovement(DeltaTime))
	{
		SetActorTickEnabled(false);
	}
}

void ASwarmShapePreview::GeneratePreview()
{
	const FSwarmSamplingResult Result = FSwarmShapeSampler::GenerateTargets(Settings);
	LastReport = FString::Printf(TEXT("%s Requested=%d Produced=%d Attempts=%d Expansion=%.2f%% MinDistance=%.3f Measured=%.3f Tests=%d Time=%.2fms"),
		*Result.Message, Settings.ParticleCount, Result.Points.Num(), Result.Attempts, Result.ExpansionRatio * 100.0,
		Result.MinimumDistance, Result.MeasuredMinimumDistance, Result.CandidateTests, Result.ElapsedMilliseconds);

	if (!Result.bSuccess)
	{
		UE_LOG(LogSwarmShape, Warning, TEXT("%s Existing preview retained."), *LastReport);
		return;
	}

	Modify();
	Instances->Modify();

	SetActorTickEnabled(false);

	Particles.Reset(Result.Points.Num());

	TArray<FTransform> Transforms;
	Transforms.Reserve(Result.Points.Num());

	// Engine BasicShapes/Sphere는 반지름 50cm이다. 목표 도형 크기와 무관하게 고정한다.
	const FVector ParticleScale(Settings.ParticleRadius / 50.0);

	for (const FVector& Point : Result.Points)
	{
		FSwarmParticle& Particle = Particles.AddDefaulted_GetRef();
		Particle.LocalPosition = Point;
		Particle.TargetLocalPosition = Particle.LocalPosition;

		Transforms.Emplace(FQuat::Identity, Particle.LocalPosition, ParticleScale);
	}

	Instances->ClearInstances();
	// 기존 레벨에 NoCollision으로 저장된 Actor도 재생성하면 새 프리셋을 적용한다.
	Instances->SetCollisionProfileName(TEXT("SwarmSpherePreset"));

	const TArray<int32> NewInstanceIndices = Instances->AddInstances(
		Transforms,
		/*bShouldReturnIndices=*/true,
		/*bWorldSpace=*/false,
		/*bUpdateNavigation=*/false);

	InstanceToParticle.Init(INDEX_NONE, Instances->GetInstanceCount());

	// 반환된 번호는 Transforms와 같은 순서다. 개체 ID는 Particles의 슬롯 번호다.
	for (int32 ParticleId = 0; ParticleId < NewInstanceIndices.Num(); ++ParticleId)
	{
		const int32 InstanceIndex = NewInstanceIndices[ParticleId];

		Particles[ParticleId].InstanceIndex = InstanceIndex;
		InstanceToParticle[InstanceIndex] = ParticleId;
	}

	ActiveParticleCount = Particles.Num();
	AppliedHalfExtents = Result.HalfExtents;
	AppliedShape = Settings.Shape;

	UE_LOG(LogSwarmShape, Display, TEXT("%s"), *LastReport);
}

FSwarmSamplingResult ASwarmShapePreview::GenerateReconstructionTargets() const
{
	FSwarmSamplingSettings ReconstructionSettings = Settings;
	ReconstructionSettings.ParticleCount = ActiveParticleCount;

	return FSwarmShapeSampler::GenerateTargets(ReconstructionSettings);
}

bool ASwarmShapePreview::AssignReconstructionTargets(const TArray<FVector>& Targets)
{
	if (Targets.Num() != ActiveParticleCount)
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

	int32 TargetIndex = 0;

	for (int32 ParticleId = 0; ParticleId < Particles.Num(); ++ParticleId)
	{
		if (!Particles[ParticleId].bActive)
		{
			continue;
		}

		AssignParticleTarget(ParticleId, Targets[TargetIndex]);
		++TargetIndex;
	}

	return true;
}

bool ASwarmShapePreview::ReconstructPreview()
{
	const FSwarmSamplingResult Result = GenerateReconstructionTargets();

	if (!Result.bSuccess)
	{
		UE_LOG(LogSwarmShape, Warning, TEXT("Swarm reconstruction sampling failed for %s: %s"),
			*GetName(), *Result.Message);
		return false;
	}

	if (!AssignReconstructionTargets(Result.Points))
	{
		UE_LOG(LogSwarmShape, Warning, TEXT("Swarm reconstruction targets were rejected for %s."),
			*GetName());
		return false;
	}

	const FVector PreviousHalfExtents = AppliedHalfExtents;

	AppliedHalfExtents = Result.HalfExtents;
	AppliedShape = Settings.Shape;

	LastReport = FString::Printf(
		TEXT("Reconstruction Active=%d HalfExtents(cm): %s -> %s Expansion=%.2f%%"),
		ActiveParticleCount,
		*PreviousHalfExtents.ToString(),
		*AppliedHalfExtents.ToString(),
		Result.ExpansionRatio * 100.0);

	UE_LOG(LogSwarmShape, Display, TEXT("%s: %s"), *GetName(), *LastReport);

	return true;
}

void ASwarmShapePreview::ClearPreview()
{
	SetActorTickEnabled(false);

	Modify();
	Instances->Modify();

	Instances->ClearInstances();
	Particles.Reset();
	InstanceToParticle.Reset();

	ActiveParticleCount = 0;
	AppliedHalfExtents = FVector::ZeroVector;
	LastReport = TEXT("Preview cleared.");
}

void ASwarmShapePreview::ShowBounds()
{
	if (!GetWorld() || Instances->GetInstanceCount() == 0)
	{
		return;
	}

	if (AppliedShape == ESwarmPrimitive::Sphere)
	{
		DrawDebugSphere(GetWorld(), GetActorLocation(), AppliedHalfExtents.X, 32, FColor::Cyan, false, 10.0f);
	}
	else
	{
		DrawDebugBox(GetWorld(), GetActorLocation(), AppliedHalfExtents, GetActorQuat(), FColor::Cyan, false, 10.0f);
	}
}

bool ASwarmShapePreview::HighlightInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex)
{
	if (!IsValid(Instances) || HitComponent != Instances.Get() || !GetWorld()
		|| InstanceIndex < 0 || InstanceIndex >= Instances->GetInstanceCount())
	{
		return false;
	}

	const UStaticMesh* Mesh = Instances->GetStaticMesh();
	FTransform WorldTransform;

	if (!Mesh || !Instances->GetInstanceTransform(InstanceIndex, WorldTransform, true))
	{
		return false;
	}

	// 충돌 지점이 아닌 번호로 조회한 위치를 표시해야 번호 매핑을 검증할 수 있다.
	// 현재 Settings는 재생성 전 값일 수 있으므로 실제 메시 크기와 인스턴스 Scale을 사용한다.
	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const FVector Center = WorldTransform.TransformPosition(Bounds.Origin);
	const double Radius = Bounds.BoxExtent.GetMax() * WorldTransform.GetScale3D().GetAbsMax() * 1.1;

	DrawDebugSphere(GetWorld(), Center, Radius, 24, FColor::Green, false, 2.0f, 0, 1.5f);

	UE_LOG(LogSwarmShape, Display, TEXT("Highlight Instance=%d Center=%s"), InstanceIndex, *Center.ToString());

	return true;
}

bool ASwarmShapePreview::RemoveInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex)
{
	if (!IsValid(Instances) || HitComponent != Instances.Get())
	{
		return false;
	}

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

	FSwarmParticle& RemovedParticle = Particles[RemovedParticleId];
	RemovedParticle.bActive = false;
	RemovedParticle.InstanceIndex = INDEX_NONE;

	// ISM과 같은 방식으로 마지막 항목을 삭제한 자리에 옮긴다.
	InstanceToParticle.RemoveAtSwap(InstanceIndex, EAllowShrinking::No);

	if (InstanceIndex != LastInstanceIndex)
	{
		Particles[LastParticleId].InstanceIndex = InstanceIndex;
	}

	--ActiveParticleCount;

	UE_LOG(LogSwarmShape, Display,
		TEXT("Removed Instance=%d Remaining=%d"),
		InstanceIndex,
		ActiveParticleCount);

	return true;
}

bool ASwarmShapePreview::AssignParticleTarget(int32 ParticleId, const FVector& TargetLocalPosition)
{
	if (!Particles.IsValidIndex(ParticleId) || TargetLocalPosition.ContainsNaN())
	{
		return false;
	}

	FSwarmParticle& Particle = Particles[ParticleId];

	if (!Particle.bActive)
	{
		return false;
	}

	Particle.TargetLocalPosition = TargetLocalPosition;

	if (Particle.LocalPosition != TargetLocalPosition)
	{
		SetActorTickEnabled(true);
	}

	return true;
}

void ASwarmShapePreview::OffsetActiveParticleTargets(const FVector& LocalOffset)
{
	for (int32 ParticleId = 0; ParticleId < Particles.Num(); ++ParticleId)
	{
		const FSwarmParticle& Particle = Particles[ParticleId];

		if (Particle.bActive)
		{
			AssignParticleTarget(ParticleId, Particle.LocalPosition + LocalOffset);
		}
	}
}

bool ASwarmShapePreview::UpdateParticleMovement(float DeltaTime)
{
	if (!IsValid(Instances))
	{
		return false;
	}

	constexpr float MoveSpeed = 200.0f; // cm/s
	bool bTransformsChanged = false;
	bool bHasMovingParticles = false;

	for (FSwarmParticle& Particle : Particles)
	{
		if (!Particle.bActive || Particle.LocalPosition == Particle.TargetLocalPosition)
		{
			continue;
		}

		FTransform InstanceTransform;
		if (!Instances->GetInstanceTransform(Particle.InstanceIndex, InstanceTransform, false))
		{
			continue;
		}

		const FVector NextPosition = FMath::VInterpConstantTo(
			Particle.LocalPosition,
			Particle.TargetLocalPosition,
			DeltaTime,
			MoveSpeed);

		InstanceTransform.SetLocation(NextPosition);

		if (Instances->UpdateInstanceTransform(
			Particle.InstanceIndex,
			InstanceTransform,
			/*bWorldSpace=*/false,
			/*bMarkRenderStateDirty=*/false,
			/*bTeleport=*/true))
		{
			Particle.LocalPosition = NextPosition;
			bTransformsChanged = true;
			bHasMovingParticles |= NextPosition != Particle.TargetLocalPosition;
		}
	}

	if (bTransformsChanged)
	{
		Instances->MarkRenderStateDirty();
	}

	return bHasMovingParticles;
}
