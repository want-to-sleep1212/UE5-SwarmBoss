// SwarmShapeSamplerTests.cpp
// 1단계 Unreal Automation 테스트(SwarmBoss.Stage1.*). 개발 빌드(WITH_DEV_AUTOMATION_TESTS)에서만 컴파일된다.
// - SamplingInvariants: 구·박스 × 시드 3개 × 개수 3개 조합의 성공, 개수, 간격·경계, 확장 상한.
// - DeterminismAndScale: 같은 시드의 재현성, 간격 2배 → 크기 2배, 개수 8배 → 크기 2배.
// - EdgeCasesAndFailure: 0개·1개 처리, 잘못된 설정 거부, 후보 예산·시도 상한 준수, 검증기의 겹침·경계 위반 검출.
// - PreviewInstances: 임시 월드에서 미리보기 Actor의 인스턴스 수, 구체 스케일, 실패 시 보존, 초기화 동작.

#include "SwarmShapeSampler.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SwarmShapePreview.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmSamplingInvariantTest, "SwarmBoss.Stage1.SamplingInvariants",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSwarmSamplingInvariantTest::RunTest(const FString& Parameters)
{
	for (ESwarmPrimitive Shape : { ESwarmPrimitive::Sphere, ESwarmPrimitive::Box })
	for (int32 Seed : { 1, 42, 12345 })
	for (int32 Count : { 2, 50, 300 })
	{
		FSwarmSamplingSettings Settings;
		Settings.Shape = Shape;
		Settings.Seed = Seed;
		Settings.ParticleCount = Count;

		const FSwarmSamplingResult Result = FSwarmShapeSampler::GenerateTargets(Settings);
		const FString Context = FString::Printf(TEXT("Shape=%d Seed=%d N=%d"), int32(Shape), Seed, Count);

		TestTrue(Context + TEXT(" success: ") + Result.Message, Result.bSuccess);
		TestEqual(Context + TEXT(" count"), Result.Points.Num(), Count);

		double MinDistance;
		TestTrue(Context + TEXT(" spacing and bounds"), FSwarmShapeSampler::ValidateTargets(Result.Points,
			Shape, Result.HalfExtents, 22.0, MinDistance));
		TestTrue(Context + TEXT(" expansion cap"), Result.ExpansionRatio <= Settings.MaxExpansionRatio + 1.e-6);

		AddInfo(FString::Printf(TEXT("%s attempts=%d expansion=%.3f ms=%.2f"), *Context,
			Result.Attempts, Result.ExpansionRatio, Result.ElapsedMilliseconds));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmSamplingScaleTest, "SwarmBoss.Stage1.DeterminismAndScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSwarmSamplingScaleTest::RunTest(const FString& Parameters)
{
	FSwarmSamplingSettings Settings;
	Settings.ParticleCount = 80;

	const auto A = FSwarmShapeSampler::GenerateTargets(Settings);
	const auto B = FSwarmShapeSampler::GenerateTargets(Settings);
	TestTrue(TEXT("Repeat succeeds"), A.bSuccess && B.bSuccess);
	TestTrue(TEXT("Same seed produces identical targets"), A.Points == B.Points);

	Settings.ParticleRadius *= 2;
	Settings.SpacingMargin *= 2;
	const auto C = FSwarmShapeSampler::GenerateTargets(Settings);
	TestTrue(TEXT("Scaled succeeds"), C.bSuccess);
	TestTrue(TEXT("Double spacing doubles dimensions"), C.HalfExtents.Equals(A.HalfExtents * 2.0, 1.e-5));

	Settings.ParticleCount = 640;
	const auto D = FSwarmShapeSampler::GenerateTargets(Settings);
	TestTrue(TEXT("Eightfold count succeeds"), D.bSuccess);
	TestTrue(TEXT("Eightfold count doubles initial dimensions"),
		(D.HalfExtents / (1.0 + D.ExpansionRatio)).Equals(C.HalfExtents / (1.0 + C.ExpansionRatio) * 2.0, 1.e-5));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmSamplingFailureTest, "SwarmBoss.Stage1.EdgeCasesAndFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSwarmSamplingFailureTest::RunTest(const FString& Parameters)
{
	FSwarmSamplingSettings S;

	S.ParticleCount = 0;
	auto R = FSwarmShapeSampler::GenerateTargets(S);
	TestTrue(TEXT("Zero is empty success"), R.bSuccess && R.Points.IsEmpty());

	S.ParticleCount = 1;
	R = FSwarmShapeSampler::GenerateTargets(S);
	TestTrue(TEXT("One is centered"), R.bSuccess && R.Points.Num() == 1 && R.Points[0].IsZero());

	S.ParticleCount = -1;
	TestFalse(TEXT("Negative count rejected"), FSwarmShapeSampler::GenerateTargets(S).bSuccess);

	S.ParticleCount = 300;
	S.SpacingMargin = -1;
	TestFalse(TEXT("Negative spacing rejected"), FSwarmShapeSampler::GenerateTargets(S).bSuccess);

	S.SpacingMargin = 2;
	S.MaxCandidateTests = 100;
	R = FSwarmShapeSampler::GenerateTargets(S);
	TestFalse(TEXT("Budget exhaustion fails explicitly"), R.bSuccess);
	TestEqual(TEXT("Candidate budget respected"), R.CandidateTests, 100);

	S.MaxCandidateTests = 1000000;
	S.PackingCoefficient = 10;
	S.MaxExpansionRatio = 0;
	R = FSwarmShapeSampler::GenerateTargets(S);
	TestFalse(TEXT("Impossible density fails"), R.bSuccess);
	TestEqual(TEXT("Attempt cap respected"), R.Attempts, S.MaxAttempts);
	TestEqual(TEXT("No unrequested expansion"), R.ExpansionRatio, 0.0);

	double Minimum;
	TestFalse(TEXT("Validator rejects overlapping targets"), FSwarmShapeSampler::ValidateTargets(
		{ FVector::ZeroVector, FVector::ZeroVector }, ESwarmPrimitive::Sphere, FVector(100), 22, Minimum));
	TestFalse(TEXT("Validator rejects out-of-bounds targets"), FSwarmShapeSampler::ValidateTargets(
		{ FVector(101, 0, 0) }, ESwarmPrimitive::Box, FVector(100), 22, Minimum));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmPreviewTest, "SwarmBoss.Stage1.PreviewInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSwarmPreviewTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmShapePreview* Preview = World->SpawnActor<ASwarmShapePreview>();
	if (TestNotNull(TEXT("Preview actor"), Preview))
	{
		Preview->Settings.ParticleCount = 50;
		Preview->GeneratePreview();
		TestTrue(TEXT("Engine sphere mesh loaded"), Preview->Instances->GetStaticMesh() != nullptr);
		TestEqual(TEXT("Rendered instance count"), Preview->Instances->GetInstanceCount(), 50);

		FTransform Instance;
		TestTrue(TEXT("Instance transform exists"), Preview->Instances->GetInstanceTransform(0, Instance, false));
		TestTrue(TEXT("Particle scale independent of shape"), Instance.GetScale3D().Equals(FVector(0.2), 1.e-6));

		TestTrue(TEXT("Query-only profile"), Preview->Instances->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
		TestTrue(TEXT("SwarmSphere object type"), Preview->Instances->GetCollisionObjectType() == ECC_GameTraceChannel3);
		TestTrue(TEXT("PlayerProjectile overlap response"),
			Preview->Instances->GetCollisionResponseToChannel(ECC_GameTraceChannel2) == ECR_Overlap);
		TestFalse(TEXT("Null hit rejected"), Preview->HighlightInstance(nullptr, 0));
		TestFalse(TEXT("Negative index rejected"), Preview->HighlightInstance(Preview->Instances.Get(), -1));
		TestFalse(TEXT("Out-of-range index rejected"), Preview->HighlightInstance(Preview->Instances.Get(), 50));
		Preview->SetActorLocationAndRotation(FVector(100, 200, 300), FRotator(0, 90, 0));
		TestTrue(TEXT("Valid instance highlighted after transform"), Preview->HighlightInstance(Preview->Instances.Get(), 0));

		const int32 PreviousCount = Preview->Instances->GetInstanceCount();
		Preview->Settings.SpacingMargin = -1;
		AddExpectedError(TEXT("Invalid sampling settings."), EAutomationExpectedErrorFlags::Contains, 1);
		Preview->GeneratePreview();
		TestEqual(TEXT("Failure retains previous preview"), Preview->Instances->GetInstanceCount(), PreviousCount);

		Preview->ClearPreview();
		TestEqual(TEXT("Clear removes all instances"), Preview->Instances->GetInstanceCount(), 0);

		// 이동·회전된 Actor의 단일 구체를 엔진 Object Trace로 식별한다.
		Preview->Settings.SpacingMargin = 2;
		Preview->Settings.ParticleCount = 1;
		Preview->GeneratePreview();

		FCollisionObjectQueryParams ObjectQuery;
		ObjectQuery.AddObjectTypesToQuery(ECC_GameTraceChannel3);

		const FVector Center = Preview->GetActorLocation();
		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByObjectType(
			Hit,
			Center - FVector(100, 0, 0),
			Center + FVector(100, 0, 0),
			ObjectQuery);

		TestTrue(TEXT("Object trace hits query-only sphere"), bHit);
		TestTrue(TEXT("Trace identifies preview component"), Hit.GetComponent() == Preview->Instances.Get());
		TestEqual(TEXT("Trace returns instance zero"), Hit.Item, 0);
		TestTrue(TEXT("Trace result can be highlighted"), Preview->HighlightInstance(Hit.GetComponent(), Hit.Item));

		TestFalse(TEXT("Null removal target rejected"), Preview->RemoveInstance(nullptr, 0));
		TestFalse(TEXT("Negative removal index rejected"), Preview->RemoveInstance(Preview->Instances.Get(), -1));
		TestFalse(TEXT("Out-of-range removal rejected"), Preview->RemoveInstance(Preview->Instances.Get(), 1));
		TestEqual(TEXT("Invalid removal preserves instance"), Preview->Instances->GetInstanceCount(), 1);

		TestTrue(TEXT("Hit instance removed"), Preview->RemoveInstance(Hit.GetComponent(), Hit.Item));
		TestEqual(TEXT("No instance remains"), Preview->Instances->GetInstanceCount(), 0);
		TestEqual(TEXT("GeneratedCount follows actual instances"), Preview->GeneratedCount, 0);
		TestEqual(TEXT("Regeneration setting preserved"), Preview->Settings.ParticleCount, 1);

		FHitResult HitAfterRemoval;
		TestFalse(TEXT("Removed sphere no longer collides"), World->LineTraceSingleByObjectType(
			HitAfterRemoval,
			Center - FVector(100, 0, 0),
			Center + FVector(100, 0, 0),
			ObjectQuery));


	}

	World->DestroyWorld(false);
	return true;
}
#endif
