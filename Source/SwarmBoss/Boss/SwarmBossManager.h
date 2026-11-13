#pragma once

#include "GameFramework/Actor.h"
#include "Swarm/SwarmParticleStore.h"
#include "Swarm/SwarmShapeSampler.h"
#include "SwarmBossManager.generated.h"

class UInstancedStaticMeshComponent;
class UPrimitiveComponent;
class USwarmFormDefinition;

// 초기 생성할 부위와 해당 ISM 컴포넌트 기준의 구체 중심 좌표 목록.
struct FSwarmPartPlacement
{
	ESwarmPartId PartId;
	TConstArrayView<FVector> LocalPositions;
};

// 부위 하나의 ISM과 인스턴스 번호를 중앙 ParticleId로 바꾸는 배열.
USTRUCT()
struct FSwarmPart
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> Instances;

	TArray<int32> InstanceToParticle;

	// 마지막 초기 생성에 사용한 도형·간격 설정. 재분배 시 개수만 바꿔 목표 좌표를 생성한다.
	FSwarmSamplingSettings SamplingSettings;
};

/** 중앙 구체 저장소와 부위별 ISM을 소유하고, 구체 생성·제거·목표 배정·이동을 처리한다. */
UCLASS()
class SWARMBOSS_API ASwarmBossManager : public AActor
{
	GENERATED_BODY()

public:
	ASwarmBossManager();

	bool GenerateInitialParts();

	// 초기 생성에 사용할 형태 데이터를 바꾼다. 이미 생성된 구체는 그대로 두고 다음 GenerateInitialParts()부터 적용한다.
	void SetFormDefinition(USwarmFormDefinition* NewFormDefinition);

	bool RemoveInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex);
	bool TransferParticle(int32 ParticleId, ESwarmPartId DestinationPartId);

	int32 TransferParticlesToTargets(ESwarmPartId SourcePartId, ESwarmPartId DestinationPartId,
		TConstArrayView<FVector> TargetLocalPositions);

	bool RebalanceParts(ESwarmPartId FirstPartId, ESwarmPartId SecondPartId, int32 FirstWeight, int32 SecondWeight);

	// 현재 FormDefinition의 AllocationWeight로 두 부위의 생존 구체를 재분배한다. 나머지는 RemainderPartId 부위가 받는다.
	bool RebalancePartsFromDefinition();

	bool AssignParticleTarget(int32 ParticleId, const FVector& TargetLocalPosition, float MoveDuration = 0.0f);
	bool AssignReconstructionTargets(const TArray<FVector>& Targets);

	int32 GetActiveParticleCount() const;
	const TArray<FSwarmParticle>& GetParticles() const;

	// 중앙 Particles 배열 순서대로 지정 부위의 활성 구체를 최대 MaxCount개 선택해 ParticleId 목록을 반환한다.
	TArray<int32> SelectPartParticleIds(ESwarmPartId PartId, int32 MaxCount) const;

	virtual void Tick(float DeltaTime) override;

protected:
	virtual void BeginPlay() override;

private:
	friend class FSwarmManagerBehaviorTest;

	// 재구성 목표를 배정한 시점부터 도착까지의 게임 시간(초). 변경한 값은 다음 목표 배정부터 적용한다.
	UPROPERTY(EditAnywhere, Category="Swarm|Movement", meta=(ClampMin="0.01", UIMin="0.01", Units="s"))
	float ReconstructionDuration = 0.8f;

	bool InitializeParticles(TConstArrayView<FSwarmPartPlacement> Placements, double ParticleRadius);
	void AddPartParticles(const FSwarmPartPlacement& Placement, double ParticleRadius);
	void ResetParticles(int32 ExpectedParticleCount);

	bool ValidateFormDefinition(const USwarmFormDefinition& Definition) const;

	FSwarmPart* FindPartByComponent(const UPrimitiveComponent* HitComponent);
	bool UpdateParticleMovement(float DeltaTime);

	// 초기 생성할 부위의 도형·종횡비·배분 가중치를 담은 형태 데이터. 비어 있으면 생성하지 않는다.
	// 현재 Manager는 몸통과 왼팔 ISM만 가지므로 이 두 부위를 정확히 정의한 데이터만 사용할 수 있다.
	UPROPERTY(EditAnywhere, Category="Swarm|Generation")
	TObjectPtr<USwarmFormDefinition> FormDefinition;

	// Play 시작 시 생성할 전체 구체 수. FormDefinition의 가중치 비율로 나누고 정수 배분의 나머지는 잔여 수신 부위에 배정한다.
	UPROPERTY(EditAnywhere, Category="Swarm|Generation", meta=(ClampMin="0", ClampMax="10000"))
	int32 InitialParticleCount = 90;

	// 초기 생성 시 몸통과 왼팔의 구체 표면 사이에 확보할 Y축 여유 거리(cm).
	UPROPERTY(EditAnywhere, Category="Swarm|Generation", meta=(ClampMin="0", Units="cm"))
	float PartGap = 150.0f;

	FSwarmParticleStore ParticleStore;

	UPROPERTY()
	TMap<ESwarmPartId, FSwarmPart> Parts;

	UPROPERTY(VisibleAnywhere, Category="Swarm")
	TObjectPtr<UInstancedStaticMeshComponent> TorsoInstances;

	UPROPERTY(VisibleAnywhere, Category="Swarm")
	TObjectPtr<UInstancedStaticMeshComponent> LeftArmInstances;
};
