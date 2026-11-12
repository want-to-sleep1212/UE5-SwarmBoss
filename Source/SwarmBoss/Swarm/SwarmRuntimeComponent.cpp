#include "SwarmRuntimeComponent.h"

#include "Components/InstancedStaticMeshComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwarmRuntime, Log, All);

USwarmRuntimeComponent::USwarmRuntimeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void USwarmRuntimeComponent::BindInstances(UInstancedStaticMeshComponent* InInstances)
{
	Instances = InInstances;
}

bool USwarmRuntimeComponent::ReplaceParticles(TConstArrayView<FVector> LocalPositions, double ParticleRadius)
{
	if (!IsValid(Instances))
	{
		return false;
	}

	Instances->Modify();
	SetComponentTickEnabled(false);

	ParticleStore.ResetFromPositions(LocalPositions);

	TArray<FTransform> Transforms;
	Transforms.Reserve(LocalPositions.Num());

	// Engine BasicShapes/Sphere는 반지름 50cm이다. 목표 도형 크기와 무관하게 고정한다.
	const FVector ParticleScale(ParticleRadius / 50.0);

	for (const FVector& Point : LocalPositions)
	{
		Transforms.Emplace(FQuat::Identity, Point, ParticleScale);
	}

	Instances->ClearInstances();
	// 기존 레벨에 NoCollision으로 저장된 Actor도 재생성하면 새 프리셋을 적용한다.
	Instances->SetCollisionProfileName(TEXT("SwarmSpherePreset"));
	Instances->SetRemoveSwap();

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

		ParticleStore.SetInstanceIndex(ParticleId, InstanceIndex);
		InstanceToParticle[InstanceIndex] = ParticleId;
	}

	OnActiveParticleCountChanged.Broadcast(GetActiveParticleCount());

	return true;
}

void USwarmRuntimeComponent::ClearParticles()
{
	SetComponentTickEnabled(false);

	if (IsValid(Instances))
	{
		Instances->Modify();
		Instances->ClearInstances();
	}

	ParticleStore.Clear();
	InstanceToParticle.Reset();

	OnActiveParticleCountChanged.Broadcast(GetActiveParticleCount());
}

bool USwarmRuntimeComponent::RemoveInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex)
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

	ParticleStore.DeactivateParticle(RemovedParticleId);

	// ISM과 같은 방식으로 마지막 항목을 삭제한 자리에 옮긴다.
	InstanceToParticle.RemoveAtSwap(InstanceIndex, EAllowShrinking::No);

	if (InstanceIndex != LastInstanceIndex)
	{
		ParticleStore.SetInstanceIndex(LastParticleId, InstanceIndex);
	}

	OnActiveParticleCountChanged.Broadcast(GetActiveParticleCount());

	UE_LOG(LogSwarmRuntime, Display,
		TEXT("Removed Instance=%d Remaining=%d"),
		InstanceIndex,
		GetActiveParticleCount());

	return true;
}

bool USwarmRuntimeComponent::AssignParticleTarget(int32 ParticleId, const FVector& TargetLocalPosition)
{
	if (!ParticleStore.AssignTarget(ParticleId, TargetLocalPosition))
	{
		return false;
	}

	if (ParticleStore.GetParticles()[ParticleId].LocalPosition != TargetLocalPosition)
	{
		SetComponentTickEnabled(true);
	}

	return true;
}

bool USwarmRuntimeComponent::AssignReconstructionTargets(const TArray<FVector>& Targets)
{
	if (Targets.Num() != GetActiveParticleCount())
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

		AssignParticleTarget(ParticleId, Targets[TargetIndex]);
		++TargetIndex;
	}

	return true;
}

void USwarmRuntimeComponent::OffsetActiveParticleTargets(const FVector& LocalOffset)
{
	const TArray<FSwarmParticle>& Particles = GetParticles();

	for (int32 ParticleId = 0; ParticleId < Particles.Num(); ++ParticleId)
	{
		const FSwarmParticle& Particle = Particles[ParticleId];

		if (Particle.bActive)
		{
			AssignParticleTarget(ParticleId, Particle.LocalPosition + LocalOffset);
		}
	}
}

void USwarmRuntimeComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!UpdateParticleMovement(DeltaTime))
	{
		SetComponentTickEnabled(false);
	}
}

bool USwarmRuntimeComponent::UpdateParticleMovement(float DeltaTime)
{
	if (!IsValid(Instances))
	{
		return false;
	}

	constexpr float MoveSpeed = 200.0f; // cm/s
	bool bTransformsChanged = false;
	bool bHasMovingParticles = false;

	const TArray<FSwarmParticle>& Particles = ParticleStore.GetParticles();

	for (int32 ParticleId = 0; ParticleId < Particles.Num(); ++ParticleId)
	{
		const FSwarmParticle& Particle = Particles[ParticleId];
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
			ParticleStore.SetLocalPosition(ParticleId, NextPosition);
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
