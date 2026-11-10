#pragma once

#include "CoreMinimal.h"
#include "SwarmShapeSampler.generated.h"

UENUM(BlueprintType)
enum class ESwarmPrimitive : uint8
{
	Sphere,
	Box
};

/** 단일 도형 생성 설정. 길이는 cm, BoxHalfExtents는 기준 반길이이다. */
USTRUCT(BlueprintType)
struct SWARMBOSS_API FSwarmSamplingSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category="Shape")
	ESwarmPrimitive Shape = ESwarmPrimitive::Sphere;

	UPROPERTY(EditAnywhere, Category="Shape", meta=(ClampMin="0", ClampMax="10000"))
	int32 ParticleCount = 300;

	UPROPERTY(EditAnywhere, Category="Shape", meta=(ClampMin="0.01"))
	FVector BoxHalfExtents = FVector(1.0, 1.0, 2.0);

	UPROPERTY(EditAnywhere, Category="Spacing", meta=(ClampMin="0.01"))
	float ParticleRadius = 10.0f;

	UPROPERTY(EditAnywhere, Category="Spacing", meta=(ClampMin="0"))
	float SpacingMargin = 2.0f;

	UPROPERTY(EditAnywhere, Category="Spacing", meta=(ClampMin="0.01", ClampMax="10"))
	float PackingCoefficient = 0.35f;

	UPROPERTY(EditAnywhere, Category="Sampling")
	int32 Seed = 12345;

	UPROPERTY(EditAnywhere, Category="Sampling", meta=(ClampMin="1", ClampMax="100"))
	int32 CandidatesPerPoint = 30;

	UPROPERTY(EditAnywhere, Category="Sampling", meta=(ClampMin="1", ClampMax="16"))
	int32 MaxAttempts = 3;

	// 최대 선형 확장 비율. 0.1이면 각 축을 최대 10% 확장한다.
	UPROPERTY(EditAnywhere, Category="Sampling", meta=(ClampMin="0", ClampMax="1"))
	float MaxExpansionRatio = 0.1f;

	UPROPERTY(EditAnywhere, Category="Sampling", meta=(ClampMin="100", ClampMax="10000000"))
	int32 MaxCandidateTests = 1000000;
};

/** 목표 중심 좌표와 진단 정보. 실패 결과는 미리보기에 적용하지 않는다. */
struct SWARMBOSS_API FSwarmSamplingResult
{
	bool bSuccess = false;
	TArray<FVector> Points;
	FVector HalfExtents = FVector::ZeroVector;
	double MinimumDistance = 0.0;
	double MeasuredMinimumDistance = 0.0;
	double ExpansionRatio = 0.0;
	double ElapsedMilliseconds = 0.0;
	int32 Attempts = 0;
	int32 CandidateTests = 0;
	FString Message;
};

/** 엔진 오브젝트와 분리된 3D Poisson Disk 샘플러. */
class SWARMBOSS_API FSwarmShapeSampler
{
public:
	static FSwarmSamplingResult GenerateTargets(const FSwarmSamplingSettings& Settings);
	static bool Contains(const FVector& Point, ESwarmPrimitive Shape, const FVector& HalfExtents);
	static bool ValidateTargets(const TArray<FVector>& Points, ESwarmPrimitive Shape,
		const FVector& HalfExtents, double MinimumDistance, double& OutMeasuredMinimum);
};
