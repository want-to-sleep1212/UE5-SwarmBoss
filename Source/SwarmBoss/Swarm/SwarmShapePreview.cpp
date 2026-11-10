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
