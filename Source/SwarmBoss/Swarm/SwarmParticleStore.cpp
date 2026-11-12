#include "SwarmParticleStore.h"

void FSwarmParticleStore::ResetFromPositions(TConstArrayView<FVector> LocalPositions)
{
	Particles.Reset(LocalPositions.Num());

	for (const FVector& LocalPosition : LocalPositions)
	{
		FSwarmParticle& Particle = Particles.AddDefaulted_GetRef();
		Particle.LocalPosition = LocalPosition;
		Particle.TargetLocalPosition = LocalPosition;
	}

	ActiveParticleCount = Particles.Num();
}

void FSwarmParticleStore::Clear()
{
	Particles.Reset();
	ActiveParticleCount = 0;
}

bool FSwarmParticleStore::AssignTarget(int32 ParticleId, const FVector& TargetLocalPosition)
{
	if (!Particles.IsValidIndex(ParticleId) || TargetLocalPosition.ContainsNaN())
	{
		return false;
	}

	FSwarmParticle& Particle = Particles[ParticleId];

	if (!Particle.bActive)
	{
		return false;
	}

	Particle.TargetLocalPosition = TargetLocalPosition;

	return true;
}

void FSwarmParticleStore::DeactivateParticle(int32 ParticleId)
{
	FSwarmParticle& Particle = Particles[ParticleId];

	if (!Particle.bActive)
	{
		return;
	}

	Particle.bActive = false;
	Particle.InstanceIndex = INDEX_NONE;
	--ActiveParticleCount;
}

void FSwarmParticleStore::SetInstanceIndex(int32 ParticleId, int32 InstanceIndex)
{
	Particles[ParticleId].InstanceIndex = InstanceIndex;
}

void FSwarmParticleStore::SetLocalPosition(int32 ParticleId, const FVector& LocalPosition)
{
	Particles[ParticleId].LocalPosition = LocalPosition;
}
