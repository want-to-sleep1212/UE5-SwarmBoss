#pragma once

#include "CoreMinimal.h"
#include "Swarm/SwarmPartTypes.h"

/** 구체 하나의 소속 도형 기준 위치·목표·남은 이동 시간과 활성 상태. */
struct FSwarmParticle
{
	FVector LocalPosition = FVector::ZeroVector;

	FVector TargetLocalPosition = FVector::ZeroVector;

	// 시간 지정 이동에서 목표 도착까지 남은 초. 0이면 기존 고정 속도 이동을 사용한다.
	float RemainingMoveTime = 0.0f;

	bool bActive = true;

	int32 InstanceIndex = INDEX_NONE;
	ESwarmPartId PartId = ESwarmPartId::Preview;
};

/** 군체 개체의 상태를 소유한다. 배열의 슬롯 번호가 개체 ID다. */
class FSwarmParticleStore
{
public:
	void Reset(int32 ExpectedParticleCount = 0);

	// PartId 도형의 LocalPositions를 Particles 배열 끝에 추가하고, 첫 번째 추가 원소의 배열 인덱스(ParticleId)를 반환한다.
	int32 AddParticles(ESwarmPartId PartId, TConstArrayView<FVector> LocalPositions);

	bool AssignTarget(int32 ParticleId, const FVector& TargetLocalPosition, float MoveDuration = 0.0f);
	void DeactivateParticle(int32 ParticleId);

	void UpdateParticlePart(int32 ParticleId, ESwarmPartId PartId, int32 InstanceIndex,
		const FVector& LocalPosition, const FVector& TargetLocalPosition);

	void SetInstanceIndex(int32 ParticleId, int32 InstanceIndex);
	void UpdateMovementProgress(int32 ParticleId, const FVector& LocalPosition, float RemainingMoveTime);

	// 제거된 구체의 데이터도 남아 있는 Particles 배열을 읽기 전용 참조로 반환한다. 살아 있는 구체는 bActive로 구분한다.
	const TArray<FSwarmParticle>& GetParticles() const { return Particles; }
	// 현재 활성 상태인 구체의 수를 반환한다.
	int32 GetActiveParticleCount() const { return ActiveParticleCount; }

private:
	TArray<FSwarmParticle> Particles;
	int32 ActiveParticleCount = 0;
};
