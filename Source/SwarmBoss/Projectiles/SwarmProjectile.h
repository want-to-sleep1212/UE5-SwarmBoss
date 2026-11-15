#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SwarmProjectile.generated.h"

class UStaticMeshComponent;

/**
 * 플레이어가 쏘는 관통 구체 투사체. 겉모습 반지름과 충돌 반지름이 같다.
 * 충돌 컴포넌트 없이 매 Tick 이번 이동 구간을 같은 반지름의 구로 Sweep 조회(SweepMultiByObjectType)하고,
 * 닿은 군체 구체(SwarmSphere)를 모두 제거한 채 멈추지 않고 계속 날아간다(관통 한도 없음).
 * 벽 등 WorldStatic·WorldDynamic에 닿으면 그 앞까지의 구체만 제거하고 사라진다.
 */
UCLASS()
class SWARMBOSS_API ASwarmProjectile : public AActor
{
	GENERATED_BODY()

public:
	ASwarmProjectile();

	// 발사 방향(정규화 전 가능), 속도(cm/s), 반지름(cm)을 정한다. 반지름은 겉모습과 충돌에 함께 쓴다. 생성 직후 한 번 호출한다.
	void Launch(const FVector& Direction, float Speed, float InRadius);

	// DeltaTime만큼 이동하면서 그 구간에 닿은 구체를 제거한다. 벽에 닿아 사라졌으면 true를 반환한다.
	// Tick이 호출하며, 자동 테스트에서 직접 호출해 시간 진행을 흉내 낸다.
	bool Advance(float DeltaTime);

	virtual void Tick(float DeltaTime) override;

	// 발사 후 지금까지 제거한 구체 수의 합계.
	int32 GetTotalRemovedCount() const { return TotalRemovedCount; }

private:
	UPROPERTY(VisibleAnywhere, Category="Projectile")
	TObjectPtr<UStaticMeshComponent> Mesh;

	FVector Velocity = FVector::ZeroVector;
	float Radius = 0.0f;
	int32 TotalRemovedCount = 0;
};
