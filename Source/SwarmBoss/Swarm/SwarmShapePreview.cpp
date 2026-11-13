#include "SwarmShapePreview.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwarmShape, Log, All);

// 샘플러 결과를 표시할 ISM과 엔진 기본 구체 메시를 설정한다.
// 게임 피격 대상이 아니므로 충돌을 끄고 중앙 저장소나 Runtime을 만들지 않는다.
ASwarmShapePreview::ASwarmShapePreview()
{
	Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Particles"));
	SetRootComponent(Instances);

	Instances->SetMobility(EComponentMobility::Movable);
	Instances->SetCollisionProfileName(TEXT("NoCollision"));
	Instances->SetCanEverAffectNavigation(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded())
	{
		Instances->SetStaticMesh(Sphere.Object);
	}
}

// 배치 범위는 샘플러 설정으로 조정할 수 있도록 Preview Actor의 스케일을 1로 고정한다.
void ASwarmShapePreview::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 크기는 Settings로 결정한다. Actor Scale로 구체까지 축소하지 않는다.
	SetActorScale3D(FVector::OneVector);
}

// Play 시작 시 현재 설정으로 미리보기 구체를 생성한다.
void ASwarmShapePreview::BeginPlay()
{
	Super::BeginPlay();

	GeneratePreview();
}

// Settings의 개수와 간격 조건으로 구체 중심 좌표를 생성한다.
// 성공한 좌표로 미리보기 구체를 교체하고, 적용한 도형 크기와 생성 결과를 Details에 기록한다.
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

	TArray<FTransform> Transforms;
	Transforms.Reserve(Result.Points.Num());

	// Engine BasicShapes/Sphere의 반지름 50cm를 Settings.ParticleRadius에 맞춘다.
	const FVector ParticleScale(Settings.ParticleRadius / 50.0);

	for (const FVector& Point : Result.Points)
	{
		Transforms.Emplace(FQuat::Identity, Point, ParticleScale);
	}

	Modify();
	Instances->Modify();
	Instances->ClearInstances();
	Instances->AddInstances(Transforms, /*bShouldReturnIndices=*/false,
		/*bWorldSpace=*/false, /*bUpdateNavigation=*/false);

	GeneratedCount = Instances->GetInstanceCount();
	AppliedHalfExtents = Result.HalfExtents;
	AppliedShape = Settings.Shape;

	UE_LOG(LogSwarmShape, Display, TEXT("%s"), *LastReport);
}

// 미리보기 메시를 모두 지우고 Details의 생성 개수와 도형 크기를 0으로 갱신한다.
// 에디터 변경 기록을 위해 Actor와 ISM에 Modify()를 호출한다.
void ASwarmShapePreview::ClearPreview()
{
	Modify();
	Instances->Modify();
	Instances->ClearInstances();

	GeneratedCount = 0;
	AppliedHalfExtents = FVector::ZeroVector;
	LastReport = TEXT("Preview cleared.");
}

// 마지막 생성에서 구체 중심을 배치한 영역을 청록색 구나 박스로 10초간 표시한다.
// 영역의 크기는 AppliedHalfExtents를 사용하고 위치와 회전은 Preview Actor를 따른다.
void ASwarmShapePreview::ShowBounds()
{
	if (!GetWorld() || Instances->GetInstanceCount() == 0)
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
