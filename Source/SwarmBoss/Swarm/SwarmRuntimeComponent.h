#pragma once

#include "Components/ActorComponent.h"
#include "SwarmParticleStore.h"
#include "SwarmRuntimeComponent.generated.h"

class UInstancedStaticMeshComponent;
class UPrimitiveComponent;

DECLARE_MULTICAST_DELEGATE_OneParam(FSwarmActiveParticleCountChanged, int32);

/** 군체 개체와 ISM 인스턴스의 연결, 제거 및 이동을 관리한다. */
UCLASS()
class SWARMBOSS_API USwarmRuntimeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USwarmRuntimeComponent();

	void BindInstances(UInstancedStaticMeshComponent* InInstances);
	bool ReplaceParticles(TConstArrayView<FVector> LocalPositions, double ParticleRadius);
	void ClearParticles();

	bool RemoveInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex);
	bool AssignParticleTarget(int32 ParticleId, const FVector& TargetLocalPosition);

	// 개체 ID 순서대로 활성 구체에 재구성 목표를 하나씩 배정한다.
	bool AssignReconstructionTargets(const TArray<FVector>& Targets);

	// 활성 개체마다 현재 위치에 로컬 오프셋을 더한 좌표를 배정한다.
	void OffsetActiveParticleTargets(const FVector& LocalOffset);

	int32 GetActiveParticleCount() const { return ParticleStore.GetActiveParticleCount(); }
	const TArray<FSwarmParticle>& GetParticles() const { return ParticleStore.GetParticles(); }

	FSwarmActiveParticleCountChanged OnActiveParticleCountChanged;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	friend class FSwarmPreviewTest;

	bool UpdateParticleMovement(float DeltaTime);

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> Instances;

	FSwarmParticleStore ParticleStore;

	// 현재 인스턴스 번호로 개체 ID를 찾는다.
	TArray<int32> InstanceToParticle;
};
