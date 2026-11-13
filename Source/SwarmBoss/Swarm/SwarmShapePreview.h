// SwarmShapePreview.h
// 1단계 배치 실험용 Actor(ASwarmShapePreview) 선언.
// 레벨에 배치한 뒤 Details에서 Settings를 조정하고 에디터 버튼(Generate/Clear Preview, Show Bounds)으로
// 샘플러 결과를 Instanced Static Mesh 구체로 확인한다. 결과 보고(LastReport 등)를 읽기 전용으로 노출한다.
// 게임 실행용 데이터나 충돌 없이 샘플러의 좌표를 표시한다.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SwarmShapeSampler.h"
#include "SwarmShapePreview.generated.h"

class UInstancedStaticMeshComponent;

/** 샘플러가 생성한 구체의 개수, 간격과 도형 경계를 확인하는 Actor. */
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

	// 마지막으로 적용한 미리보기의 메시 인스턴스 수.
	UPROPERTY(VisibleInstanceOnly, Transient, Category="Swarm|Result")
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

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY()
	ESwarmPrimitive AppliedShape = ESwarmPrimitive::Sphere;
};
