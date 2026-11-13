#include "SwarmParticleStore.h"

// Particles 배열을 비우고 ActiveParticleCount를 0으로 만든다.
// ExpectedParticleCount개의 구체를 담을 메모리를 확보하며, 구체 데이터는 이후 AddParticles()에서 채운다.
void FSwarmParticleStore::Reset(int32 ExpectedParticleCount)
{
	Particles.Reset(ExpectedParticleCount);
	ActiveParticleCount = 0;
}

// LocalPositions의 좌표마다 FSwarmParticle 하나를 Particles 배열 끝에 추가한다.
// 각 구체의 소속을 PartId로, 현재 위치와 목표 위치를 받은 좌표로 설정하고 생존 수를 늘린다.
// 반환값은 첫 번째로 추가한 원소의 배열 인덱스이며, 이 인덱스를 ParticleId로 사용한다.
int32 FSwarmParticleStore::AddParticles(ESwarmPartId PartId, TConstArrayView<FVector> LocalPositions)
{
	const int32 FirstParticleId = Particles.AddDefaulted(LocalPositions.Num());

	for (int32 PointIndex = 0; PointIndex < LocalPositions.Num(); ++PointIndex)
	{
		FSwarmParticle& Particle = Particles[FirstParticleId + PointIndex];
		Particle.PartId = PartId;
		Particle.LocalPosition = LocalPositions[PointIndex];
		Particle.TargetLocalPosition = Particle.LocalPosition;
	}

	ActiveParticleCount += LocalPositions.Num();

	return FirstParticleId;
}

// Particles[ParticleId]가 살아 있고 좌표가 유효하면 TargetLocalPosition을 갱신한다.
// 목표는 소속 도형의 로컬 좌표다. MoveDuration은 도착까지의 초이며 0이면 기존 고정 속도 이동을 사용한다.
// 새 목표를 받으면 현재 위치 기준으로 남은 시간을 다시 설정한다. ID·좌표·시간이 잘못됐거나 비활성이면 false를 반환한다.
bool FSwarmParticleStore::AssignTarget(int32 ParticleId, const FVector& TargetLocalPosition, float MoveDuration)
{
	if (!Particles.IsValidIndex(ParticleId) || TargetLocalPosition.ContainsNaN()
		|| !FMath::IsFinite(MoveDuration) || MoveDuration < 0.0f)
	{
		return false;
	}

	FSwarmParticle& Particle = Particles[ParticleId];

	if (!Particle.bActive)
	{
		return false;
	}

	Particle.TargetLocalPosition = TargetLocalPosition;
	Particle.RemainingMoveTime = Particle.LocalPosition == TargetLocalPosition ? 0.0f : MoveDuration;

	return true;
}

// Particles[ParticleId]를 배열에 남겨 둔 채 bActive를 false, InstanceIndex를 INDEX_NONE으로 바꾼다.
// 남은 이동 시간을 0으로 지우고 처음 제거할 때만 생존 수를 줄인다. 다른 구체의 ParticleId는 밀리지 않는다.
void FSwarmParticleStore::DeactivateParticle(int32 ParticleId)
{
	FSwarmParticle& Particle = Particles[ParticleId];

	if (!Particle.bActive)
	{
		return;
	}

	Particle.bActive = false;
	Particle.InstanceIndex = INDEX_NONE;
	Particle.RemainingMoveTime = 0.0f;
	--ActiveParticleCount;
}

// Manager가 양쪽 ISM의 소속 변경을 마친 뒤, 검증된 활성 ParticleId의 소속과 목적지 인스턴스 번호를 기록한다.
// 두 좌표는 새 부위 ISM 기준이다. ParticleId, bActive, 전체 생존 수와 남은 이동 시간은 유지한다.
void FSwarmParticleStore::UpdateParticlePart(int32 ParticleId, ESwarmPartId PartId, int32 InstanceIndex,
	const FVector& LocalPosition, const FVector& TargetLocalPosition)
{
	check(Particles.IsValidIndex(ParticleId) && Particles[ParticleId].bActive);

	FSwarmParticle& Particle = Particles[ParticleId];
	Particle.PartId = PartId;
	Particle.InstanceIndex = InstanceIndex;
	Particle.LocalPosition = LocalPosition;
	Particle.TargetLocalPosition = TargetLocalPosition;
}

void FSwarmParticleStore::SetInstanceIndex(int32 ParticleId, int32 InstanceIndex)
{
	Particles[ParticleId].InstanceIndex = InstanceIndex;
}

// ISM 위치 갱신에 성공한 활성 구체의 현재 위치와 남은 이동 시간(초)을 함께 기록한다.
// LocalPosition은 해당 구체의 소속 ISM 기준이며, 목표 도착 시 RemainingMoveTime은 0이다.
void FSwarmParticleStore::UpdateMovementProgress(int32 ParticleId, const FVector& LocalPosition, float RemainingMoveTime)
{
	FSwarmParticle& Particle = Particles[ParticleId];
	Particle.LocalPosition = LocalPosition;
	Particle.RemainingMoveTime = RemainingMoveTime;
}
