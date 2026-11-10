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

/** 배치와 인스턴스 식별 실험용 Actor. 개체 이동·재분배는 포함하지 않는다. */
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

	UPROPERTY(VisibleInstanceOnly, Category="Swarm|Result")
	FString LastReport;

	UPROPERTY(VisibleInstanceOnly, Category="Swarm|Result")
	int32 GeneratedCount = 0;

	UPROPERTY(VisibleInstanceOnly, Category="Swarm|Result")
	FVector AppliedHalfExtents = FVector::ZeroVector;

	// 무거운 생성을 Details 변경·Actor 드래그마다 실행하지 않는다.
	UFUNCTION(CallInEditor, Category="Swarm")
	void GeneratePreview();

	UFUNCTION(CallInEditor, Category="Swarm")
	void ClearPreview();

	UFUNCTION(CallInEditor, Category="Swarm")
	void ShowBounds();

	// 반환된 번호로 월드 위치를 다시 조회해 표시한다. 잘못된 대상·번호면 false.
	bool HighlightInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex);

	// 맞힌 구체 하나를 제거하고 실제 남은 개수를 갱신한다.
	bool RemoveInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex);



protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY()
	ESwarmPrimitive AppliedShape = ESwarmPrimitive::Sphere;
};
