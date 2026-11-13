// SwarmShapeSamplerTests.cpp
// 1단계 Unreal Automation 테스트(SwarmBoss.Stage1.*). 개발 빌드(WITH_DEV_AUTOMATION_TESTS)에서만 컴파일된다.
// - SamplingInvariants: 구·박스 × 시드 3개 × 개수 3개 조합의 성공, 개수, 간격·경계, 확장 상한.
// - DeterminismAndScale: 같은 시드의 재현성, 간격 2배 → 크기 2배, 개수 8배 → 크기 2배.
// - EdgeCasesAndFailure: 0개·1개 처리, 잘못된 설정 거부, 후보 예산·시도 상한 준수, 검증기의 겹침·경계 위반 검출.
// - PreviewInstances: 임시 월드에서 미리보기 Actor의 인스턴스 수, 구체 스케일, 실패 시 보존, 초기화 동작.

#include "SwarmShapeSampler.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include <limits>
#include "Boss/SwarmBossManager.h"
#include "SwarmShapePreview.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"

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

// 표시 전용 Preview의 생성 개수, 스케일, 충돌 비활성화, 실패 시 보존과 전체 삭제를 검증한다.
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

		TestEqual(TEXT("Rendered count"), Preview->Instances->GetInstanceCount(), 50);
		TestEqual(TEXT("Displayed generated count"), Preview->GeneratedCount, 50);
		TestFalse(TEXT("Preview has no tick"), Preview->PrimaryActorTick.bCanEverTick);
		TestTrue(TEXT("Preview collision disabled"), Preview->Instances->GetCollisionEnabled() == ECollisionEnabled::NoCollision);

		FTransform Instance;
		TestTrue(TEXT("Instance exists"), Preview->Instances->GetInstanceTransform(0, Instance, false));
		TestTrue(TEXT("Particle radius preserved"), Instance.GetScale3D().Equals(FVector(0.2), 1.e-6));
		const FVector OriginalBounds = Preview->AppliedHalfExtents;

		Preview->Settings.SpacingMargin = -1;
		AddExpectedError(TEXT("Invalid sampling settings."), EAutomationExpectedErrorFlags::Contains, 1);
		Preview->GeneratePreview();
		TestEqual(TEXT("Failure retains instances"), Preview->Instances->GetInstanceCount(), 50);
		TestEqual(TEXT("Failure retains count"), Preview->GeneratedCount, 50);
		TestTrue(TEXT("Failure retains bounds"), Preview->AppliedHalfExtents.Equals(OriginalBounds));

		Preview->Settings.SpacingMargin = 2;
		Preview->Settings.ParticleCount = 1;
		Preview->GeneratePreview();
		TestEqual(TEXT("Regeneration replaces instances"), Preview->Instances->GetInstanceCount(), 1);
		Preview->SetActorLocationAndRotation(FVector(100, 200, 300), FRotator(0, 90, 0));
		TestTrue(TEXT("World transform exists"), Preview->Instances->GetInstanceTransform(0, Instance, true));
		TestTrue(TEXT("Preview follows actor"), Instance.GetLocation().Equals(Preview->GetActorLocation()));
		Preview->ShowBounds();

		Preview->ClearPreview();
		TestEqual(TEXT("Clear removes instances"), Preview->Instances->GetInstanceCount(), 0);
		TestEqual(TEXT("Clear resets count"), Preview->GeneratedCount, 0);
		TestTrue(TEXT("Clear resets bounds"), Preview->AppliedHalfExtents.IsZero());

		Preview->Settings.ParticleCount = 0;
		Preview->GeneratePreview();
		TestEqual(TEXT("Zero remains empty"), Preview->Instances->GetInstanceCount(), 0);
	}

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmManagerBehaviorTest, "SwarmBoss.Stage2.ManagerBehavior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 임시 Manager에서 교환 삭제, 연결 정보, 이동, 축소 재구성, 엔진 피격과 초기 전체 개수 배분을 검증한다.
bool FSwarmManagerBehaviorTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();
	if (TestNotNull(TEXT("Boss manager"), Boss))
	{
		FSwarmSamplingSettings Settings;
		FVector AppliedHalfExtents = FVector::ZeroVector;
		UInstancedStaticMeshComponent* Instances = Boss->TorsoInstances.Get();

		// 몸통에만 샘플러 결과를 넣고 왼팔은 비워서 기존 단일 도형 실행 검증을 유지한다.
		auto Generate = [&]()
		{
			const FSwarmSamplingResult Result = FSwarmShapeSampler::GenerateTargets(Settings);
			if (Result.bSuccess)
			{
				const FSwarmPartPlacement Placement[] =
				{
					{ESwarmPartId::Torso, Result.Points},
					{ESwarmPartId::LeftArm, TConstArrayView<FVector>()}
				};
				TestTrue(TEXT("Manager initialization succeeds"), Boss->InitializeParticles(Placement, Settings.ParticleRadius));
				AppliedHalfExtents = Result.HalfExtents;
			}
		};

		// 초기 설정을 보존하고 생존 수만 복사본에 적용해 재구성 좌표를 계산한다.
		auto GenerateReconstructionTargets = [&]()
		{
			FSwarmSamplingSettings ReconstructionSettings = Settings;
			ReconstructionSettings.ParticleCount = Boss->GetActiveParticleCount();
			return FSwarmShapeSampler::GenerateTargets(ReconstructionSettings);
		};

		// 생성과 목표 배정이 성공한 경우에만 검증할 도형 경계를 변경한다.
		auto Reconstruct = [&]()
		{
			const FSwarmSamplingResult Result = GenerateReconstructionTargets();
			if (!Result.bSuccess || !Boss->AssignReconstructionTargets(Result.Points))
			{
				return false;
			}

			AppliedHalfExtents = Result.HalfExtents;
			return true;
		};

		const TArray<int32>& InstanceToParticle =
			Boss->Parts.FindChecked(ESwarmPartId::Torso).InstanceToParticle;

		auto CheckInstanceMappings = [&]()
		{
			const int32 Count = Instances->GetInstanceCount();

			int32 ActiveCount = 0;
			TestEqual(TEXT("Reverse mapping count matches instances"), InstanceToParticle.Num(), Count);

			for (int32 ParticleId = 0; ParticleId < Boss->GetParticles().Num(); ++ParticleId)
			{
				const FSwarmParticle& Particle = Boss->GetParticles()[ParticleId];

				if (!Particle.bActive)
				{
					TestEqual(TEXT("Inactive particle has no instance"), Particle.InstanceIndex, INDEX_NONE);
					continue;
				}

				++ActiveCount;

				if (!TestTrue(TEXT("Instance index has a reverse entry"),
					InstanceToParticle.IsValidIndex(Particle.InstanceIndex)))
				{
					continue;
				}

				TestEqual(TEXT("Reverse entry resolves the same particle"),
					InstanceToParticle[Particle.InstanceIndex], ParticleId);

				FTransform LocalTransform;
				if (TestTrue(TEXT("Mapped instance exists"),
					Instances->GetInstanceTransform(Particle.InstanceIndex, LocalTransform, false)))
				{
					TestTrue(TEXT("Mapped instance is at the particle position"),
						LocalTransform.GetLocation().Equals(Particle.LocalPosition, 1.e-4));
				}
			}

			TestEqual(TEXT("Active particle count matches instances"), ActiveCount, Count);
			TestEqual(TEXT("Stored active count matches live slots"), Boss->GetActiveParticleCount(), ActiveCount);
		};

		Settings.ParticleCount = 50;
		Generate();
		TestTrue(TEXT("Engine sphere mesh loaded"), Instances->GetStaticMesh() != nullptr);
		TestEqual(TEXT("Rendered instance count"), Instances->GetInstanceCount(), 50);
		CheckInstanceMappings();

		FTransform Instance;
		TestTrue(TEXT("Instance transform exists"), Instances->GetInstanceTransform(0, Instance, false));
		TestTrue(TEXT("Particle scale independent of shape"), Instance.GetScale3D().Equals(FVector(0.2), 1.e-6));

		TestTrue(TEXT("Query-only profile"), Instances->GetCollisionEnabled() == ECollisionEnabled::QueryOnly);
		TestTrue(TEXT("SwarmSphere object type"), Instances->GetCollisionObjectType() == ECC_GameTraceChannel3);
		TestTrue(TEXT("PlayerProjectile overlap response"),
			Instances->GetCollisionResponseToChannel(ECC_GameTraceChannel2) == ECR_Overlap);
		Boss->SetActorLocationAndRotation(FVector(100, 200, 300), FRotator(0, 90, 0));

		const int32 PreviousCount = Instances->GetInstanceCount();
		Settings.SpacingMargin = -1;
		Generate();
		TestEqual(TEXT("Failure retains previous preview"), Instances->GetInstanceCount(), PreviousCount);
		CheckInstanceMappings();

		// 중간 번호를 지워 실제로 마지막 구체의 번호가 바뀌는 경우를 확인한다.
		const int32 MiddleIndex = PreviousCount / 2;
		const int32 RemovedParticleId = InstanceToParticle[MiddleIndex];
		const int32 MovedParticleId = InstanceToParticle.Last();

		TestTrue(TEXT("Middle instance removed"), Boss->RemoveInstance(Instances, MiddleIndex));
		TestFalse(TEXT("Removed particle is inactive"), Boss->GetParticles()[RemovedParticleId].bActive);
		TestEqual(TEXT("Particle slots are preserved"), Boss->GetParticles().Num(), PreviousCount);
		TestEqual(TEXT("Last particle fills the gap"), InstanceToParticle[MiddleIndex], MovedParticleId);
		CheckInstanceMappings();

		Settings.SpacingMargin = 2;

		const FVector OriginalTarget = Boss->GetParticles()[MovedParticleId].TargetLocalPosition;
		const FSwarmSamplingResult Reconstruction = GenerateReconstructionTargets();

		TestTrue(TEXT("Reconstruction samples surviving particles"), Reconstruction.bSuccess);
		TestEqual(TEXT("Reconstruction produces one target per survivor"), Reconstruction.Points.Num(), PreviousCount - 1);
		TestEqual(TEXT("Reconstruction preserves initial generation count"), Settings.ParticleCount, PreviousCount);
		TestTrue(TEXT("Sampling leaves the particle target unchanged"),
			Boss->GetParticles()[MovedParticleId].TargetLocalPosition.Equals(OriginalTarget));
		CheckInstanceMappings();

		// 교환 삭제로 번호가 바뀐 개체에 목표를 배정한다.
		const FVector Start = Boss->GetParticles()[MovedParticleId].LocalPosition;
		const FVector Target = Start + FVector(100, 0, 0);

		TestFalse(TEXT("Actor tick starts disabled"), Boss->IsActorTickEnabled());
		TestFalse(TEXT("Inactive particle cannot receive a target"), Boss->AssignParticleTarget(RemovedParticleId, Target));
		TestTrue(TEXT("Moved particle receives a target"), Boss->AssignParticleTarget(MovedParticleId, Target));
		TestTrue(TEXT("Target assignment enables Tick"), Boss->IsActorTickEnabled());

		Boss->Tick(0.25f);
		TestTrue(TEXT("Particle moves at a constant speed"),
			Boss->GetParticles()[MovedParticleId].LocalPosition.Equals(Start + FVector(50, 0, 0), 1.e-4));
		TestTrue(TEXT("Tick stays enabled while moving"), Boss->IsActorTickEnabled());
		CheckInstanceMappings();

		Boss->Tick(1.0f);
		TestTrue(TEXT("Particle stops at target without overshooting"),
			Boss->GetParticles()[MovedParticleId].LocalPosition.Equals(Target, 1.e-4));
		TestFalse(TEXT("Arrival disables Tick"), Boss->IsActorTickEnabled());
		CheckInstanceMappings();

		const FVector InactiveTarget = Boss->GetParticles()[RemovedParticleId].TargetLocalPosition;

		TestFalse(TEXT("Wrong target count is rejected"), Boss->AssignReconstructionTargets({}));
		TestFalse(TEXT("Rejected assignment leaves Tick disabled"), Boss->IsActorTickEnabled());

		if (TestTrue(TEXT("Reconstruction targets are assigned"), Boss->AssignReconstructionTargets(Reconstruction.Points)))
		{
			int32 TargetIndex = 0;
			for (const FSwarmParticle& Particle : Boss->GetParticles())
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
			Boss->GetParticles()[RemovedParticleId].TargetLocalPosition.Equals(InactiveTarget));
		CheckInstanceMappings();

		const FVector InitialHalfExtents = AppliedHalfExtents;

		for (int32 Index = Instances->GetInstanceCount() - 1; Index >= 14; --Index)
		{
			TestTrue(TEXT("Remove instance before reconstruction"), Boss->RemoveInstance(Instances, Index));
		}

		if (TestTrue(TEXT("Reconstruct 14 survivors"), Reconstruct()))
		{
			TestEqual(TEXT("Reconstruction preserves 14 live instances"), Instances->GetInstanceCount(), 14);
			TestTrue(TEXT("Displayed bounds shrink with the new target layout"),
				AppliedHalfExtents.GetMax() < InitialHalfExtents.GetMax());

			Boss->Tick(10.0f);
			TestFalse(TEXT("Survivors reach their reconstruction targets"), Boss->IsActorTickEnabled());

			for (const FSwarmParticle& Particle : Boss->GetParticles())
			{
				if (Particle.bActive)
				{
					TestTrue(TEXT("Reconstructed particle lies inside the new bounds"), FSwarmShapeSampler::Contains(
						Particle.LocalPosition, Settings.Shape, AppliedHalfExtents));
				}
			}

			CheckInstanceMappings();
		}

		Boss->ResetParticles(0);
		TestEqual(TEXT("Clear removes all instances"), Instances->GetInstanceCount(), 0);
		CheckInstanceMappings();

		// 이동·회전된 Actor의 단일 구체를 엔진 Object Trace로 식별한다.
		Settings.SpacingMargin = 2;
		Settings.ParticleCount = 1;
		Generate();
		CheckInstanceMappings();

		FCollisionObjectQueryParams ObjectQuery;
		ObjectQuery.AddObjectTypesToQuery(ECC_GameTraceChannel3);

		const FVector Center = Boss->GetActorLocation();
		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByObjectType(
			Hit,
			Center - FVector(100, 0, 0),
			Center + FVector(100, 0, 0),
			ObjectQuery);

		TestTrue(TEXT("Object trace hits query-only sphere"), bHit);
		TestTrue(TEXT("Trace identifies preview component"), Hit.GetComponent() == Instances);
		TestEqual(TEXT("Trace returns instance zero"), Hit.Item, 0);

		TestFalse(TEXT("Null removal target rejected"), Boss->RemoveInstance(nullptr, 0));
		TestFalse(TEXT("Negative removal index rejected"), Boss->RemoveInstance(Instances, -1));
		TestFalse(TEXT("Out-of-range removal rejected"), Boss->RemoveInstance(Instances, 1));
		TestEqual(TEXT("Invalid removal preserves instance"), Instances->GetInstanceCount(), 1);

		TestTrue(TEXT("Hit instance removed"), Boss->RemoveInstance(Hit.GetComponent(), Hit.Item));
		TestEqual(TEXT("No instance remains"), Instances->GetInstanceCount(), 0);
		TestEqual(TEXT("No active particle remains"), Boss->GetActiveParticleCount(), 0);

		const FSwarmSamplingResult EmptyReconstruction = GenerateReconstructionTargets();
		TestTrue(TEXT("No survivors produce an empty target list"),
			EmptyReconstruction.bSuccess && EmptyReconstruction.Points.IsEmpty());
		TestEqual(TEXT("Regeneration setting preserved"), Settings.ParticleCount, 1);
		CheckInstanceMappings();

		FHitResult HitAfterRemoval;
		TestFalse(TEXT("Removed sphere no longer collides"), World->LineTraceSingleByObjectType(
			HitAfterRemoval,
			Center - FVector(100, 0, 0),
			Center + FVector(100, 0, 0),
			ObjectQuery));


		// 에디터용 초기 전체 개수의 0·소수·나머지 배분을 검증한다. 재생성 후 중앙 데이터와 ISM 수가 같아야 한다.
		for (int32 TotalCount : {0, 1, 2, 5, 100, 1000})
		{
			Boss->InitialParticleCount = TotalCount;
			TestTrue(TEXT("Configured total generates successfully"), Boss->GenerateInitialParts());
			TestEqual(TEXT("Configured total matches survivors"), Boss->GetActiveParticleCount(), TotalCount);
			TestEqual(TEXT("Configured total matches central slots"), Boss->GetParticles().Num(), TotalCount);
			TestEqual(TEXT("Arm receives integer third"), Boss->LeftArmInstances->GetInstanceCount(), TotalCount / 3);
			TestEqual(TEXT("Torso receives remainder"), Boss->TorsoInstances->GetInstanceCount(), TotalCount - TotalCount / 3);

			if (TotalCount >= 5)
			{
				double TorsoMinY = TNumericLimits<double>::Max();
				double ArmMaxY = TNumericLimits<double>::Lowest();
				for (const FSwarmParticle& Particle : Boss->GetParticles())
				{
					UInstancedStaticMeshComponent* Part = Particle.PartId == ESwarmPartId::Torso
						? Boss->TorsoInstances.Get() : Boss->LeftArmInstances.Get();
					// Manager가 회전되어 있으므로 배치 기준인 Manager 로컬 Y축에서 표면 간격을 측정한다.
					const FVector WorldPosition = Part->GetComponentTransform().TransformPosition(Particle.LocalPosition);
					const double CenterY = Boss->GetActorTransform().InverseTransformPosition(WorldPosition).Y;
					if (Particle.PartId == ESwarmPartId::Torso)
					{
						TorsoMinY = FMath::Min(TorsoMinY, CenterY - 10.0);
					}
					else
					{
						ArmMaxY = FMath::Max(ArmMaxY, CenterY + 10.0);
					}
				}
				TestTrue(TEXT("Generated sizes preserve configured surface gap"), TorsoMinY - ArmMaxY >= Boss->PartGap - 1.e-4);
			}
		}

		Boss->InitialParticleCount = -1;
		AddExpectedError(TEXT("InitialParticleCount must be between"), EAutomationExpectedErrorFlags::Contains, 1);
		TestFalse(TEXT("Invalid initial count is rejected"), Boss->GenerateInitialParts());
		TestEqual(TEXT("Invalid setting preserves existing particles"), Boss->GetActiveParticleCount(), 1000);

	}

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmTwoPartRemovalTest, "SwarmBoss.Stage2.TwoPartRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSwarmTwoPartRemovalTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);

	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();

	if (!TestNotNull(TEXT("Boss manager"), Boss) ||
		!TestTrue(TEXT("Both parts generate"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	TInlineComponentArray<UInstancedStaticMeshComponent*> Parts(Boss);
	UInstancedStaticMeshComponent* Torso = nullptr;
	UInstancedStaticMeshComponent* LeftArm = nullptr;

	for (UInstancedStaticMeshComponent* Part : Parts)
	{
		if (Part->GetFName() == TEXT("TorsoInstances"))
		{
			Torso = Part;
		}
		else if (Part->GetFName() == TEXT("LeftArmInstances"))
		{
			LeftArm = Part;
		}
	}

	if (!TestNotNull(TEXT("Torso ISM"), Torso) ||
		!TestNotNull(TEXT("Left arm ISM"), LeftArm) ||
		!TestEqual(TEXT("One store holds both sets of slots"), Boss->GetParticles().Num(), 90))
	{
		World->DestroyWorld(false);
		return false;
	}

	TestEqual(TEXT("Torso count"), Torso->GetInstanceCount(), 60);
	TestEqual(TEXT("Left arm count"), LeftArm->GetInstanceCount(), 30);
	TestEqual(TEXT("Initial combined active count"), Boss->GetActiveParticleCount(), 90);

	const TArray<FSwarmParticle>& Particles = Boss->GetParticles();
	auto TraceAndRemove = [&](UInstancedStaticMeshComponent* ExpectedPart, ESwarmPartId PartId)
	{
		FTransform Instance;

		if (!TestTrue(TEXT("First instance has a world transform"),
			ExpectedPart->GetInstanceTransform(0, Instance, true)))
		{
			return;
		}

		const FVector Center = Instance.GetLocation();
		FCollisionObjectQueryParams Query;
		Query.AddObjectTypesToQuery(ECC_GameTraceChannel3);
		FHitResult Hit;

		if (!TestTrue(TEXT("Trace hits the expected ISM"),
			World->LineTraceSingleByObjectType(Hit, Center - FVector(100, 0, 0), Center, Query) &&
			Hit.GetComponent() == ExpectedPart))
		{
			return;
		}

		const int32 ParticleId = Particles.IndexOfByPredicate([&](const FSwarmParticle& Particle)
		{
			return Particle.PartId == PartId && Particle.InstanceIndex == Hit.Item && Particle.bActive;
		});

		if (TestTrue(TEXT("Trace resolves to an active particle in that part"), ParticleId != INDEX_NONE))
		{
			TestTrue(TEXT("Trace removes its instance"), Boss->RemoveInstance(Hit.GetComponent(), Hit.Item));
			TestFalse(TEXT("Correct particle was deactivated"), Particles[ParticleId].bActive);
		}
	};

	TraceAndRemove(LeftArm, ESwarmPartId::LeftArm);
	TestEqual(TEXT("Arm hit preserves torso count"), Torso->GetInstanceCount(), 60);
	TestEqual(TEXT("Arm hit removes only one arm instance"), LeftArm->GetInstanceCount(), 29);

	TraceAndRemove(Torso, ESwarmPartId::Torso);
	TestEqual(TEXT("Torso hit removes only one torso instance"), Torso->GetInstanceCount(), 59);
	TestEqual(TEXT("Torso hit preserves arm count"), LeftArm->GetInstanceCount(), 29);

	// 양쪽 ISM의 동일한 번호도 서로 다른 중앙 구체를 가리킨다.
	const int32 TorsoId = Particles.IndexOfByPredicate([](const FSwarmParticle& Particle)
	{
		return Particle.PartId == ESwarmPartId::Torso && Particle.InstanceIndex == 0 && Particle.bActive;
	});
	const int32 ArmId = Particles.IndexOfByPredicate([](const FSwarmParticle& Particle)
	{
		return Particle.PartId == ESwarmPartId::LeftArm && Particle.InstanceIndex == 0 && Particle.bActive;
	});

	if (TestTrue(TEXT("Both ISMs have a live instance at index zero"),
		TorsoId != INDEX_NONE && ArmId != INDEX_NONE && TorsoId != ArmId))
	{
		TestTrue(TEXT("Remove arm index zero"), Boss->RemoveInstance(LeftArm, 0));
		TestFalse(TEXT("Arm particle removed"), Particles[ArmId].bActive);
		TestTrue(TEXT("Torso particle with the same index survives"), Particles[TorsoId].bActive);

		TestTrue(TEXT("Remove torso index zero"), Boss->RemoveInstance(Torso, 0));
		TestFalse(TEXT("Torso particle removed"), Particles[TorsoId].bActive);
	}

	TestEqual(TEXT("Shared count after four removals"), Boss->GetActiveParticleCount(), 86);

	// 부위별 로컬 좌표로 움직이고, 이동 중 한 부위의 구체를 제거해도 다른 부위의 이동이 계속되는지 확인한다.
	const int32 MovingTorsoId = Particles.IndexOfByPredicate([](const FSwarmParticle& Particle)
	{
		return Particle.bActive && Particle.PartId == ESwarmPartId::Torso;
	});
	const int32 MovingArmId = Particles.IndexOfByPredicate([](const FSwarmParticle& Particle)
	{
		return Particle.bActive && Particle.PartId == ESwarmPartId::LeftArm;
	});

	if (TestTrue(TEXT("Both parts have a movement candidate"), MovingTorsoId != INDEX_NONE && MovingArmId != INDEX_NONE))
	{
		const FVector TorsoTarget = Particles[MovingTorsoId].LocalPosition + FVector(100, 0, 0);
		const FVector ArmTarget = Particles[MovingArmId].LocalPosition + FVector(0, 100, 0);
		TestTrue(TEXT("Torso target accepted"), Boss->AssignParticleTarget(MovingTorsoId, TorsoTarget));
		TestTrue(TEXT("Arm target accepted"), Boss->AssignParticleTarget(MovingArmId, ArmTarget));
		Boss->Tick(0.25f);

		for (int32 ParticleId : {MovingTorsoId, MovingArmId})
		{
			const FSwarmParticle& Particle = Particles[ParticleId];
			UInstancedStaticMeshComponent* Part = Particle.PartId == ESwarmPartId::Torso ? Torso : LeftArm;
			FTransform Transform;
			TestTrue(TEXT("Moving instance exists"), Part->GetInstanceTransform(Particle.InstanceIndex, Transform, false));
			TestTrue(TEXT("Correct part follows its particle"), Transform.GetLocation().Equals(Particle.LocalPosition));
		}

		TestTrue(TEXT("Remove moving arm particle"), Boss->RemoveInstance(LeftArm, Particles[MovingArmId].InstanceIndex));
		Boss->Tick(1.0f);
		TestTrue(TEXT("Torso reaches its target"), Particles[MovingTorsoId].LocalPosition.Equals(TorsoTarget));
		TestFalse(TEXT("Removed arm particle stays inactive"), Particles[MovingArmId].bActive);
		TestFalse(TEXT("Manager Tick stops at arrival"), Boss->IsActorTickEnabled());
		TestEqual(TEXT("Removal during movement changes total once"), Boss->GetActiveParticleCount(), 85);
	}


	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmPartTransferTest, "SwarmBoss.Stage2.PartTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 회전·이동된 부위 사이에서 구체 ID와 월드 위치·목표·크기를 유지하는지 확인한다.
// 중간 및 마지막 인스턴스 전달, 잘못된 요청, 전달 후 피격과 재전달의 개수·연결 정보도 검증한다.
bool FSwarmPartTransferTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();
	if (!TestNotNull(TEXT("Manager"), Boss) || !TestTrue(TEXT("Generate parts"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	UInstancedStaticMeshComponent* Torso = nullptr;
	UInstancedStaticMeshComponent* Arm = nullptr;
	for (UInstancedStaticMeshComponent* Part : TInlineComponentArray<UInstancedStaticMeshComponent*>(Boss))
	{
		if (Part->GetFName() == TEXT("TorsoInstances"))
		{
			Torso = Part;
		}
		else if (Part->GetFName() == TEXT("LeftArmInstances"))
		{
			Arm = Part;
		}
	}

	if (!TestNotNull(TEXT("Torso"), Torso) || !TestNotNull(TEXT("Arm"), Arm))
	{
		World->DestroyWorld(false);
		return false;
	}

	Boss->SetActorLocationAndRotation(FVector(100, 200, 300), FRotator(20, 40, 10));
	Torso->SetRelativeRotation(FRotator(0, 15, 0));
	Arm->SetRelativeRotation(FRotator(10, -35, 20));
	Arm->SetRelativeScale3D(FVector(1.5));

	const TArray<FSwarmParticle>& Particles = Boss->GetParticles();
	const int32 ParticleId = Boss->SelectPartParticleIds(ESwarmPartId::Torso, 10).Last();
	const int32 SourceIndex = Particles[ParticleId].InstanceIndex;
	const int32 LastTorsoId = Boss->SelectPartParticleIds(ESwarmPartId::Torso, 100).Last();
	FTransform Before;
	TestTrue(TEXT("Original world transform"), Torso->GetInstanceTransform(SourceIndex, Before, true));

	TestFalse(TEXT("Invalid particle rejected"), Boss->TransferParticle(-1, ESwarmPartId::LeftArm));
	TestFalse(TEXT("Out of range rejected"), Boss->TransferParticle(Particles.Num(), ESwarmPartId::LeftArm));
	TestFalse(TEXT("Same part rejected"), Boss->TransferParticle(ParticleId, ESwarmPartId::Torso));
	TestFalse(TEXT("Unknown part rejected"), Boss->TransferParticle(ParticleId, static_cast<ESwarmPartId>(255)));
	TestEqual(TEXT("Invalid requests keep source count"), Torso->GetInstanceCount(), 60);
	TestEqual(TEXT("Invalid requests keep destination count"), Arm->GetInstanceCount(), 30);

	if (TestTrue(TEXT("Transfer stationary particle"), Boss->TransferParticle(ParticleId, ESwarmPartId::LeftArm)))
	{
		FTransform After;
		TestTrue(TEXT("Destination instance exists"), Arm->GetInstanceTransform(Particles[ParticleId].InstanceIndex, After, true));
		TestTrue(TEXT("World transform preserved"), After.Equals(Before, 1.e-4));
		TestTrue(TEXT("Stationary target stays at current position"), Particles[ParticleId].LocalPosition == Particles[ParticleId].TargetLocalPosition);
		TestFalse(TEXT("Transfer does not enable Tick"), Boss->IsActorTickEnabled());
		TestTrue(TEXT("Same central particle now belongs to arm"), Particles[ParticleId].PartId == ESwarmPartId::LeftArm);
		TestEqual(TEXT("Central slots unchanged"), Particles.Num(), 90);
		TestEqual(TEXT("Total survivors unchanged"), Boss->GetActiveParticleCount(), 90);
		TestEqual(TEXT("Torso loses one"), Torso->GetInstanceCount(), 59);
		TestEqual(TEXT("Arm gains one"), Arm->GetInstanceCount(), 31);
		TestEqual(TEXT("Swap moved particle index updated"), Particles[LastTorsoId].InstanceIndex, SourceIndex);

		// 방금 추가한 마지막 인스턴스를 원래 부위로 돌려보내 마지막 원소 삭제도 검증한다.
		TestTrue(TEXT("Transfer last destination instance back"), Boss->TransferParticle(ParticleId, ESwarmPartId::Torso));
		TestTrue(TEXT("Returned world transform exists"), Torso->GetInstanceTransform(Particles[ParticleId].InstanceIndex, After, true));
		TestTrue(TEXT("Return preserves world transform"), After.Equals(Before, 1.e-4));
		TestEqual(TEXT("Torso count restored"), Torso->GetInstanceCount(), 60);
		TestEqual(TEXT("Arm count restored"), Arm->GetInstanceCount(), 30);
	}

	// 이동 중이던 목표의 월드 위치를 보존하고 소속 변경 후에도 같은 위치에 도달하는지 검증한다.
	const FVector Target = Particles[ParticleId].LocalPosition + FVector(100, 0, 0);
	const FVector WorldTarget = Torso->GetComponentTransform().TransformPosition(Target);
	TestTrue(TEXT("Assign existing target"), Boss->AssignParticleTarget(ParticleId, Target));
	Boss->Tick(0.1f);
	TestTrue(TEXT("Transfer moving particle"), Boss->TransferParticle(ParticleId, ESwarmPartId::LeftArm));
	TestTrue(TEXT("Existing world target preserved"), Arm->GetComponentTransform().TransformPosition(
		Particles[ParticleId].TargetLocalPosition).Equals(WorldTarget, 1.e-4));
	Boss->Tick(10.0f);
	FTransform Arrived;
	TestTrue(TEXT("Transferred instance still exists"), Arm->GetInstanceTransform(Particles[ParticleId].InstanceIndex, Arrived, true));
	TestTrue(TEXT("Transferred particle reaches existing world target"), Arrived.GetLocation().Equals(WorldTarget, 1.e-4));

	TestTrue(TEXT("Remove swapped source particle"), Boss->RemoveInstance(Torso, SourceIndex));
	TestFalse(TEXT("Source mapping resolves correct particle"), Particles[LastTorsoId].bActive);
	TestTrue(TEXT("Transferred particle survives source removal"), Particles[ParticleId].bActive);
	TestTrue(TEXT("Remove transferred destination particle"), Boss->RemoveInstance(Arm, Particles[ParticleId].InstanceIndex));
	TestFalse(TEXT("Destination mapping resolves transferred particle"), Particles[ParticleId].bActive);
	TestFalse(TEXT("Removed particle cannot transfer"), Boss->TransferParticle(ParticleId, ESwarmPartId::Torso));
	TestEqual(TEXT("Only real removals reduce survivors"), Boss->GetActiveParticleCount(), 88);
	TestEqual(TEXT("ISM counts match survivors"), Torso->GetInstanceCount() + Arm->GetInstanceCount(), 88);

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmTransferBatchTest, "SwarmBoss.Stage2.TransferBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 공통 전달 함수의 양방향 전달, 목표 개수·좌표 반영, 잘못된 입력 거부와 이동 중 제거를 검증한다.
// 실제 키 입력과 화면 표시는 이 자동 테스트의 검증 대상이 아니다.
bool FSwarmTransferBatchTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();
	if (!TestNotNull(TEXT("Manager"), Boss) || !TestTrue(TEXT("Generate parts"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	UInstancedStaticMeshComponent* Torso = nullptr;
	UInstancedStaticMeshComponent* Arm = nullptr;
	for (UInstancedStaticMeshComponent* Part : TInlineComponentArray<UInstancedStaticMeshComponent*>(Boss))
	{
		if (Part->GetFName() == TEXT("TorsoInstances"))
		{
			Torso = Part;
		}
		else if (Part->GetFName() == TEXT("LeftArmInstances"))
		{
			Arm = Part;
		}
	}

	if (!TestNotNull(TEXT("Torso"), Torso) || !TestNotNull(TEXT("Arm"), Arm))
	{
		World->DestroyWorld(false);
		return false;
	}

	TArray<FVector> TestTargets;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		TestTargets.Add(FVector(5.0, 10.0, Index * 7.0));
	}

	const FVector InvalidTargets[] = {FVector::ZeroVector, FVector(std::numeric_limits<double>::quiet_NaN(), 0, 0)};
	TestEqual(TEXT("Invalid target rejects entire request"), Boss->TransferParticlesToTargets(
		ESwarmPartId::Torso, ESwarmPartId::LeftArm, InvalidTargets), 0);
	TestEqual(TEXT("Invalid input preserves torso"), Torso->GetInstanceCount(), 60);
	TestEqual(TEXT("Invalid input preserves arm"), Arm->GetInstanceCount(), 30);
	TestEqual(TEXT("Empty targets transfer nothing"), Boss->TransferParticlesToTargets(
		ESwarmPartId::Torso, ESwarmPartId::LeftArm, TConstArrayView<FVector>()), 0);
	TestEqual(TEXT("Same part transfers nothing"), Boss->TransferParticlesToTargets(
		ESwarmPartId::Torso, ESwarmPartId::Torso, TestTargets), 0);
	TestEqual(TEXT("Unknown destination transfers nothing"), Boss->TransferParticlesToTargets(
		ESwarmPartId::Torso, static_cast<ESwarmPartId>(255), TestTargets), 0);

	const TArray<int32> Selected = Boss->SelectPartParticleIds(ESwarmPartId::Torso, 10);
	const TArray<FSwarmParticle>& Particles = Boss->GetParticles();
	TestEqual(TEXT("Batch transfers ten"), Boss->TransferParticlesToTargets(ESwarmPartId::Torso, ESwarmPartId::LeftArm, TestTargets), 10);
	TestEqual(TEXT("Torso after transfer"), Torso->GetInstanceCount(), 50);
	TestEqual(TEXT("Arm after transfer"), Arm->GetInstanceCount(), 40);
	TestEqual(TEXT("Transfer preserves total"), Boss->GetActiveParticleCount(), 90);
	TestTrue(TEXT("Targets enable Tick"), Boss->IsActorTickEnabled());

	for (int32 Index = 0; Index < Selected.Num(); ++Index)
	{
		const FSwarmParticle& Particle = Particles[Selected[Index]];
		TestTrue(TEXT("Selected particle belongs to arm"), Particle.PartId == ESwarmPartId::LeftArm);
		TestTrue(TEXT("Caller target is used"), Particle.TargetLocalPosition.Equals(TestTargets[Index]));
	}

	Boss->Tick(0.01f);
	const int32 RemovedId = Selected[0];
	TestTrue(TEXT("Remove batch particle during movement"), Boss->RemoveInstance(Arm, Particles[RemovedId].InstanceIndex));
	Boss->Tick(10.0f);

	for (int32 ParticleId : Selected)
	{
		const FSwarmParticle& Particle = Particles[ParticleId];
		if (!Particle.bActive)
		{
			continue;
		}

		TestTrue(TEXT("Survivor reaches assigned target"), Particle.LocalPosition.Equals(Particle.TargetLocalPosition));
		FTransform LocalTransform;
		TestTrue(TEXT("Destination instance exists"), Arm->GetInstanceTransform(Particle.InstanceIndex, LocalTransform, false));
		TestTrue(TEXT("Destination instance reaches target"), LocalTransform.GetLocation().Equals(Particle.TargetLocalPosition));
	}

	TestFalse(TEXT("Removed particle remains inactive"), Particles[RemovedId].bActive);
	TestFalse(TEXT("Arrival disables Tick"), Boss->IsActorTickEnabled());
	TestEqual(TEXT("Removal decrements total once"), Boss->GetActiveParticleCount(), 89);
	TestEqual(TEXT("Repeated batch transfers ten more"), Boss->TransferParticlesToTargets(ESwarmPartId::Torso, ESwarmPartId::LeftArm, TestTargets), 10);
	TestEqual(TEXT("Repeated batch torso count"), Torso->GetInstanceCount(), 40);
	TestEqual(TEXT("Repeated batch arm count"), Arm->GetInstanceCount(), 49);

	// 몸통에 3개만 남겨 요청 개수보다 부족할 때와 완전히 비었을 때를 확인한다.
	const TArray<int32> RemainingTorso = Boss->SelectPartParticleIds(ESwarmPartId::Torso, 100);
	for (int32 Index = 3; Index < RemainingTorso.Num(); ++Index)
	{
		TestTrue(TEXT("Remove surplus torso particle"), Boss->RemoveInstance(Torso, Particles[RemainingTorso[Index]].InstanceIndex));
	}

	const int32 TotalBeforeFinalTransfer = Boss->GetActiveParticleCount();
	TestEqual(TEXT("Only three survivors transfer"), Boss->TransferParticlesToTargets(ESwarmPartId::Torso, ESwarmPartId::LeftArm, TestTargets), 3);
	TestEqual(TEXT("Torso becomes empty"), Torso->GetInstanceCount(), 0);
	TestEqual(TEXT("Final transfer preserves total"), Boss->GetActiveParticleCount(), TotalBeforeFinalTransfer);
	TestEqual(TEXT("Empty torso transfers zero"), Boss->TransferParticlesToTargets(ESwarmPartId::Torso, ESwarmPartId::LeftArm, TestTargets), 0);
	TestEqual(TEXT("ISM counts match total"), Torso->GetInstanceCount() + Arm->GetInstanceCount(), Boss->GetActiveParticleCount());

	// 출발·목적지를 반대로 지정하고 2개짜리 목표를 전달해 부위나 개수가 고정되지 않았음을 확인한다.
	const TArray<int32> ReturnIds = Boss->SelectPartParticleIds(ESwarmPartId::LeftArm, 2);
	const FVector ReturnTargets[] = {FVector(1, 2, 3), FVector(4, 5, 6)};
	TestEqual(TEXT("Reverse transfer uses target count"), Boss->TransferParticlesToTargets(
		ESwarmPartId::LeftArm, ESwarmPartId::Torso, ReturnTargets), 2);
	TestEqual(TEXT("Reverse transfer restores two torso particles"), Torso->GetInstanceCount(), 2);
	for (int32 Index = 0; Index < ReturnIds.Num(); ++Index)
	{
		const FSwarmParticle& Particle = Particles[ReturnIds[Index]];
		TestTrue(TEXT("Returned particle belongs to torso"), Particle.PartId == ESwarmPartId::Torso);
		TestTrue(TEXT("Returned particle uses torso-local target"), Particle.TargetLocalPosition.Equals(ReturnTargets[Index]));
	}
	Boss->Tick(10.0f);
	for (int32 ParticleId : ReturnIds)
	{
		TestTrue(TEXT("Reverse transfer reaches target"), Particles[ParticleId].LocalPosition.Equals(Particles[ParticleId].TargetLocalPosition));
	}
	TestEqual(TEXT("Reverse transfer preserves total"), Boss->GetActiveParticleCount(), TotalBeforeFinalTransfer);

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmRebalanceTest, "SwarmBoss.Stage2.RebalanceParts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 한쪽의 손실량으로 전달 수를 계산해 양방향 모두 2:1로 복구하고 반복 입력 시 초과 전달하지 않는지 검증한다.
bool FSwarmRebalanceTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();
	if (!TestNotNull(TEXT("Manager"), Boss) || !TestTrue(TEXT("Generate initial parts"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	const FFloatProperty* DurationProperty = FindFProperty<FFloatProperty>(
		ASwarmBossManager::StaticClass(), TEXT("ReconstructionDuration"));
	if (!TestNotNull(TEXT("Reconstruction duration is reflected"), DurationProperty))
	{
		World->DestroyWorld(false);
		return false;
	}

	const float Duration = DurationProperty->GetPropertyValue_InContainer(Boss);

	UInstancedStaticMeshComponent* Torso = nullptr;
	UInstancedStaticMeshComponent* Arm = nullptr;
	for (UInstancedStaticMeshComponent* Part : TInlineComponentArray<UInstancedStaticMeshComponent*>(Boss))
	{
		if (Part->GetFName() == TEXT("TorsoInstances"))
		{
			Torso = Part;
		}
		else if (Part->GetFName() == TEXT("LeftArmInstances"))
		{
			Arm = Part;
		}
	}

	if (!TestNotNull(TEXT("Torso"), Torso) || !TestNotNull(TEXT("Arm"), Arm))
	{
		World->DestroyWorld(false);
		return false;
	}

	TestFalse(TEXT("Invalid weights rejected"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 0, 1));
	TestTrue(TEXT("Initial balanced state succeeds"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 2, 1));
	TestEqual(TEXT("Initial torso stays at sixty"), Torso->GetInstanceCount(), 60);
	TestEqual(TEXT("Initial arm stays at thirty"), Arm->GetInstanceCount(), 30);

	for (int32 Index = 0; Index < 9; ++Index)
	{
		TestTrue(TEXT("Remove arm particle"), Boss->RemoveInstance(Arm, 0));
	}
	TestTrue(TEXT("Rebalance after arm loss"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 2, 1));
	TestEqual(TEXT("Six leave torso"), Torso->GetInstanceCount(), 54);
	TestEqual(TEXT("Six join arm"), Arm->GetInstanceCount(), 27);
	TestEqual(TEXT("Total after arm loss"), Boss->GetActiveParticleCount(), 81);

	// 이동 중 다시 요청해도 불필요한 추가 전달이 발생하지 않아야 한다.
	TestTrue(TEXT("Repeated request during movement succeeds"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 2, 1));
	TestEqual(TEXT("Repeated request preserves torso"), Torso->GetInstanceCount(), 54);
	TestEqual(TEXT("Repeated request preserves arm"), Arm->GetInstanceCount(), 27);
	Boss->Tick(Duration * 0.5f);
	TestTrue(TEXT("Rebalance still moves at half duration"), Boss->IsActorTickEnabled());
	Boss->Tick(Duration * 0.5f);
	TestFalse(TEXT("Rebalance finishes in configured duration"), Boss->IsActorTickEnabled());

	for (int32 Index = 0; Index < 9; ++Index)
	{
		TestTrue(TEXT("Remove torso particle"), Boss->RemoveInstance(Torso, 0));
	}
	TestTrue(TEXT("Reverse rebalance after torso loss"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 2, 1));
	TestEqual(TEXT("Three return to torso"), Torso->GetInstanceCount(), 48);
	TestEqual(TEXT("Three leave arm"), Arm->GetInstanceCount(), 24);
	TestEqual(TEXT("Total after torso loss"), Boss->GetActiveParticleCount(), 72);
	Boss->Tick(Duration);

	for (const FSwarmParticle& Particle : Boss->GetParticles())
	{
		if (Particle.bActive)
		{
			TestTrue(TEXT("Survivor reaches regenerated target"), Particle.LocalPosition.Equals(Particle.TargetLocalPosition));
		}
	}
	TestFalse(TEXT("Arrival disables Tick"), Boss->IsActorTickEnabled());

	TestTrue(TEXT("Odd survivor removal"), Boss->RemoveInstance(Torso, 0));
	TestTrue(TEXT("Remainder rebalance"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 2, 1));
	TestEqual(TEXT("Remainder goes to first part"), Torso->GetInstanceCount(), 48);
	TestEqual(TEXT("Second part uses integer division"), Arm->GetInstanceCount(), 23);
	TestEqual(TEXT("Total remains seventy-one"), Boss->GetActiveParticleCount(), 71);

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmTimedReconstructionTest, "SwarmBoss.Stage2.TimedReconstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 에디터에 노출된 시간을 변경해 도착 시간을 검증하고, 이동 중 제거·재배정·소속 변경과 잘못된 시간을 확인한다.
bool FSwarmTimedReconstructionTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();
	if (!TestNotNull(TEXT("Manager"), Boss) || !TestTrue(TEXT("Generate parts"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	const FFloatProperty* DurationProperty = FindFProperty<FFloatProperty>(
		ASwarmBossManager::StaticClass(), TEXT("ReconstructionDuration"));
	if (!TestNotNull(TEXT("Reconstruction duration is reflected"), DurationProperty))
	{
		World->DestroyWorld(false);
		return false;
	}

	TestTrue(TEXT("Duration is editable"), DurationProperty->HasAnyPropertyFlags(CPF_Edit));
	DurationProperty->SetPropertyValue_InContainer(Boss, 0.6f);
	const float Duration = DurationProperty->GetPropertyValue_InContainer(Boss);

	UInstancedStaticMeshComponent* Torso = nullptr;
	UInstancedStaticMeshComponent* Arm = nullptr;
	for (UInstancedStaticMeshComponent* Part : TInlineComponentArray<UInstancedStaticMeshComponent*>(Boss))
	{
		if (Part->GetFName() == TEXT("TorsoInstances"))
		{
			Torso = Part;
		}
		else if (Part->GetFName() == TEXT("LeftArmInstances"))
		{
			Arm = Part;
		}
	}
	if (!TestNotNull(TEXT("Torso"), Torso) || !TestNotNull(TEXT("Arm"), Arm))
	{
		World->DestroyWorld(false);
		return false;
	}

	const TArray<FSwarmParticle> Before = Boss->GetParticles();
	const TArray<FSwarmParticle>& Particles = Boss->GetParticles();
	TArray<FVector> Targets;
	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		Targets.Add(Before[Index].LocalPosition + FVector(Index % 2 == 0 ? 20.0 : 2000.0, 0, 0));
	}

	TestTrue(TEXT("Assign reconstruction"), Boss->AssignReconstructionTargets(Targets));
	Boss->Tick(Duration * 0.25f);
	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		TestTrue(TEXT("Quarter time follows ease-out quadratic"), Particles[Index].LocalPosition.Equals(
			FMath::Lerp(Before[Index].LocalPosition, Targets[Index], 0.4375), 1.e-3));
	}
	Boss->Tick(Duration * 0.25f);
	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		TestTrue(TEXT("Near and far particles both travel 75 percent at half time"), Particles[Index].LocalPosition.Equals(
			FMath::Lerp(Before[Index].LocalPosition, Targets[Index], 0.75), 1.e-3));
	}

	const TArray<int32> TorsoIds = Boss->SelectPartParticleIds(ESwarmPartId::Torso, 3);
	const int32 RemovedId = TorsoIds[0];
	const int32 RetargetId = TorsoIds[1];
	const int32 TransferId = TorsoIds[2];
	const FVector RetargetStart = Particles[RetargetId].LocalPosition;
	const FVector RetargetGoal = RetargetStart + FVector(0, 300, 0);
	const FVector TransferWorldGoal = Torso->GetComponentTransform().TransformPosition(Particles[TransferId].TargetLocalPosition);

	TestTrue(TEXT("Remove during timed reconstruction"), Boss->RemoveInstance(Torso, Particles[RemovedId].InstanceIndex));
	TestTrue(TEXT("Retarget from current position"), Boss->AssignParticleTarget(RetargetId, RetargetGoal, Duration));
	TestTrue(TEXT("Transfer preserves remaining duration"), Boss->TransferParticle(TransferId, ESwarmPartId::LeftArm));
	Boss->Tick(Duration * 0.5f);

	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		if (Index == RemovedId || Index == RetargetId)
		{
			continue;
		}
		TestTrue(TEXT("Original reconstruction reaches target in configured duration"),
			Particles[Index].LocalPosition.Equals(Particles[Index].TargetLocalPosition, 1.e-3));
	}
	TestTrue(TEXT("Retarget has its own configured duration"), Particles[RetargetId].LocalPosition.Equals(
		FMath::Lerp(RetargetStart, RetargetGoal, 0.75), 1.e-3));
	TestTrue(TEXT("Transfer still reaches original world goal"), Arm->GetComponentTransform().TransformPosition(
		Particles[TransferId].LocalPosition).Equals(TransferWorldGoal, 1.e-3));
	TestFalse(TEXT("Removed particle stays inactive"), Particles[RemovedId].bActive);
	TestEqual(TEXT("Removed particle clears remaining time"), Particles[RemovedId].RemainingMoveTime, 0.0f);
	TestEqual(TEXT("Removal counted once"), Boss->GetActiveParticleCount(), 89);

	Boss->Tick(Duration * 0.5f);
	TestTrue(TEXT("Retarget reaches new goal"), Particles[RetargetId].LocalPosition.Equals(RetargetGoal));
	TestFalse(TEXT("Arrival disables Tick"), Boss->IsActorTickEnabled());
	TestFalse(TEXT("Negative duration rejected"), Boss->AssignParticleTarget(RetargetId, RetargetGoal + FVector(1, 0, 0), -1.0f));
	TestTrue(TEXT("Invalid duration preserves target"), Particles[RetargetId].TargetLocalPosition.Equals(RetargetGoal));

	const FVector FinalTarget = RetargetGoal + FVector(100, 0, 0);
	TestTrue(TEXT("Assign target before long frame"), Boss->AssignParticleTarget(RetargetId, FinalTarget, Duration));
	Boss->Tick(1.0f);
	TestTrue(TEXT("Long frame clamps at target"), Particles[RetargetId].LocalPosition.Equals(FinalTarget));
	TestFalse(TEXT("Long frame completes movement"), Boss->IsActorTickEnabled());

	DurationProperty->SetPropertyValue_InContainer(Boss, 0.0f);
	TestFalse(TEXT("Zero reconstruction duration rejected"), Boss->RebalanceParts(
		ESwarmPartId::Torso, ESwarmPartId::LeftArm, 2, 1));
	TestEqual(TEXT("Rejected duration preserves active count"), Boss->GetActiveParticleCount(), 89);

	World->DestroyWorld(false);
	return true;
}

#endif
