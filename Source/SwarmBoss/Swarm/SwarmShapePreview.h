// SwarmShapePreview.h
// 1단계 배치 실험용 Actor(ASwarmShapePreview) 선언.
// 레벨에 배치한 뒤 Details에서 Settings를 조정하고 에디터 버튼(Generate/Clear Preview, Show Bounds)으로
// 샘플러 결과를 Instanced Static Mesh 구체로 확인한다. 결과 보고(LastReport 등)를 읽기 전용으로 노출한다.
// 조회용 충돌과 번호 기반 강조 표시를 제공한다. 클릭·Trace로 맞힌 구체 하나를 제거한다.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SwarmShapeSampler.h"
#include "SwarmShapePreview.generated.h"

class UInstancedStaticMeshComponent;
class UPrimitiveComponent;

/** 생성한 구체 하나의 도형 기준 위치와 초기 활성 상태. */
struct FSwarmParticle
{
	FVector LocalPosition = FVector::ZeroVector;

	FVector TargetLocalPosition = FVector::ZeroVector;

	bool bActive = true;

	int32 InstanceIndex = INDEX_NONE;
};

/** 구체 배치, 목표 이동, 인스턴스 식별 실험용 Actor. */
UCLASS()
class SWARMBOSS_API ASwarmShapePreview : public AActor
{
	GENERATED_BODY()

public:
	ASwarmShapePreview();
	virtual void OnConstruction(const FTransform& Transform) override;

	virtual void Tick(float DeltaTime) override;

	UPROPERTY(EditAnywhere, Category="Swarm")
	FSwarmSamplingSettings Settings;

	UPROPERTY(VisibleAnywhere, Category="Swarm")
	TObjectPtr<UInstancedStaticMeshComponent> Instances;

	UPROPERTY(VisibleInstanceOnly, Category="Swarm|Result")
	FString LastReport;

	UPROPERTY(VisibleInstanceOnly, Transient, Category="Swarm|Result")
	int32 ActiveParticleCount = 0;

	UPROPERTY(VisibleInstanceOnly, Category="Swarm|Result")
	FVector AppliedHalfExtents = FVector::ZeroVector;

	// 무거운 생성을 Details 변경·Actor 드래그마다 실행하지 않는다.
	UFUNCTION(CallInEditor, Category="Swarm")
	void GeneratePreview();

	// 생존 개수로 재구성에 사용할 목표 좌표를 계산한다.
	FSwarmSamplingResult GenerateReconstructionTargets() const;

	// 개체 ID 순서대로 활성 구체에 재구성 목표를 하나씩 배정한다.
	bool AssignReconstructionTargets(const TArray<FVector>& Targets);

	// 생존 구체의 목표와 표시할 도형 경계를 함께 갱신한다.
	bool ReconstructPreview();

	UFUNCTION(CallInEditor, Category="Swarm")
	void ClearPreview();

	UFUNCTION(CallInEditor, Category="Swarm")
	void ShowBounds();

	// 반환된 번호로 월드 위치를 다시 조회해 표시한다. 잘못된 대상·번호면 false.
	bool HighlightInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex);

	// 맞힌 구체 하나를 제거하고 실제 남은 개수를 갱신한다.
	bool RemoveInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex);

	// 지정한 활성 개체에 Actor 기준 목표 좌표를 배정하고 이동을 시작한다.
	bool AssignParticleTarget(int32 ParticleId, const FVector& TargetLocalPosition);

	// 활성 개체마다 현재 위치에 로컬 오프셋을 더한 좌표를 배정한다.
	void OffsetActiveParticleTargets(const FVector& LocalOffset);



protected:
	virtual void BeginPlay() override;

private:
	friend class FSwarmPreviewTest;

	// 한 프레임 이동한 뒤 아직 목표로 이동할 개체가 있는지 반환한다.
	bool UpdateParticleMovement(float DeltaTime);

	// 성공한 생성 결과를 담는다. 배열의 자리 번호가 개체 ID다.
	TArray<FSwarmParticle> Particles;

	// 현재 인스턴스 번호로 개체 ID를 찾는다.
	TArray<int32> InstanceToParticle;

	UPROPERTY()
	ESwarmPrimitive AppliedShape = ESwarmPrimitive::Sphere;
};
