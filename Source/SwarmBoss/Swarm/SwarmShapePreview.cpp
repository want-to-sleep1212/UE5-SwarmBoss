// SwarmShapePreview.cpp
// ASwarmShapePreview 구현.
// - 생성자: 충돌 없는 ISM 컴포넌트를 루트로 만들고 엔진 기본 구체 메시(반지름 50cm)를 지정한다.
// - OnConstruction(): Actor Scale을 1로 고정한다. 도형 크기는 Settings로만 결정한다.
// - GeneratePreview(): 샘플러를 호출해 보고 문자열과 로그(LogSwarmShape)를 남기고, 성공 시에만 인스턴스를 교체한다.
//   실패하면 기존 미리보기를 유지한다. 구체 스케일은 ParticleRadius로만 정한다. BeginPlay에서도 한 번 실행한다.
// - ClearPreview(): 모든 인스턴스와 결과 값을 초기화한다.
// - ShowBounds(): 적용된 중심 허용 영역을 10초간 디버그 도형으로 표시한다.

#include "SwarmShapePreview.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwarmShape, Log, All);

ASwarmShapePreview::ASwarmShapePreview()
{
	PrimaryActorTick.bCanEverTick = false;

	Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Particles"));
	SetRootComponent(Instances);

	Instances->SetMobility(EComponentMobility::Movable);
	Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Instances->SetCanEverAffectNavigation(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded())
	{
		Instances->SetStaticMesh(Sphere.Object);
	}
}

void ASwarmShapePreview::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 크기는 Settings로 결정한다. Actor Scale로 구체까지 축소하지 않는다.
	SetActorScale3D(FVector::OneVector);
}

void ASwarmShapePreview::BeginPlay()
{
	Super::BeginPlay();

	GeneratePreview();
}

void ASwarmShapePreview::GeneratePreview()
{
	const FSwarmSamplingResult Result = FSwarmShapeSampler::GenerateTargets(Settings);
	LastReport = FString::Printf(TEXT("%s Requested=%d Produced=%d Attempts=%d Expansion=%.2f%% MinDistance=%.3f Measured=%.3f Tests=%d Time=%.2fms"),
		*Result.Message, Settings.ParticleCount, Result.Points.Num(), Result.Attempts, Result.ExpansionRatio * 100.0,
		Result.MinimumDistance, Result.MeasuredMinimumDistance, Result.CandidateTests, Result.ElapsedMilliseconds);

	if (!Result.bSuccess)
	{
		UE_LOG(LogSwarmShape, Warning, TEXT("%s Existing preview retained."), *LastReport);
		return;
	}

	Modify();
	Instances->Modify();

	TArray<FTransform> Transforms;
	Transforms.Reserve(Result.Points.Num());
	// Engine BasicShapes/Sphere는 반지름 50cm이다. 목표 도형 크기와 무관하게 고정한다.
	const FVector ParticleScale(Settings.ParticleRadius / 50.0);
	for (const FVector& Point : Result.Points)
	{
		Transforms.Emplace(FQuat::Identity, Point, ParticleScale);
	}

	Instances->ClearInstances();
	Instances->AddInstances(Transforms, false, false, false);

	GeneratedCount = Result.Points.Num();
	AppliedHalfExtents = Result.HalfExtents;
	AppliedShape = Settings.Shape;

	UE_LOG(LogSwarmShape, Display, TEXT("%s"), *LastReport);
}

void ASwarmShapePreview::ClearPreview()
{
	Modify();
	Instances->Modify();

	Instances->ClearInstances();

	GeneratedCount = 0;
	AppliedHalfExtents = FVector::ZeroVector;
	LastReport = TEXT("Preview cleared.");
}

void ASwarmShapePreview::ShowBounds()
{
	if (!GetWorld() || GeneratedCount == 0)
	{
		return;
	}

	if (AppliedShape == ESwarmPrimitive::Sphere)
	{
		DrawDebugSphere(GetWorld(), GetActorLocation(), AppliedHalfExtents.X, 32, FColor::Cyan, false, 10.0f);
	}
	else
	{
		DrawDebugBox(GetWorld(), GetActorLocation(), AppliedHalfExtents, GetActorQuat(), FColor::Cyan, false, 10.0f);
	}
}
