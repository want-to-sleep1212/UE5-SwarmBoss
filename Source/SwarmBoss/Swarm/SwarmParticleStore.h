#pragma once

#include "CoreMinimal.h"

/** 생성한 구체 하나의 도형 기준 위치와 초기 활성 상태. */
struct FSwarmParticle
{
	FVector LocalPosition = FVector::ZeroVector;

	FVector TargetLocalPosition = FVector::ZeroVector;

	bool bActive = true;

	int32 InstanceIndex = INDEX_NONE;
};

/** 군체 개체의 상태를 소유한다. 배열의 슬롯 번호가 개체 ID다. */
class FSwarmParticleStore
{
public:
	void ResetFromPositions(TConstArrayView<FVector> LocalPositions);
	void Clear();

	bool AssignTarget(int32 ParticleId, const FVector& TargetLocalPosition);
	void DeactivateParticle(int32 ParticleId);

	void SetInstanceIndex(int32 ParticleId, int32 InstanceIndex);
	void SetLocalPosition(int32 ParticleId, const FVector& LocalPosition);

	const TArray<FSwarmParticle>& GetParticles() const { return Particles; }
	int32 GetActiveParticleCount() const { return ActiveParticleCount; }

private:
	TArray<FSwarmParticle> Particles;
	int32 ActiveParticleCount = 0;
};
