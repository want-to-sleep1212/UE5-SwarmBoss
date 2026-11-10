// SwarmShapeSampler.h
// 군체 목표 위치 생성기의 공개 인터페이스.
// - ESwarmPrimitive: 배치 영역 종류(구·박스).
// - FSwarmSamplingSettings: 개체 수, 구체 반지름, 간격, 배치 계수, 시드, 시도·확장·예산 상한 등 생성 설정.
// - FSwarmSamplingResult: 생성된 목표 좌표(도형 중심 기준 상대 좌표)와 적용 크기·확장량·소요 시간 등 진단 정보.
// - FSwarmShapeSampler: 엔진 오브젝트와 분리된 3D Poisson Disk 샘플러. 생성·포함 판정·결과 검증을 제공한다.

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

	// 구체 중심을 배치할 영역의 종류. 구형 또는 박스형.
	UPROPERTY(EditAnywhere, Category="Shape")
	ESwarmPrimitive Shape = ESwarmPrimitive::Sphere;

	// 생성할 목표 좌표(구체) 개수 N. 도형 크기는 이 값과 최소 간격으로 결정된다.
	// 0이면 빈 결과로 성공, 1이면 원점 하나만 생성한다.
	UPROPERTY(EditAnywhere, Category="Shape", meta=(ClampMin="0", ClampMax="10000"))
	int32 ParticleCount = 300;

	// 박스 영역의 기준 종횡비. 절대 크기가 아니라 축 간 비율만 사용하며,
	// 실제 반길이는 필요 부피에 맞춰 균등 확대·축소된다. 구형 영역에서는 무시한다.
	UPROPERTY(EditAnywhere, Category="Shape", meta=(ClampMin="0.01"))
	FVector BoxHalfExtents = FVector(1.0, 1.0, 2.0);

	// 개별 구체의 고정 반지름(cm). 도형 크기가 변해도 구체 크기는 유지한다.
	UPROPERTY(EditAnywhere, Category="Spacing", meta=(ClampMin="0.01"))
	float ParticleRadius = 10.0f;

	// 두 구체 표면 사이에 추가로 확보할 여유(cm). 최소 중심 간격 d = 2 × 반지름 + 여유.
	UPROPERTY(EditAnywhere, Category="Spacing", meta=(ClampMin="0"))
	float SpacingMargin = 2.0f;

	// 무차원 배치 계수 k. 필요 부피 V = N × d³ / k를 계산한다.
	// 작을수록 도형이 커져 배치가 넉넉해지고, 클수록 촘촘해지지만 생성 실패·확장이 늘어난다.
	UPROPERTY(EditAnywhere, Category="Spacing", meta=(ClampMin="0.01", ClampMax="10"))
	float PackingCoefficient = 0.35f;

	// 난수 시드. 같은 시드와 같은 설정이면 같은 배치를 재현한다.
	// 시도마다 시드에 고정 오프셋을 더해 다른 난수열을 사용한다.
	UPROPERTY(EditAnywhere, Category="Sampling")
	int32 Seed = 12345;

	// 활성점 하나에서 d~2d 구각 안에 시험할 후보 수(Bridson의 k).
	// 모두 실패하면 해당 활성점을 비활성화한다. 클수록 빈틈이 줄지만 느려진다.
	UPROPERTY(EditAnywhere, Category="Sampling", meta=(ClampMin="1", ClampMax="100"))
	int32 CandidatesPerPoint = 30;

	// 개수가 부족할 때 영역을 넓혀 다시 시도하는 최대 횟수(첫 시도 포함).
	// 확장 비율은 0부터 MaxExpansionRatio까지 시도 횟수에 따라 선형으로 증가한다. 1이면 확장하지 않는다.
	UPROPERTY(EditAnywhere, Category="Sampling", meta=(ClampMin="1", ClampMax="16"))
	int32 MaxAttempts = 3;

	// 최대 선형 확장 비율. 0.1이면 각 축을 최대 10% 확장한다.
	UPROPERTY(EditAnywhere, Category="Sampling", meta=(ClampMin="0", ClampMax="1"))
	float MaxExpansionRatio = 0.1f;

	// 모든 시도를 합친 후보 검사 총 예산. 초과하면 즉시 실패하고 결과를 적용하지 않는다.
	// 잘못된 설정으로 에디터가 오래 멈추는 것을 막기 위한 안전 상한이다.
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
