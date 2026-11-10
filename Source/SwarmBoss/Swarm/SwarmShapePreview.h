#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SwarmShapeSampler.h"
#include "SwarmShapePreview.generated.h"

class UInstancedStaticMeshComponent;

/** 1단계 배치 실험용 Actor. 이동·피격·재분배 기능은 포함하지 않는다. */
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

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY()
	ESwarmPrimitive AppliedShape = ESwarmPrimitive::Sphere;
};
