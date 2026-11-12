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
class USwarmRuntimeComponent;

/** 구체 배치, 목표 이동, 인스턴스 식별 실험용 Actor. */
UCLASS()
class SWARMBOSS_API ASwarmShapePreview : public AActor
{
	GENERATED_BODY()

public:
	ASwarmShapePreview();
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(EditAnywhere, Category="Swarm")
	FSwarmSamplingSettings Settings;

	UPROPERTY(VisibleAnywhere, Category="Swarm")
	TObjectPtr<UInstancedStaticMeshComponent> Instances;

	UPROPERTY(VisibleAnywhere, Category="Swarm")
	TObjectPtr<USwarmRuntimeComponent> SwarmRuntime;

	UPROPERTY(VisibleInstanceOnly, Category="Swarm|Result")
	FString LastReport;

	// Details 표시용 값. 게임 로직은 ParticleStore의 생존 개수를 사용한다.
	UPROPERTY(VisibleInstanceOnly, Transient, Category="Swarm|Result")
	int32 ActiveParticleCount = 0;

	UPROPERTY(VisibleInstanceOnly, Category="Swarm|Result")
	FVector AppliedHalfExtents = FVector::ZeroVector;

	// 무거운 생성을 Details 변경·Actor 드래그마다 실행하지 않는다.
	UFUNCTION(CallInEditor, Category="Swarm")
	void GeneratePreview();

	// 생존 개수로 재구성에 사용할 목표 좌표를 계산한다.
	// TODO: 군체 보스의 형태 제어 코드를 추가할 때 이 함수를 그쪽으로 옮긴다.
	FSwarmSamplingResult GenerateReconstructionTargets() const;

	// 생존 구체의 목표와 표시할 도형 경계를 함께 갱신한다.
	bool ReconstructPreview();

	UFUNCTION(CallInEditor, Category="Swarm")
	void ClearPreview();

	UFUNCTION(CallInEditor, Category="Swarm")
	void ShowBounds();

	// 반환된 번호로 월드 위치를 다시 조회해 표시한다. 잘못된 대상·번호면 false.
	bool HighlightInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex);

protected:
	virtual void BeginPlay() override;

private:
	void UpdateDisplayedActiveParticleCount(int32 NewCount);

	UPROPERTY()
	ESwarmPrimitive AppliedShape = ESwarmPrimitive::Sphere;
};
