#include "Projectiles/SwarmProjectile.h"

#include "Boss/SwarmBossManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "SwarmBoss.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// 발사 후 벽에 닿지 않으면 사라지기까지의 시간(초).
	constexpr float ProjectileLifeSeconds = 3.0f;

	// 엔진 기본 구 메시(/Engine/BasicShapes/Sphere)의 반지름(cm). 반지름을 이 값으로 나눠 메시 배율을 정한다.
	constexpr float EngineSphereMeshRadius = 50.0f;
}

// 충돌 없는 구 메시 하나로 구성한다. 충돌은 Advance()의 Sweep 조회가 처리한다.
ASwarmProjectile::ASwarmProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	InitialLifeSpan = ProjectileLifeSeconds;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	SetRootComponent(Mesh);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	if (Sphere.Succeeded())
	{
		Mesh->SetStaticMesh(Sphere.Object);
	}
}

// 이동 속도와 반지름을 정하고, 겉모습 크기를 충돌 반지름에 맞춘다.
void ASwarmProjectile::Launch(const FVector& Direction, float Speed, float InRadius)
{
	Velocity = Direction.GetSafeNormal() * FMath::Max(0.0f, Speed);
	Radius = FMath::Max(0.0f, InRadius);
	Mesh->SetRelativeScale3D(FVector(Radius / EngineSphereMeshRadius));
}

void ASwarmProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	Advance(DeltaTime);
}

// 이번 프레임 이동 구간을 반지름 Radius 구로 Sweep 조회해 닿은 것을 모두 받는다. 엔진 물리 장면의 공간 자료구조가
// 경로 근처의 충돌 바디만 찾아 준다. 결과를 가까운 순으로 보며 벽(군체 Manager가 아닌 것)이 나오면 거기서 멈추고,
// 그 앞까지 닿은 군체 구체만 Manager별로 모아 제거한다. 벽이 없으면 구간 끝으로 이동해 계속 날아간다.
bool ASwarmProjectile::Advance(float DeltaTime)
{
	UWorld* World = GetWorld();

	if (!World || IsActorBeingDestroyed() || DeltaTime <= 0.0f || Velocity.IsNearlyZero())
	{
		return false;
	}

	const FVector Start = GetActorLocation();
	const FVector End = Start + Velocity * DeltaTime;

	FCollisionObjectQueryParams ObjectQuery;
	ObjectQuery.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectQuery.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjectQuery.AddObjectTypesToQuery(ECC_GameTraceChannel3);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SwarmProjectileSweep), false, this);

	if (AActor* OwnerActor = GetOwner())
	{
		QueryParams.AddIgnoredActor(OwnerActor);
	}

	if (APawn* InstigatorPawn = GetInstigator())
	{
		QueryParams.AddIgnoredActor(InstigatorPawn);
	}

	TArray<FHitResult> Hits;
	World->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, ObjectQuery,
		FCollisionShape::MakeSphere(Radius), QueryParams);

	Hits.StableSort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });

	TMap<ASwarmBossManager*, TArray<FHitResult>> SwarmHits;
	const FHitResult* WallHit = nullptr;

	for (const FHitResult& Hit : Hits)
	{
		if (ASwarmBossManager* Boss = Cast<ASwarmBossManager>(Hit.GetActor()))
		{
			SwarmHits.FindOrAdd(Boss).Add(Hit);
		}
		else
		{
			WallHit = &Hit;
			break;
		}
	}

	for (TPair<ASwarmBossManager*, TArray<FHitResult>>& Pair : SwarmHits)
	{
		// 이 보스를 처음 맞혔으면 제거 전에 피해 구간을 열어, 이번 통과에서 지우는 구체를 모두 한 번의 재분배로 묶는다.
		if (!OpenWindowBosses.Contains(Pair.Key))
		{
			Pair.Key->BeginDamageWindow();
			OpenWindowBosses.Add(Pair.Key);
		}

		const int32 RemovedCount = Pair.Key->RemoveHitInstances(Pair.Value);
		TotalRemovedCount += RemovedCount;

		if (RemovedCount > 0)
		{
			UE_LOG(LogSwarmBoss, Display, TEXT("Projectile passed through %s: Removed=%d Total=%d Remaining=%d"),
				*Pair.Key->GetName(), RemovedCount, TotalRemovedCount, Pair.Key->GetActiveParticleCount());
		}
	}

	if (WallHit)
	{
		// Destroy()가 Destroyed()·EndPlay()를 거쳐 열린 피해 구간을 모두 닫는다.
		SetActorLocation(WallHit->Location);
		Destroy();
		return true;
	}

	SetActorLocation(End);

	// 이번 구간에서 맞히지 않았고 투사체 구가 보스 경계 상자와 겹치지 않으면 통과가 끝난 것으로 보고 구간을 닫는다.
	// 경계 상자 안의 빈틈(관절 Gap 등)에서는 닫지 않는다. 맞힌 프레임에 닫지 않는 이유는, 직전 재분배로 아직
	// 이동 중인 구체가 상자 밖에 잠깐 있을 수 있어 그 구체를 맞히는 동안 구간이 열리고 닫히길 반복하지 않게 하기 위해서다.
	for (int32 Index = OpenWindowBosses.Num() - 1; Index >= 0; --Index)
	{
		ASwarmBossManager* Boss = OpenWindowBosses[Index].Get();

		if (!Boss)
		{
			OpenWindowBosses.RemoveAt(Index);
			continue;
		}

		if (!SwarmHits.Contains(Boss) && !Boss->GetSwarmBounds().ExpandBy(Radius).IsInside(End))
		{
			Boss->EndDamageWindow();
			OpenWindowBosses.RemoveAt(Index);
		}
	}

	return false;
}

// 벽·바닥 충돌, 수명 종료, 레벨 종료 등 어떤 이유로 사라지든 아직 닫지 않은 피해 구간을 모두 닫는다.
// 닫지 않으면 그 보스의 열린 구간 수가 0으로 돌아오지 않아 손실 재분배가 영영 일어나지 않는다.
// 파괴는 Destroyed(), 레벨 언로드 등은 EndPlay()로 오므로 둘 다에서 닫는다. BeginPlay 전의 Actor는 EndPlay가 불리지 않는다.
void ASwarmProjectile::CloseAllDamageWindows()
{
	for (const TWeakObjectPtr<ASwarmBossManager>& Boss : OpenWindowBosses)
	{
		if (Boss.IsValid())
		{
			Boss->EndDamageWindow();
		}
	}

	OpenWindowBosses.Reset();
}

void ASwarmProjectile::Destroyed()
{
	CloseAllDamageWindows();
	Super::Destroyed();
}

void ASwarmProjectile::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CloseAllDamageWindows();
	Super::EndPlay(EndPlayReason);
}
