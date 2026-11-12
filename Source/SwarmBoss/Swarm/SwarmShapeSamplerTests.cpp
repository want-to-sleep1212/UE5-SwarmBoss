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
		auto CheckInstanceMappings = [&]()
		{
			const int32 Count = Preview->Instances->GetInstanceCount();

			int32 ActiveCount = 0;
			TestEqual(TEXT("Reverse mapping count matches instances"), Preview->InstanceToParticle.Num(), Count);

			for (int32 ParticleId = 0; ParticleId < Preview->Particles.Num(); ++ParticleId)
			{
				const FSwarmParticle& Particle = Preview->Particles[ParticleId];

				if (!Particle.bActive)
				{
					TestEqual(TEXT("Inactive particle has no instance"), Particle.InstanceIndex, INDEX_NONE);
					continue;
				}

				++ActiveCount;

				if (!TestTrue(TEXT("Instance index has a reverse entry"),
					Preview->InstanceToParticle.IsValidIndex(Particle.InstanceIndex)))
				{
					continue;
				}

				TestEqual(TEXT("Reverse entry resolves the same particle"),
					Preview->InstanceToParticle[Particle.InstanceIndex], ParticleId);

				FTransform LocalTransform;
				if (TestTrue(TEXT("Mapped instance exists"),
					Preview->Instances->GetInstanceTransform(Particle.InstanceIndex, LocalTransform, false)))
				{
					TestTrue(TEXT("Mapped instance is at the particle position"),
						LocalTransform.GetLocation().Equals(Particle.LocalPosition, 1.e-4));
				}
			}

			TestEqual(TEXT("Active particle count matches instances"), ActiveCount, Count);
			TestEqual(TEXT("ActiveParticleCount matches live slots"), Preview->ActiveParticleCount, ActiveCount);
		};

		Preview->Settings.ParticleCount = 50;
		Preview->GeneratePreview();
		TestTrue(TEXT("Engine sphere mesh loaded"), Preview->Instances->GetStaticMesh() != nullptr);
		TestEqual(TEXT("Rendered instance count"), Preview->Instances->GetInstanceCount(), 50);
		CheckInstanceMappings();

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
		CheckInstanceMappings();

		// 중간 번호를 지워 실제로 마지막 구체의 번호가 바뀌는 경우를 확인한다.
		const int32 MiddleIndex = PreviousCount / 2;
		const int32 RemovedParticleId = Preview->InstanceToParticle[MiddleIndex];
		const int32 MovedParticleId = Preview->InstanceToParticle.Last();

		TestTrue(TEXT("Middle instance removed"), Preview->RemoveInstance(Preview->Instances.Get(), MiddleIndex));
		TestFalse(TEXT("Removed particle is inactive"), Preview->Particles[RemovedParticleId].bActive);
		TestEqual(TEXT("Particle slots are preserved"), Preview->Particles.Num(), PreviousCount);
		TestEqual(TEXT("Last particle fills the gap"), Preview->InstanceToParticle[MiddleIndex], MovedParticleId);
		CheckInstanceMappings();

		Preview->Settings.SpacingMargin = 2;

		const FVector OriginalTarget = Preview->Particles[MovedParticleId].TargetLocalPosition;
		const FSwarmSamplingResult Reconstruction = Preview->GenerateReconstructionTargets();

		TestTrue(TEXT("Reconstruction samples surviving particles"), Reconstruction.bSuccess);
		TestEqual(TEXT("Reconstruction produces one target per survivor"), Reconstruction.Points.Num(), PreviousCount - 1);
		TestEqual(TEXT("Reconstruction preserves initial generation count"), Preview->Settings.ParticleCount, PreviousCount);
		TestTrue(TEXT("Sampling leaves the particle target unchanged"),
			Preview->Particles[MovedParticleId].TargetLocalPosition.Equals(OriginalTarget));
		CheckInstanceMappings();

		// 교환 삭제로 번호가 바뀐 개체에 목표를 배정한다.
		const FVector Start = Preview->Particles[MovedParticleId].LocalPosition;
		const FVector Target = Start + FVector(100, 0, 0);

		TestFalse(TEXT("Tick starts disabled"), Preview->IsActorTickEnabled());
		TestFalse(TEXT("Inactive particle cannot receive a target"), Preview->AssignParticleTarget(RemovedParticleId, Target));
		TestTrue(TEXT("Moved particle receives a target"), Preview->AssignParticleTarget(MovedParticleId, Target));
		TestTrue(TEXT("Target assignment enables Tick"), Preview->IsActorTickEnabled());

		Preview->Tick(0.25f);
		TestTrue(TEXT("Particle moves at a constant speed"),
			Preview->Particles[MovedParticleId].LocalPosition.Equals(Start + FVector(50, 0, 0), 1.e-4));
		TestTrue(TEXT("Tick stays enabled while moving"), Preview->IsActorTickEnabled());
		CheckInstanceMappings();

		Preview->Tick(1.0f);
		TestTrue(TEXT("Particle stops at target without overshooting"),
			Preview->Particles[MovedParticleId].LocalPosition.Equals(Target, 1.e-4));
		TestFalse(TEXT("Arrival disables Tick"), Preview->IsActorTickEnabled());
		CheckInstanceMappings();

		const FVector InactiveTarget = Preview->Particles[RemovedParticleId].TargetLocalPosition;

		TestFalse(TEXT("Wrong target count is rejected"), Preview->AssignReconstructionTargets({}));
		TestFalse(TEXT("Rejected assignment leaves Tick disabled"), Preview->IsActorTickEnabled());

		if (TestTrue(TEXT("Reconstruction targets are assigned"), Preview->AssignReconstructionTargets(Reconstruction.Points)))
		{
			int32 TargetIndex = 0;
			for (const FSwarmParticle& Particle : Preview->Particles)
			{
				if (Particle.bActive)
				{
					TestTrue(TEXT("Active particles receive targets in slot order"),
						Particle.TargetLocalPosition.Equals(Reconstruction.Points[TargetIndex]));
					++TargetIndex;
				}
			}
		}

		TestTrue(TEXT("Inactive particle keeps its target"),
			Preview->Particles[RemovedParticleId].TargetLocalPosition.Equals(InactiveTarget));
		CheckInstanceMappings();

		const FVector InitialHalfExtents = Preview->AppliedHalfExtents;

		for (int32 Index = Preview->Instances->GetInstanceCount() - 1; Index >= 14; --Index)
		{
			TestTrue(TEXT("Remove instance before reconstruction"), Preview->RemoveInstance(Preview->Instances.Get(), Index));
		}

		if (TestTrue(TEXT("Reconstruct 14 survivors"), Preview->ReconstructPreview()))
		{
			TestEqual(TEXT("Reconstruction preserves 14 live instances"), Preview->Instances->GetInstanceCount(), 14);
			TestTrue(TEXT("Displayed bounds shrink with the new target layout"),
				Preview->AppliedHalfExtents.GetMax() < InitialHalfExtents.GetMax());

			Preview->Tick(10.0f);
			TestFalse(TEXT("Survivors reach their reconstruction targets"), Preview->IsActorTickEnabled());

			for (const FSwarmParticle& Particle : Preview->Particles)
			{
				if (Particle.bActive)
				{
					TestTrue(TEXT("Reconstructed particle lies inside the new bounds"), FSwarmShapeSampler::Contains(
						Particle.LocalPosition, Preview->Settings.Shape, Preview->AppliedHalfExtents));
				}
			}

			CheckInstanceMappings();
		}

		Preview->ClearPreview();
		TestEqual(TEXT("Clear removes all instances"), Preview->Instances->GetInstanceCount(), 0);
		CheckInstanceMappings();

		// 이동·회전된 Actor의 단일 구체를 엔진 Object Trace로 식별한다.
		Preview->Settings.SpacingMargin = 2;
		Preview->Settings.ParticleCount = 1;
		Preview->GeneratePreview();
		CheckInstanceMappings();

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
		TestEqual(TEXT("No active particle remains"), Preview->ActiveParticleCount, 0);

		const FSwarmSamplingResult EmptyReconstruction = Preview->GenerateReconstructionTargets();
		TestTrue(TEXT("No survivors produce an empty target list"),
			EmptyReconstruction.bSuccess && EmptyReconstruction.Points.IsEmpty());
		TestEqual(TEXT("Regeneration setting preserved"), Preview->Settings.ParticleCount, 1);
		CheckInstanceMappings();

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
