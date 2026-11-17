// SwarmShapeSamplerTests.cpp
// 1단계 Unreal Automation 테스트(SwarmBoss.Stage1.*). 개발 빌드(WITH_DEV_AUTOMATION_TESTS)에서만 컴파일된다.
// - SamplingInvariants: 구·박스 × 시드 3개 × 개수 3개 조합의 성공, 개수, 간격·경계, 확장 상한.
// - DeterminismAndScale: 같은 시드의 재현성, 간격 2배 → 크기 2배, 개수 8배 → 크기 2배.
// - EdgeCasesAndFailure: 0개·1개 처리, 잘못된 설정 거부, 후보 예산·시도 상한 준수, 검증기의 겹침·경계 위반 검출.
// - PreviewInstances: 임시 월드에서 미리보기 Actor의 인스턴스 수, 구체 스케일, 실패 시 보존, 초기화 동작.

#include "SwarmShapeSampler.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Algo/Reverse.h"
#include "UObject/UnrealType.h"
#include <limits>
#include "Boss/SwarmBossManager.h"
#include "Boss/SwarmFormDefinition.h"
#include "Boss/SwarmTransferPlanner.h"
#include "Swarm/SwarmTargetAssigner.h"
#include "Projectiles/SwarmProjectile.h"
#include "Engine/StaticMeshActor.h"
#include "SwarmShapePreview.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"

namespace
{
	// 테스트용 2부위 데이터에서 몸통 왼쪽 면과 왼팔 오른쪽 면 사이의 구체 표면 간격(cm). 이전 PartGap 기본값과 같다.
	constexpr float TwoPartArmGap = 150.0f;

	// ASwarmBossManager::PackingCoefficient 기본값. 기대 도형 크기를 Manager와 같은 설정으로 계산할 때 쓴다.
	constexpr float ManagerPackingCoefficient = 0.5f;

	// 이전 코드 기본값과 같은 몸통:왼팔 2:1 형태 데이터를 임시 패키지에 만든다.
	// 몸통은 최상위 부위이며 박스 비율 (1, 0.75, 1.5), 왼팔은 몸통의 자식이며 (0.45, 0.45, 1)이다.
	// 왼팔은 몸통 왼쪽 면 중앙에 오른쪽 면 중앙을 맞추고 TwoPartArmGap만큼 띄운다.
	USwarmFormDefinition* CreateTwoPartDefinition()
	{
		USwarmFormDefinition* Definition = NewObject<USwarmFormDefinition>(GetTransientPackage());

		FSwarmPartDefinition& Torso = Definition->Parts.AddDefaulted_GetRef();
		Torso.PartId = ESwarmPartId::Torso;
		Torso.ParentPartId = ESwarmPartId::None;
		Torso.Shape = ESwarmPrimitive::Box;
		Torso.BoxHalfExtentsRatio = FVector(1.0, 0.75, 1.5);
		Torso.AllocationWeight = 2;

		FSwarmPartDefinition& LeftArm = Definition->Parts.AddDefaulted_GetRef();
		LeftArm.PartId = ESwarmPartId::LeftArm;
		LeftArm.ParentPartId = ESwarmPartId::Torso;
		LeftArm.Shape = ESwarmPrimitive::Box;
		LeftArm.BoxHalfExtentsRatio = FVector(0.45, 0.45, 1.0);
		LeftArm.AllocationWeight = 1;
		LeftArm.ParentAnchor = FVector(0.0, -1.0, 0.0);
		LeftArm.ChildAnchor = FVector(0.0, 1.0, 0.0);
		LeftArm.Gap = TwoPartArmGap;

		return Definition;
	}

	// Manager가 Part 정의와 Count개 구체로 초기 생성·재분배할 때 사용하는 것과 같은 샘플러 설정으로 도형 반크기를 계산한다.
	// 샘플러는 같은 설정·시드에서 같은 결과를 내므로 테스트의 기대 위치 계산에 사용한다.
	FVector ExpectedHalfExtents(const FSwarmPartDefinition& Part, int32 Count)
	{
		FSwarmSamplingSettings Settings;
		Settings.Shape = Part.Shape;
		Settings.BoxHalfExtents = Part.BoxHalfExtentsRatio;
		Settings.ParticleCount = Count;
		Settings.PackingCoefficient = ManagerPackingCoefficient;
		Settings.Seed = ASwarmBossManager::GetPartSamplingSeed(Part.PartId);

		return FSwarmShapeSampler::GenerateTargets(Settings).HalfExtents;
	}

	// World에 Manager를 생성하고 몸통:왼팔 2:1 형태 데이터를 지정한다. 구체는 아직 생성하지 않는다.
	// 제거·이동·전달 같은 개별 동작을 검증하는 테스트가 Tick에서 자동 재분배에 섞이지 않도록 기본은 손실 자동 재분배를 끈다.
	// 자동 재분배는 AutoRebalance·ProjectileAttack 테스트가 켜고 검증한다.
	ASwarmBossManager* SpawnTwoPartManager(UWorld* World, bool bAutoRebalanceOnLoss = false)
	{
		ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();

		if (Boss)
		{
			Boss->SetFormDefinition(CreateTwoPartDefinition());
			Boss->SetAutoRebalanceOnLoss(bAutoRebalanceOnLoss);
		}

		return Boss;
	}

	// Manager의 활성 구체 중 소속이 PartId인 개수를 센다.
	int32 CountActivePartParticles(const ASwarmBossManager& Boss, ESwarmPartId PartId)
	{
		int32 Count = 0;

		for (const FSwarmParticle& Particle : Boss.GetParticles())
		{
			if (Particle.bActive && Particle.PartId == PartId)
			{
				++Count;
			}
		}

		return Count;
	}
}

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

	ASwarmBossManager* Boss = SpawnTwoPartManager(World);
	// 부위 ISM은 형태 데이터로 생성할 때 만들어지므로, 단일 도형 검증 전에 몸통·왼팔 ISM을 먼저 준비한다.
	if (TestNotNull(TEXT("Boss manager"), Boss) && TestTrue(TEXT("Create part instances"), Boss->GenerateInitialParts()))
	{
		FSwarmSamplingSettings Settings;
		FVector AppliedHalfExtents = FVector::ZeroVector;
		UInstancedStaticMeshComponent* Instances = Boss->GetPartInstances(ESwarmPartId::Torso);

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

		// 몸통은 바닥 맞춤으로 Manager 위로 올라가 있으므로 몸통 ISM 중심을 지나도록 쏜다.
		const FVector Center = Instances->GetComponentLocation();
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
			TestEqual(TEXT("Arm receives integer third"), Boss->GetPartInstances(ESwarmPartId::LeftArm)->GetInstanceCount(), TotalCount / 3);
			TestEqual(TEXT("Torso receives remainder"), Boss->GetPartInstances(ESwarmPartId::Torso)->GetInstanceCount(), TotalCount - TotalCount / 3);

			if (TotalCount >= 5)
			{
				double TorsoMinY = TNumericLimits<double>::Max();
				double ArmMaxY = TNumericLimits<double>::Lowest();
				for (const FSwarmParticle& Particle : Boss->GetParticles())
				{
					UInstancedStaticMeshComponent* Part = Particle.PartId == ESwarmPartId::Torso
						? Boss->GetPartInstances(ESwarmPartId::Torso) : Boss->GetPartInstances(ESwarmPartId::LeftArm);
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
				TestTrue(TEXT("Generated sizes preserve configured surface gap"), TorsoMinY - ArmMaxY >= TwoPartArmGap - 1.e-4);
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

	ASwarmBossManager* Boss = SpawnTwoPartManager(World);

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

	ASwarmBossManager* Boss = SpawnTwoPartManager(World);
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

	ASwarmBossManager* Boss = SpawnTwoPartManager(World);
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

	ASwarmBossManager* Boss = SpawnTwoPartManager(World);
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

	TestFalse(TEXT("Negative weight rejected"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, -1, 1));
	TestFalse(TEXT("Zero weight sum rejected"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 0, 0));
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

	ASwarmBossManager* Boss = SpawnTwoPartManager(World);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmFormDefinitionTest, "SwarmBoss.Stage2.FormDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 형태 데이터에 따른 초기 생성을 검증한다. 데이터 누락은 생성하지 않고, 가중치 비율과 잔여 배정을 따르며,
// 잘못된 데이터는 경고와 함께 거절하고 이미 생성된 구체를 유지해야 한다.
bool FSwarmFormDefinitionTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);

	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();

	if (!TestNotNull(TEXT("Manager"), Boss))
	{
		World->DestroyWorld(false);
		return false;
	}

	AddExpectedError(TEXT("FormDefinition is not assigned"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Missing definition is rejected"), Boss->GenerateInitialParts());
	TestEqual(TEXT("Missing definition creates no particles"), Boss->GetParticles().Num(), 0);

	// 기본 전체 90개를 몸통:왼팔 3:4로 나누면 버림 결과는 38(38.57)과 51(51.43)이다.
	// 버림으로 남은 1개는 잔여 수신 부위인 몸통으로 가서 39개가 된다.
	USwarmFormDefinition* WeightedDefinition = CreateTwoPartDefinition();
	WeightedDefinition->Parts[0].AllocationWeight = 3;
	WeightedDefinition->Parts[1].AllocationWeight = 4;
	Boss->SetFormDefinition(WeightedDefinition);

	if (TestTrue(TEXT("Custom weights generate"), Boss->GenerateInitialParts()))
	{
		TestEqual(TEXT("Torso receives share and remainder"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 39);
		TestEqual(TEXT("Arm receives floored share"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 51);
	}

	// 가중치 0인 왼팔은 비율 배분을 받지 않으며 전체가 몸통에 배정된다.
	USwarmFormDefinition* ZeroArmDefinition = CreateTwoPartDefinition();
	ZeroArmDefinition->Parts[1].AllocationWeight = 0;
	Boss->SetFormDefinition(ZeroArmDefinition);

	if (TestTrue(TEXT("Zero weight part generates"), Boss->GenerateInitialParts()))
	{
		TestEqual(TEXT("Zero weight arm stays empty"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 0);
		TestEqual(TEXT("Torso receives every particle"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 90);
	}

	// 각 잘못된 데이터는 해당 원인의 경고를 남기고 실패하며, 직전에 생성한 90개를 그대로 유지해야 한다.
	struct FInvalidCase
	{
		const TCHAR* Name;
		const TCHAR* ExpectedLog;
		TFunction<void(USwarmFormDefinition&)> Modify;
	};

	const FInvalidCase InvalidCases[] =
	{
		{TEXT("Duplicate part"), TEXT("is defined more than once"),
			[](USwarmFormDefinition& D) { D.Parts[1].PartId = ESwarmPartId::Torso; }},
		{TEXT("None part"), TEXT("cannot be used as a part"),
			[](USwarmFormDefinition& D) { D.Parts[1].PartId = ESwarmPartId::None; }},
		{TEXT("Negative weight"), TEXT("negative allocation weight"),
			[](USwarmFormDefinition& D) { D.Parts[1].AllocationWeight = -1; }},
		{TEXT("Zero weight sum"), TEXT("must sum to a positive value"),
			[](USwarmFormDefinition& D) { D.Parts[0].AllocationWeight = 0; D.Parts[1].AllocationWeight = 0; }},
		{TEXT("Non-positive box ratio"), TEXT("box ratio must be finite and positive"),
			[](USwarmFormDefinition& D) { D.Parts[1].BoxHalfExtentsRatio = FVector(0.45, 0.0, 1.0); }},
		{TEXT("Non-finite anchor"), TEXT("anchors must be finite and within -1 to 1"),
			[](USwarmFormDefinition& D) { D.Parts[1].ParentAnchor.X = std::numeric_limits<double>::quiet_NaN(); }},
		{TEXT("Anchor outside shape"), TEXT("anchors must be finite and within -1 to 1"),
			[](USwarmFormDefinition& D) { D.Parts[1].ChildAnchor = FVector(0.0, 1.5, 0.0); }},
		{TEXT("Negative gap"), TEXT("gap must be finite and non-negative"),
			[](USwarmFormDefinition& D) { D.Parts[1].Gap = -1.0f; }},
		{TEXT("Non-finite rotation"), TEXT("relative rotation must be finite"),
			[](USwarmFormDefinition& D) { D.Parts[1].RelativeRotation.Yaw = std::numeric_limits<double>::quiet_NaN(); }},
		{TEXT("Self parent"), TEXT("cannot be its own parent"),
			[](USwarmFormDefinition& D) { D.Parts[1].ParentPartId = ESwarmPartId::LeftArm; }},
		{TEXT("Undefined parent"), TEXT("refers to an undefined parent"),
			[](USwarmFormDefinition& D) { D.Parts[1].ParentPartId = ESwarmPartId::Head; }},
		{TEXT("Non-torso root"), TEXT("Only the torso can have no parent"),
			[](USwarmFormDefinition& D) { D.Parts[0].ParentPartId = ESwarmPartId::LeftArm; D.Parts[1].ParentPartId = ESwarmPartId::None; }},
		{TEXT("Remainder outside parts"), TEXT("RemainderPartId must be one of the defined parts"),
			[](USwarmFormDefinition& D) { D.RemainderPartId = ESwarmPartId::Head; }},
	};

	for (const FInvalidCase& InvalidCase : InvalidCases)
	{
		USwarmFormDefinition* Definition = CreateTwoPartDefinition();
		InvalidCase.Modify(*Definition);
		Boss->SetFormDefinition(Definition);

		AddExpectedError(InvalidCase.ExpectedLog, EAutomationExpectedErrorFlags::Contains, 1);
		TestFalse(FString::Printf(TEXT("%s is rejected"), InvalidCase.Name), Boss->GenerateInitialParts());
		TestEqual(FString::Printf(TEXT("%s preserves existing particles"), InvalidCase.Name),
			Boss->GetActiveParticleCount(), 90);
	}

	// 부위 ISM은 데이터 목록대로 만들어져야 한다. 몸통 2·왼팔 1·머리 1(합 4)로 90개를 나누면
	// 45/22/22에 버림 나머지 1개가 몸통으로 가서 46/22/22가 된다.
	auto CountPartComponents = [Boss]()
	{
		return TInlineComponentArray<UInstancedStaticMeshComponent*>(Boss).Num();
	};

	USwarmFormDefinition* ThreePartDefinition = CreateTwoPartDefinition();
	FSwarmPartDefinition& HeadDefinition = ThreePartDefinition->Parts.AddDefaulted_GetRef();
	HeadDefinition.PartId = ESwarmPartId::Head;
	HeadDefinition.ParentPartId = ESwarmPartId::Torso;
	HeadDefinition.Shape = ESwarmPrimitive::Sphere;
	HeadDefinition.AllocationWeight = 1;
	Boss->SetFormDefinition(ThreePartDefinition);

	if (TestTrue(TEXT("Three part definition generates"), Boss->GenerateInitialParts()))
	{
		UInstancedStaticMeshComponent* Head = Boss->GetPartInstances(ESwarmPartId::Head);

		if (TestNotNull(TEXT("Head ISM is created from data"), Head))
		{
			TestEqual(TEXT("Head ISM holds its share"), Head->GetInstanceCount(), 22);
			TestTrue(TEXT("Head ISM is attached to its parent torso ISM"),
				Head->GetAttachParent() == Boss->GetPartInstances(ESwarmPartId::Torso));
			TestTrue(TEXT("Head ISM is registered"), Head->IsRegistered());
		}

		TestEqual(TEXT("Three part ISMs exist"), CountPartComponents(), 3);
		TestEqual(TEXT("Three part torso"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 46);
		TestEqual(TEXT("Three part arm"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 22);
		TestEqual(TEXT("Three part head"), CountActivePartParticles(*Boss, ESwarmPartId::Head), 22);

		// 세 부위 모두 데이터 가중치로 재분배한다. 머리 6개를 제거하면 46/22/16(합 84)이고 2:1:1 목표는 42/21/21이다.
		// 목록 순서대로 몸통의 초과 4개와 왼팔의 초과 1개가 부족한 머리로 직접 이동해야 한다.
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestTrue(TEXT("Remove head particle"), Boss->RemoveInstance(Head, 0));
		}

		if (TestTrue(TEXT("Three part rebalance succeeds"), Boss->RebalancePartsFromDefinition()))
		{
			TestEqual(TEXT("Three part rebalance torso"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 42);
			TestEqual(TEXT("Three part rebalance arm"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 21);
			TestEqual(TEXT("Three part rebalance head"), CountActivePartParticles(*Boss, ESwarmPartId::Head), 21);
			TestTrue(TEXT("Head ISM matches its particles"), Head && Head->GetInstanceCount() == 21);
			TestEqual(TEXT("Rebalance keeps survivors"), Boss->GetActiveParticleCount(), 84);
		}

		// 이미 목표 개수이면 다시 눌러도 아무것도 바꾸지 않고 성공한다.
		TestTrue(TEXT("Balanced three part rebalance succeeds"), Boss->RebalancePartsFromDefinition());
		TestEqual(TEXT("Balanced rebalance keeps head"), CountActivePartParticles(*Boss, ESwarmPartId::Head), 21);
	}

	// 데이터에서 머리를 빼고 다시 생성하기 전에는 생성된 부위와 목록이 다르므로 재분배를 거절한다.
	Boss->SetFormDefinition(CreateTwoPartDefinition());
	AddExpectedError(TEXT("differ from the generated parts"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Stale part list rebalance is rejected"), Boss->RebalancePartsFromDefinition());
	TestEqual(TEXT("Rejected rebalance keeps head particles"), CountActivePartParticles(*Boss, ESwarmPartId::Head), 21);

	// 다시 생성하면 목록에서 빠진 머리 ISM을 제거하고 남은 두 부위만 2:1로 채운다.
	if (TestTrue(TEXT("Two part definition regenerates"), Boss->GenerateInitialParts()))
	{
		TestNull(TEXT("Removed head ISM is released"), Boss->GetPartInstances(ESwarmPartId::Head));
		TestEqual(TEXT("Only two part ISMs remain"), CountPartComponents(), 2);
		TestEqual(TEXT("Regenerated torso"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 60);
		TestEqual(TEXT("Regenerated arm"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 30);
		TestEqual(TEXT("Regenerated store size"), Boss->GetParticles().Num(), 90);
	}

	// T 재분배도 입력 시점의 형태 데이터 가중치를 읽어야 한다. 3:4로 39/51을 만든 뒤 왼팔 6개를 제거하면 39/45가 된다.
	// 합계 84를 3:4로 나누면 왼팔 48(버림), 몸통 36이므로 몸통에서 왼팔로 3개가 이동한다.
	Boss->SetFormDefinition(WeightedDefinition);
	UInstancedStaticMeshComponent* Arm = nullptr;

	if (TestTrue(TEXT("Regenerate weighted parts"), Boss->GenerateInitialParts())
		&& TestNotNull(TEXT("Arm component"), Arm = Boss->GetPartInstances(ESwarmPartId::LeftArm)))
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestTrue(TEXT("Remove arm particle"), Boss->RemoveInstance(Arm, 0));
		}

		TestTrue(TEXT("Definition rebalance succeeds"), Boss->RebalancePartsFromDefinition());
		TestEqual(TEXT("Definition rebalance torso"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 36);
		TestEqual(TEXT("Definition rebalance arm"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 48);

		// 생성 후 데이터를 2:1로 바꾸면 다음 재분배부터 새 가중치를 사용한다. 84개는 56/28이다.
		Boss->SetFormDefinition(CreateTwoPartDefinition());
		TestTrue(TEXT("Changed weights rebalance"), Boss->RebalancePartsFromDefinition());
		TestEqual(TEXT("Changed weights torso"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 56);
		TestEqual(TEXT("Changed weights arm"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 28);

		// 가중치 0인 왼팔은 재분배 시 모든 구체를 몸통으로 보낸다.
		Boss->SetFormDefinition(ZeroArmDefinition);
		TestTrue(TEXT("Zero weight rebalance"), Boss->RebalancePartsFromDefinition());
		TestEqual(TEXT("Zero weight arm empties"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 0);
		TestEqual(TEXT("Zero weight torso receives all"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 84);

		// 데이터가 없으면 재분배하지 않고 기존 소속을 유지한다.
		Boss->SetFormDefinition(nullptr);
		AddExpectedError(TEXT("FormDefinition is not assigned"), EAutomationExpectedErrorFlags::Contains, 1);
		TestFalse(TEXT("Missing definition rebalance rejected"), Boss->RebalancePartsFromDefinition());
		TestEqual(TEXT("Missing definition preserves torso"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 84);
	}

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmPartPlacementTest, "SwarmBoss.Stage2.PartPlacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 연결점·Gap 규칙으로 부위 ISM을 부모 ISM 기준 위치에 놓는지, ISM 부착 계층이 데이터의 부모 관계를 따르는지,
// 재분배로 도형 크기가 바뀌면 관절 위치를 다시 계산하면서 구체의 화면 위치를 유지하는지, 0개 부위를 점으로 다루는지 검증한다.
bool FSwarmPartPlacementTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);

	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();

	if (!TestNotNull(TEXT("Manager"), Boss))
	{
		World->DestroyWorld(false);
		return false;
	}

	// 몸통 2, 왼팔·머리·왼손 각 1(합 5)로 기본 90개를 나누면 36/18/18/18이다.
	// 왼팔: 몸통 왼쪽 면 위쪽 절반 지점에 오른쪽 면을 맞추고 30cm 띄운다.
	// 머리: 구형, 몸통 윗면 중앙에 아랫점을 맞추고 붙인다(Gap 0). Yaw 45도 회전을 준다.
	// 왼손: 왼팔 아랫면 중앙에 윗면을 맞추고 10cm 띄운다. 왼팔의 자식이므로 왼팔 ISM에 붙어야 한다.
	USwarmFormDefinition* Definition = CreateTwoPartDefinition();
	FSwarmPartDefinition& ArmDefinition = Definition->Parts[1];
	ArmDefinition.ParentAnchor = FVector(0.0, -1.0, 0.5);
	ArmDefinition.ChildAnchor = FVector(0.0, 1.0, 0.5);
	ArmDefinition.Gap = 30.0f;

	FSwarmPartDefinition& HeadDefinition = Definition->Parts.AddDefaulted_GetRef();
	HeadDefinition.PartId = ESwarmPartId::Head;
	HeadDefinition.ParentPartId = ESwarmPartId::Torso;
	HeadDefinition.Shape = ESwarmPrimitive::Sphere;
	HeadDefinition.AllocationWeight = 1;
	HeadDefinition.ParentAnchor = FVector(0.0, 0.0, 1.0);
	HeadDefinition.ChildAnchor = FVector(0.0, 0.0, -1.0);
	HeadDefinition.RelativeRotation = FRotator(0.0, 45.0, 0.0);

	FSwarmPartDefinition& HandDefinition = Definition->Parts.AddDefaulted_GetRef();
	HandDefinition.PartId = ESwarmPartId::LeftHand;
	HandDefinition.ParentPartId = ESwarmPartId::LeftArm;
	HandDefinition.Shape = ESwarmPrimitive::Box;
	HandDefinition.BoxHalfExtentsRatio = FVector(0.3, 0.3, 0.3);
	HandDefinition.AllocationWeight = 1;
	HandDefinition.ParentAnchor = FVector(0.0, 0.0, -1.0);
	HandDefinition.ChildAnchor = FVector(0.0, 0.0, 1.0);
	HandDefinition.Gap = 10.0f;

	// 연결점 복사본을 남겨 두고 기대 위치 계산에 사용한다. 구체 반지름은 기본 10cm이므로 관절마다 두 반지름 20cm를 더한다.
	const FSwarmPartDefinition TorsoPart = Definition->Parts[0];
	const FSwarmPartDefinition ArmPart = Definition->Parts[1];
	const FSwarmPartDefinition HeadPart = Definition->Parts[2];
	const FSwarmPartDefinition HandPart = Definition->Parts[3];
	constexpr double TwoRadii = 20.0;

	auto ExpectedArmLocation = [&](const FVector& TorsoHalf, const FVector& ArmHalf)
	{
		return FVector(0.0, -(TorsoHalf.Y + ArmPart.Gap + TwoRadii + ArmHalf.Y), 0.5 * TorsoHalf.Z - 0.5 * ArmHalf.Z);
	};

	auto ExpectedHandLocation = [&](const FVector& ArmHalf, const FVector& HandHalf)
	{
		return FVector(0.0, 0.0, -(ArmHalf.Z + HandPart.Gap + TwoRadii + HandHalf.Z));
	};

	// 바닥 맞춤: 몸통 아래면·왼팔 아래면·왼손 아래면 중 가장 낮은 경계에서 구체 반지름 10cm를 더 내려간 곳이 Manager 높이가 되도록
	// 몸통을 올린다. 머리는 몸통 위에 있으므로 바닥 후보가 아니다. 반환값은 몸통의 상대 위치이다.
	auto ExpectedTorsoLocation = [&](const FVector& TorsoHalf, const FVector& ArmHalf, const FVector& HandHalf)
	{
		const double ArmZ = ExpectedArmLocation(TorsoHalf, ArmHalf).Z;
		const double HandZ = ArmZ + ExpectedHandLocation(ArmHalf, HandHalf).Z;
		const double LowestZ = FMath::Min3(-TorsoHalf.Z, ArmZ - ArmHalf.Z, HandZ - HandHalf.Z) - 0.5 * TwoRadii;
		return FVector(0.0, 0.0, -LowestZ);
	};

	// 실제 활성 구체의 가장 낮은 표면이 Manager 높이보다 아래로 내려가지 않는지 확인한다(도형 경계 안에 있으므로 같거나 위).
	auto LowestParticleSurfaceZ = [&]()
	{
		double LowestZ = TNumericLimits<double>::Max();

		for (const FSwarmParticle& Particle : Boss->GetParticles())
		{
			if (Particle.bActive)
			{
				const FVector World = Boss->GetPartInstances(Particle.PartId)->GetComponentTransform().TransformPosition(
					Particle.TargetLocalPosition);
				LowestZ = FMath::Min(LowestZ, World.Z - 0.5 * TwoRadii);
			}
		}

		return LowestZ;
	};

	Boss->SetActorLocation(FVector(0.0, 0.0, 500.0));
	Boss->SetFormDefinition(Definition);

	if (!TestTrue(TEXT("Four part definition generates"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	UInstancedStaticMeshComponent* Torso = Boss->GetPartInstances(ESwarmPartId::Torso);
	UInstancedStaticMeshComponent* Arm = Boss->GetPartInstances(ESwarmPartId::LeftArm);
	UInstancedStaticMeshComponent* Head = Boss->GetPartInstances(ESwarmPartId::Head);
	UInstancedStaticMeshComponent* Hand = Boss->GetPartInstances(ESwarmPartId::LeftHand);

	if (!TestNotNull(TEXT("Torso"), Torso) || !TestNotNull(TEXT("Arm"), Arm)
		|| !TestNotNull(TEXT("Head"), Head) || !TestNotNull(TEXT("Hand"), Hand))
	{
		World->DestroyWorld(false);
		return false;
	}

	// 부착 계층: 몸통은 루트, 왼팔·머리는 몸통, 왼손은 왼팔에 붙는다.
	TestTrue(TEXT("Torso attaches to root"), Torso->GetAttachParent() == Boss->GetRootComponent());
	TestTrue(TEXT("Arm attaches to torso"), Arm->GetAttachParent() == Torso);
	TestTrue(TEXT("Head attaches to torso"), Head->GetAttachParent() == Torso);
	TestTrue(TEXT("Hand attaches to arm"), Hand->GetAttachParent() == Arm);

	const FVector TorsoHalf = ExpectedHalfExtents(TorsoPart, 36);
	const FVector ArmHalf = ExpectedHalfExtents(ArmPart, 18);
	const FVector HeadHalf = ExpectedHalfExtents(HeadPart, 18);
	const FVector HandHalf = ExpectedHalfExtents(HandPart, 18);

	TestTrue(TEXT("Torso is lifted so the lowest part touches the manager"), Torso->GetRelativeLocation().Equals(
		ExpectedTorsoLocation(TorsoHalf, ArmHalf, HandHalf), 1.e-3));
	TestTrue(TEXT("No particle sinks below the manager"), LowestParticleSurfaceZ() >= Boss->GetActorLocation().Z - 1.e-3);
	TestTrue(TEXT("Arm follows torso side anchor"),
		Arm->GetRelativeLocation().Equals(ExpectedArmLocation(TorsoHalf, ArmHalf), 1.e-3));
	// 구형 머리의 아랫점 (0, 0, -R)은 Yaw 회전에 영향을 받지 않는다.
	TestTrue(TEXT("Head sits on torso top"), Head->GetRelativeLocation().Equals(
		FVector(0.0, 0.0, TorsoHalf.Z + TwoRadii + HeadHalf.X), 1.e-3));
	TestTrue(TEXT("Head keeps data rotation"), Head->GetRelativeRotation().Equals(HeadPart.RelativeRotation, 1.e-3));
	TestTrue(TEXT("Hand hangs below arm"),
		Hand->GetRelativeLocation().Equals(ExpectedHandLocation(ArmHalf, HandHalf), 1.e-3));

	// 몸통 10개를 제거하면 26/18이고, 2:1 재분배 목표는 몸통 30·왼팔 14(44 / 3 버림)이다.
	// 재분배 직후 왼팔이 새 크기에 맞춰 이동하고, 그 자식인 왼손도 따라 움직이지만 구체의 화면 위치는 그대로여야 한다.
	for (int32 Index = 0; Index < 10; ++Index)
	{
		TestTrue(TEXT("Remove torso particle"), Boss->RemoveInstance(Torso, 0));
	}

	TMap<int32, FVector> HandWorldPositions;

	for (int32 ParticleId = 0; ParticleId < Boss->GetParticles().Num(); ++ParticleId)
	{
		const FSwarmParticle& Particle = Boss->GetParticles()[ParticleId];

		if (Particle.bActive && Particle.PartId == ESwarmPartId::LeftHand)
		{
			HandWorldPositions.Add(ParticleId, Hand->GetComponentTransform().TransformPosition(Particle.LocalPosition));
		}
	}

	if (TestTrue(TEXT("Rebalance with four parts"), Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 2, 1)))
	{
		const FVector NewTorsoHalf = ExpectedHalfExtents(TorsoPart, 30);
		const FVector NewArmHalf = ExpectedHalfExtents(ArmPart, 14);

		TestTrue(TEXT("Arm joint follows new sizes"),
			Arm->GetRelativeLocation().Equals(ExpectedArmLocation(NewTorsoHalf, NewArmHalf), 1.e-3));
		TestTrue(TEXT("Hand joint follows new arm size"),
			Hand->GetRelativeLocation().Equals(ExpectedHandLocation(NewArmHalf, HandHalf), 1.e-3));
		TestTrue(TEXT("Torso height follows new sizes"), Torso->GetRelativeLocation().Equals(
			ExpectedTorsoLocation(NewTorsoHalf, NewArmHalf, HandHalf), 1.e-3));
		TestTrue(TEXT("No particle target sinks below the manager after rebalance"),
			LowestParticleSurfaceZ() >= Boss->GetActorLocation().Z - 1.e-3);

		bool bHandParticlesStayed = true;
		bool bHandInstancesMatch = true;

		// 관절 재배치는 중앙 좌표만 고치고 인스턴스는 다음 이동 갱신에서 쓴다. 시간 0으로 한 번 갱신해 인스턴스를 맞춘 뒤 비교한다.
		// 이 테스트의 앞선 제거로 남은 자동 재분배 대기가 Tick에서 실행되지 않도록 자동 재분배는 끈다.
		Boss->SetAutoRebalanceOnLoss(false);
		Boss->Tick(0.0f);

		for (const TPair<int32, FVector>& Pair : HandWorldPositions)
		{
			const FSwarmParticle& Particle = Boss->GetParticles()[Pair.Key];
			const FVector WorldPosition = Hand->GetComponentTransform().TransformPosition(Particle.LocalPosition);
			FTransform InstanceTransform;

			bHandParticlesStayed &= WorldPosition.Equals(Pair.Value, 1.e-3);
			bHandInstancesMatch &= Hand->GetInstanceTransform(Particle.InstanceIndex, InstanceTransform, /*bWorldSpace=*/true)
				&& InstanceTransform.GetLocation().Equals(Pair.Value, 1.e-3);
		}

		TestTrue(TEXT("Hand particles keep world positions at rebalance"), bHandParticlesStayed);
		TestTrue(TEXT("Hand instances keep world positions at rebalance"), bHandInstancesMatch);
		TestTrue(TEXT("Moved hand starts gliding to its local targets"), Boss->IsActorTickEnabled());

		// 재구성 시간이 지나면 왼손 구체는 왼손 ISM 기준 기존 목표에 도착한다.
		for (int32 Step = 0; Step < 10; ++Step)
		{
			Boss->Tick(0.1f);
		}

		bool bHandArrived = true;

		for (const TPair<int32, FVector>& Pair : HandWorldPositions)
		{
			const FSwarmParticle& Particle = Boss->GetParticles()[Pair.Key];
			bHandArrived &= Particle.LocalPosition.Equals(Particle.TargetLocalPosition, 1.e-3);
		}

		TestTrue(TEXT("Hand particles arrive at local targets"), bHandArrived);
		TestFalse(TEXT("Movement finishes"), Boss->IsActorTickEnabled());
	}

	// 0개 부위는 크기 0인 점으로 다룬다. 왼팔 가중치가 0이면 왼팔 중심이 몸통 왼쪽 면에서 (Gap + 두 반지름)만큼 떨어진다.
	USwarmFormDefinition* ZeroArmDefinition = CreateTwoPartDefinition();
	ZeroArmDefinition->Parts[1].AllocationWeight = 0;
	Boss->SetFormDefinition(ZeroArmDefinition);

	if (TestTrue(TEXT("Zero arm definition generates"), Boss->GenerateInitialParts()))
	{
		const FVector ZeroTorsoHalf = ExpectedHalfExtents(ZeroArmDefinition->Parts[0], 90);
		TestTrue(TEXT("Removed hand ISM is released"), Boss->GetPartInstances(ESwarmPartId::LeftHand) == nullptr);
		TestTrue(TEXT("Empty arm is placed as a point"), Boss->GetPartInstances(ESwarmPartId::LeftArm)->GetRelativeLocation().Equals(
			FVector(0.0, -(ZeroTorsoHalf.Y + TwoPartArmGap + TwoRadii), 0.0), 1.e-3));
		// 빈 왼팔은 바닥 계산에서 빠지므로 몸통 아래 구체 표면이 Manager 높이에 온다.
		TestTrue(TEXT("Empty arm is ignored for grounding"), Boss->GetPartInstances(ESwarmPartId::Torso)->GetRelativeLocation().Equals(
			FVector(0.0, 0.0, ZeroTorsoHalf.Z + 0.5 * TwoRadii), 1.e-3));
	}

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmTransferPlannerTest, "SwarmBoss.Stage2.TransferPlanner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 부위 트리의 관절별 흐름 계산, 이웃 부위끼리만 넘기는 계획, 길목이 모자랄 때의 경로 연장, 잘못된 입력 거부를 검증한다.
bool FSwarmTransferPlannerTest::RunTest(const FString& Parameters)
{
	// 몸통(최상위) ─ 머리, 몸통 ─ 왼팔 ─ 왼손.
	const TMap<ESwarmPartId, ESwarmPartId> Parents =
	{
		{ESwarmPartId::Torso, ESwarmPartId::None},
		{ESwarmPartId::Head, ESwarmPartId::Torso},
		{ESwarmPartId::LeftArm, ESwarmPartId::Torso},
		{ESwarmPartId::LeftHand, ESwarmPartId::LeftArm}
	};

	auto ExpectShipment = [this](const TArray<FSwarmTransferShipment>& Shipments, int32 Index,
		std::initializer_list<ESwarmPartId> Path, int32 Count)
	{
		const FString Label = FString::Printf(TEXT("Shipment %d"), Index);

		if (TestTrue(*(Label + TEXT(" exists")), Shipments.IsValidIndex(Index)))
		{
			TestTrue(*(Label + TEXT(" path")), Shipments[Index].Path == TArray<ESwarmPartId>(Path));
			TestEqual(*(Label + TEXT(" count")), Shipments[Index].Count, Count);
		}
	};

	// 머리만 부족(-6): 손 → 팔 3, 팔 → 몸통 4(팔 1 + 손 3), 몸통 → 머리 6. 모두 관절 하나만 건넌다.
	TArray<FSwarmTransferShipment> Shipments;
	TestTrue(TEXT("Head loss plan"), FSwarmTransferPlanner::Plan(Parents,
		{{ESwarmPartId::Torso, 40}, {ESwarmPartId::Head, 10}, {ESwarmPartId::LeftArm, 20}, {ESwarmPartId::LeftHand, 20}},
		{{ESwarmPartId::Torso, 38}, {ESwarmPartId::Head, 16}, {ESwarmPartId::LeftArm, 19}, {ESwarmPartId::LeftHand, 17}},
		Shipments));
	TestEqual(TEXT("Head loss shipment count"), Shipments.Num(), 3);
	ExpectShipment(Shipments, 0, {ESwarmPartId::LeftHand, ESwarmPartId::LeftArm}, 3);
	ExpectShipment(Shipments, 1, {ESwarmPartId::LeftArm, ESwarmPartId::Torso}, 4);
	ExpectShipment(Shipments, 2, {ESwarmPartId::Torso, ESwarmPartId::Head}, 6);

	// 팔이 2개만 남고 손도 부족: 몸통 → 팔 23, 팔 → 손 5. 팔은 자기 구체 2개만 보낼 수 있으므로
	// 몸통에서 오는 23개 중 3개가 팔을 건너뛰어 손으로 바로 간다.
	TestTrue(TEXT("Relay plan"), FSwarmTransferPlanner::Plan(Parents,
		{{ESwarmPartId::Torso, 60}, {ESwarmPartId::Head, 18}, {ESwarmPartId::LeftArm, 2}, {ESwarmPartId::LeftHand, 10}},
		{{ESwarmPartId::Torso, 37}, {ESwarmPartId::Head, 18}, {ESwarmPartId::LeftArm, 20}, {ESwarmPartId::LeftHand, 15}},
		Shipments));
	TestEqual(TEXT("Relay shipment count"), Shipments.Num(), 3);
	ExpectShipment(Shipments, 0, {ESwarmPartId::Torso, ESwarmPartId::LeftArm}, 20);
	ExpectShipment(Shipments, 1, {ESwarmPartId::LeftArm, ESwarmPartId::LeftHand}, 2);
	ExpectShipment(Shipments, 2, {ESwarmPartId::Torso, ESwarmPartId::LeftArm, ESwarmPartId::LeftHand}, 3);

	// 팔이 0개면 손으로 가는 몫 전부가 팔을 건너뛴다.
	TestTrue(TEXT("Empty relay plan"), FSwarmTransferPlanner::Plan(Parents,
		{{ESwarmPartId::Torso, 50}, {ESwarmPartId::Head, 10}, {ESwarmPartId::LeftArm, 0}, {ESwarmPartId::LeftHand, 0}},
		{{ESwarmPartId::Torso, 40}, {ESwarmPartId::Head, 10}, {ESwarmPartId::LeftArm, 6}, {ESwarmPartId::LeftHand, 4}},
		Shipments));
	TestEqual(TEXT("Empty relay shipment count"), Shipments.Num(), 2);
	ExpectShipment(Shipments, 0, {ESwarmPartId::Torso, ESwarmPartId::LeftArm}, 6);
	ExpectShipment(Shipments, 1, {ESwarmPartId::Torso, ESwarmPartId::LeftArm, ESwarmPartId::LeftHand}, 4);

	// 바꿀 것이 없으면 빈 계획으로 성공한다.
	const TMap<ESwarmPartId, int32> Balanced =
		{{ESwarmPartId::Torso, 40}, {ESwarmPartId::Head, 10}, {ESwarmPartId::LeftArm, 20}, {ESwarmPartId::LeftHand, 20}};
	TestTrue(TEXT("Balanced plan"), FSwarmTransferPlanner::Plan(Parents, Balanced, Balanced, Shipments));
	TestEqual(TEXT("Balanced plan is empty"), Shipments.Num(), 0);

	// 개수 합이 다르거나, 최상위가 둘이거나, 부모가 순환하면 거부한다.
	TestFalse(TEXT("Count sum mismatch rejected"), FSwarmTransferPlanner::Plan(Parents, Balanced,
		{{ESwarmPartId::Torso, 41}, {ESwarmPartId::Head, 10}, {ESwarmPartId::LeftArm, 20}, {ESwarmPartId::LeftHand, 20}},
		Shipments));

	TMap<ESwarmPartId, ESwarmPartId> TwoRoots = Parents;
	TwoRoots[ESwarmPartId::Head] = ESwarmPartId::None;
	TestFalse(TEXT("Two roots rejected"), FSwarmTransferPlanner::Plan(TwoRoots, Balanced, Balanced, Shipments));

	TMap<ESwarmPartId, ESwarmPartId> Cycle = Parents;
	Cycle[ESwarmPartId::LeftArm] = ESwarmPartId::LeftHand;
	TestFalse(TEXT("Cycle rejected"), FSwarmTransferPlanner::Plan(Cycle, Balanced, Balanced, Shipments));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmNeighborTransferTest, "SwarmBoss.Stage2.NeighborTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Manager 재분배가 이웃 전달 계획대로 구체 소속을 바꾸는지, 길목이 모자라면 몸통 구체가 팔을 건너뛰어 손으로 가는지,
// 보낼 구체를 관절에 가까운 것부터 고르는지 검증한다.
bool FSwarmNeighborTransferTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);

	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();

	if (!TestNotNull(TEXT("Manager"), Boss))
	{
		World->DestroyWorld(false);
		return false;
	}

	// 몸통 2, 왼팔 1, 왼손 1(왼팔의 자식, 왼팔 아랫면에 붙음). 기본 90개는 46/22/22(나머지 1은 몸통).
	USwarmFormDefinition* Definition = CreateTwoPartDefinition();
	FSwarmPartDefinition& HandDefinition = Definition->Parts.AddDefaulted_GetRef();
	HandDefinition.PartId = ESwarmPartId::LeftHand;
	HandDefinition.ParentPartId = ESwarmPartId::LeftArm;
	HandDefinition.Shape = ESwarmPrimitive::Box;
	HandDefinition.BoxHalfExtentsRatio = FVector(0.5, 0.5, 0.5);
	HandDefinition.AllocationWeight = 1;
	HandDefinition.ParentAnchor = FVector(0.0, 0.0, -1.0);
	HandDefinition.ChildAnchor = FVector(0.0, 0.0, 1.0);
	HandDefinition.Gap = 10.0f;
	Boss->SetFormDefinition(Definition);

	if (!TestTrue(TEXT("Three part chain generates"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	UInstancedStaticMeshComponent* Arm = Boss->GetPartInstances(ESwarmPartId::LeftArm);
	UInstancedStaticMeshComponent* Hand = Boss->GetPartInstances(ESwarmPartId::LeftHand);

	// 왼팔 20개(2개 남음), 왼손 10개(12개 남음)를 제거한다. 합계 60의 목표는 몸통 30, 왼팔 15, 왼손 15이므로
	// 몸통 → 왼팔 16, 왼팔 → 왼손 3이 필요하고, 왼팔은 2개만 있으므로 몸통 구체 1개가 왼팔을 건너뛰어 왼손으로 간다.
	for (int32 Index = 0; Index < 20; ++Index)
	{
		TestTrue(TEXT("Remove arm particle"), Boss->RemoveInstance(Arm, 0));
	}

	for (int32 Index = 0; Index < 10; ++Index)
	{
		TestTrue(TEXT("Remove hand particle"), Boss->RemoveInstance(Hand, 0));
	}

	TArray<ESwarmPartId> PartsBefore;

	for (const FSwarmParticle& Particle : Boss->GetParticles())
	{
		PartsBefore.Add(Particle.bActive ? Particle.PartId : ESwarmPartId::None);
	}

	if (TestTrue(TEXT("Neighbor rebalance succeeds"), Boss->RebalancePartsFromDefinition()))
	{
		TestEqual(TEXT("Torso count"), CountActivePartParticles(*Boss, ESwarmPartId::Torso), 30);
		TestEqual(TEXT("Arm count"), CountActivePartParticles(*Boss, ESwarmPartId::LeftArm), 15);
		TestEqual(TEXT("Hand count"), CountActivePartParticles(*Boss, ESwarmPartId::LeftHand), 15);

		int32 TorsoToArm = 0;
		int32 TorsoToHand = 0;
		int32 ArmToHand = 0;
		int32 OtherMoves = 0;

		for (int32 ParticleId = 0; ParticleId < Boss->GetParticles().Num(); ++ParticleId)
		{
			const FSwarmParticle& Particle = Boss->GetParticles()[ParticleId];
			const ESwarmPartId Before = PartsBefore[ParticleId];

			if (!Particle.bActive || Before == Particle.PartId)
			{
				continue;
			}

			if (Before == ESwarmPartId::Torso && Particle.PartId == ESwarmPartId::LeftArm)
			{
				++TorsoToArm;
			}
			else if (Before == ESwarmPartId::Torso && Particle.PartId == ESwarmPartId::LeftHand)
			{
				++TorsoToHand;
			}
			else if (Before == ESwarmPartId::LeftArm && Particle.PartId == ESwarmPartId::LeftHand)
			{
				++ArmToHand;
			}
			else
			{
				++OtherMoves;
			}
		}

		TestEqual(TEXT("Torso sends to arm"), TorsoToArm, 15);
		TestEqual(TEXT("Torso skips the short arm to reach the hand"), TorsoToHand, 1);
		TestEqual(TEXT("Arm sends all it has to the hand"), ArmToHand, 2);
		TestEqual(TEXT("No other moves"), OtherMoves, 0);

		// 전달 직후 구체는 아직 출발 위치에 있다. 몸통에서 나간 구체는 남은 몸통 구체보다 어깨 관절에 가까워야 한다.
		const FVector Shoulder = Boss->GetJointWorldLocation(ESwarmPartId::Torso, ESwarmPartId::LeftArm);
		double FarthestSent = 0.0;
		double NearestKept = TNumericLimits<double>::Max();

		for (int32 ParticleId = 0; ParticleId < Boss->GetParticles().Num(); ++ParticleId)
		{
			const FSwarmParticle& Particle = Boss->GetParticles()[ParticleId];

			if (!Particle.bActive || PartsBefore[ParticleId] != ESwarmPartId::Torso)
			{
				continue;
			}

			const FVector WorldPosition = Boss->GetPartInstances(Particle.PartId)->GetComponentTransform().TransformPosition(
				Particle.LocalPosition);
			const double Distance = FVector::Dist(WorldPosition, Shoulder);

			if (Particle.PartId == ESwarmPartId::Torso)
			{
				NearestKept = FMath::Min(NearestKept, Distance);
			}
			else
			{
				FarthestSent = FMath::Max(FarthestSent, Distance);
			}
		}

		TestTrue(TEXT("Torso sends particles nearest the shoulder"), FarthestSent <= NearestKept + 1.e-3);
	}

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmManagerDensityTest, "SwarmBoss.Stage2.ManagerDensity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Manager 기본 배치 계수로 거인 부위에 쓰는 도형(구, 몸통·팔·손발 비율 박스)이 여러 개수에서 샘플링에 성공하는지 확인한다.
bool FSwarmManagerDensityTest::RunTest(const FString& Parameters)
{
	const FVector BoxRatios[] = {FVector(1.0, 0.75, 1.5), FVector(0.45, 0.45, 1.0), FVector(0.3, 0.3, 1.0), FVector(0.5, 0.5, 0.5)};
	const int32 Counts[] = {2, 10, 100, 1000, 2000};

	for (int32 Count : Counts)
	{
		FSwarmSamplingSettings Settings;
		Settings.ParticleCount = Count;
		Settings.PackingCoefficient = ManagerPackingCoefficient;
		Settings.Shape = ESwarmPrimitive::Sphere;
		const FSwarmSamplingResult SphereResult = FSwarmShapeSampler::GenerateTargets(Settings);
		TestTrue(*FString::Printf(TEXT("Sphere %d succeeds (%s)"), Count, *SphereResult.Message), SphereResult.bSuccess);

		for (const FVector& Ratio : BoxRatios)
		{
			Settings.Shape = ESwarmPrimitive::Box;
			Settings.BoxHalfExtents = Ratio;
			const FSwarmSamplingResult BoxResult = FSwarmShapeSampler::GenerateTargets(Settings);
			TestTrue(*FString::Printf(TEXT("Box %s x %d succeeds (%s)"), *Ratio.ToString(), Count, *BoxResult.Message),
				BoxResult.bSuccess);
			AddInfo(FString::Printf(TEXT("Density Box %s Count=%d Attempts=%d Expansion=%.3f"),
				*Ratio.ToString(), Count, BoxResult.Attempts, BoxResult.ExpansionRatio));
		}
	}

	// 좌우 부위는 도형·비율·개수가 같아도 시드가 달라 서로 다른 배치가 나와야 한다. 같은 부위는 같은 배치를 재현한다.
	TestNotEqual(TEXT("Left and right seeds differ"), ASwarmBossManager::GetPartSamplingSeed(ESwarmPartId::LeftArm),
		ASwarmBossManager::GetPartSamplingSeed(ESwarmPartId::RightArm));

	FSwarmSamplingSettings ArmSettings;
	ArmSettings.Shape = ESwarmPrimitive::Box;
	ArmSettings.BoxHalfExtents = FVector(0.45, 0.45, 1.0);
	ArmSettings.ParticleCount = 100;
	ArmSettings.PackingCoefficient = ManagerPackingCoefficient;
	ArmSettings.Seed = ASwarmBossManager::GetPartSamplingSeed(ESwarmPartId::LeftArm);
	const TArray<FVector> LeftPoints = FSwarmShapeSampler::GenerateTargets(ArmSettings).Points;
	const TArray<FVector> LeftAgain = FSwarmShapeSampler::GenerateTargets(ArmSettings).Points;
	ArmSettings.Seed = ASwarmBossManager::GetPartSamplingSeed(ESwarmPartId::RightArm);
	const TArray<FVector> RightPoints = FSwarmShapeSampler::GenerateTargets(ArmSettings).Points;
	TestTrue(TEXT("Same part reproduces its layout"), LeftPoints == LeftAgain);
	TestFalse(TEXT("Left and right arms get different layouts"), LeftPoints == RightPoints);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmProjectileTest, "SwarmBoss.Stage2.ProjectileAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 관통 구체 투사체가 경로에 닿는 구체(중심이 경로에서 투사체 반지름 + 구체 반지름 안)를 모두 지우고 멈추지 않고 지나가는지,
// 대응표가 유지되는지, 벽에 닿으면 사라지는지, 빈 공간에서는 그대로 날아가는지 검증한다.
bool FSwarmProjectileTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);

	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = SpawnTwoPartManager(World, /*bAutoRebalanceOnLoss=*/true);

	if (!TestNotNull(TEXT("Manager"), Boss) || !TestTrue(TEXT("Generate"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	constexpr float ProjectileRadius = 15.0f;
	constexpr double ParticleRadius = 10.0;
	constexpr double Tolerance = 0.5;

	// 몸통 중심을 지나 +X로 날아가는 직선. 구체 중심과 이 직선 사이 거리로 닿는지 판단한다.
	const FVector TorsoCenter = Boss->GetPartInstances(ESwarmPartId::Torso)->GetComponentLocation();
	const FVector Direction(1.0, 0.0, 0.0);
	const FVector LaunchLocation = TorsoCenter - Direction * 1000.0;

	TArray<double> DistancesToPath;
	TArray<bool> WasActive;

	for (const FSwarmParticle& Particle : Boss->GetParticles())
	{
		const FVector WorldPosition = Particle.bActive
			? Boss->GetPartInstances(Particle.PartId)->GetComponentTransform().TransformPosition(Particle.LocalPosition)
			: FVector::ZeroVector;
		DistancesToPath.Add(FMath::PointDistToLine(WorldPosition, Direction, LaunchLocation));
		WasActive.Add(Particle.bActive);
	}

	ASwarmProjectile* Projectile = World->SpawnActor<ASwarmProjectile>(LaunchLocation, FRotator::ZeroRotator);

	if (TestNotNull(TEXT("Projectile"), Projectile))
	{
		const int32 Before = Boss->GetActiveParticleCount();
		Projectile->Launch(Direction, 6000.0f, ProjectileRadius);
		bool bStopped = false;

		// 0.05초 × 20 = 6000cm를 날아 몸 전체를 지나간다.
		for (int32 Step = 0; Step < 20 && !bStopped; ++Step)
		{
			bStopped = Projectile->Advance(0.05f);
		}

		TestFalse(TEXT("Projectile passes through the swarm without stopping"), bStopped);
		TestTrue(TEXT("Projectile removes particles along its path"), Projectile->GetTotalRemovedCount() > 0);
		TestEqual(TEXT("Active count drops by the removed count"), Boss->GetActiveParticleCount(),
			Before - Projectile->GetTotalRemovedCount());

		bool bTouchingRemoved = true;
		bool bFarKept = true;

		for (int32 ParticleId = 0; ParticleId < Boss->GetParticles().Num(); ++ParticleId)
		{
			if (!WasActive[ParticleId])
			{
				continue;
			}

			const bool bActive = Boss->GetParticles()[ParticleId].bActive;
			bTouchingRemoved &= !(DistancesToPath[ParticleId] < ProjectileRadius + ParticleRadius - Tolerance && bActive);
			bFarKept &= !(DistancesToPath[ParticleId] > ProjectileRadius + ParticleRadius + Tolerance && !bActive);
		}

		TestTrue(TEXT("Every particle touching the path is removed"), bTouchingRemoved);
		TestTrue(TEXT("Particles away from the path are kept"), bFarKept);
		TestEqual(TEXT("Damage window closes after leaving the swarm bounds"), Boss->GetOpenDamageWindowCount(), 0);
		TestTrue(TEXT("Loss waits for the next Tick to rebalance"), Boss->IsRebalancePending());

		for (ESwarmPartId PartId : {ESwarmPartId::Torso, ESwarmPartId::LeftArm})
		{
			TestEqual(TEXT("ISM count matches part particle count"),
				Boss->GetPartInstances(PartId)->GetInstanceCount(), CountActivePartParticles(*Boss, PartId));
		}
	}

	// 보스 안에서 사라지면(바닥·벽·수명 등) EndPlay에서 피해 구간을 닫는다. 첫 투사체의 터널과 겹치지 않게 +Y로 쏜다.
	// 테스트용 임시 월드에는 World Context가 없어 Actor를 파괴할 때마다 엔진 경고가 나온다(횟수 무관).
	AddExpectedMessage(TEXT("World has no context"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0, false);

	ASwarmProjectile* Inside = World->SpawnActor<ASwarmProjectile>(TorsoCenter - FVector(0.0, 1000.0, 0.0), FRotator::ZeroRotator);

	if (TestNotNull(TEXT("Inside projectile"), Inside))
	{
		Inside->Launch(FVector(0.0, 1.0, 0.0), 6000.0f, ProjectileRadius);

		for (int32 Step = 0; Step < 200 && Boss->GetOpenDamageWindowCount() == 0; ++Step)
		{
			Inside->Advance(0.01f);
		}

		TestEqual(TEXT("Hitting the swarm opens a damage window"), Boss->GetOpenDamageWindowCount(), 1);
		Inside->Destroy();
		TestEqual(TEXT("Destroying inside the swarm closes the window"), Boss->GetOpenDamageWindowCount(), 0);
	}

	// 벽(WorldStatic 상자)에 닿으면 멈추고 사라진다.

	const FVector WallLocation = TorsoCenter + FVector(0.0, 0.0, 5000.0);
	AStaticMeshActor* Wall = World->SpawnActor<AStaticMeshActor>(WallLocation, FRotator::ZeroRotator);
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

	if (TestNotNull(TEXT("Wall"), Wall) && TestNotNull(TEXT("Cube mesh"), CubeMesh))
	{
		Wall->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Wall->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
		Wall->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);

		ASwarmProjectile* WallShot = World->SpawnActor<ASwarmProjectile>(WallLocation - Direction * 500.0, FRotator::ZeroRotator);

		if (TestNotNull(TEXT("Wall projectile"), WallShot))
		{
			WallShot->Launch(Direction, 6000.0f, ProjectileRadius);
			TestTrue(TEXT("Projectile stops at a wall"), WallShot->Advance(0.1f));
			TestTrue(TEXT("Projectile is destroyed at the wall"), !IsValid(WallShot) || WallShot->IsActorBeingDestroyed());
		}
	}

	// 위쪽 빈 공간으로 쏘면 아무것도 닿지 않고 이동 거리만큼 앞으로 간다.
	ASwarmProjectile* Miss = World->SpawnActor<ASwarmProjectile>(TorsoCenter + FVector(3000.0, 0.0, 0.0), FRotator::ZeroRotator);

	if (TestNotNull(TEXT("Missing projectile"), Miss))
	{
		const FVector Start = Miss->GetActorLocation();
		Miss->Launch(FVector(0.0, 1.0, 0.0), 6000.0f, ProjectileRadius);
		TestFalse(TEXT("Projectile into empty space does not stop"), Miss->Advance(0.1f));
		TestEqual(TEXT("Projectile into empty space removes nothing"), Miss->GetTotalRemovedCount(), 0);
		TestTrue(TEXT("Projectile moves along its velocity"),
			Miss->GetActorLocation().Equals(Start + FVector(0.0, 600.0, 0.0), 1.e-3));
	}

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmTargetAssignerTest, "SwarmBoss.Stage2.TargetAssigner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 가까운 목표 배정(그리디)의 기본 성질과 통계를 검증한다. 같은 위치면 이동 0, 조금 밀린 격자는 바로 옆 목표,
// 개수가 다르면 거부, 같은 입력이면 같은 결과이고 모든 목표를 한 번씩 쓴다. 그리디가 먼 이동을 남기는 경우는 통계로 드러난다.
bool FSwarmTargetAssignerTest::RunTest(const FString& Parameters)
{
	constexpr double Spacing = 22.0;
	FSwarmAssignSettings Settings;
	Settings.CellSize = Spacing;
	Settings.LongMoveThreshold = 2.0 * Spacing;

	TArray<FVector> Grid;

	for (int32 X = 0; X < 5; ++X)
	{
		for (int32 Y = 0; Y < 5; ++Y)
		{
			for (int32 Z = 0; Z < 5; ++Z)
			{
				Grid.Add(FVector(X, Y, Z) * Spacing);
			}
		}
	}

	// 시작 위치가 목표와 같으면(순서는 반대) 모두 제자리 목표를 받는다.
	TArray<FVector> Reversed = Grid;
	Algo::Reverse(Reversed);
	TArray<int32> Assignment;
	FSwarmAssignStats Stats;

	if (TestTrue(TEXT("Identity assign succeeds"), FSwarmTargetAssigner::Assign(Reversed, Grid, Settings, Assignment, &Stats)))
	{
		bool bSame = true;

		for (int32 Index = 0; Index < Reversed.Num(); ++Index)
		{
			bSame &= Reversed[Index] == Grid[Assignment[Index]];
		}

		TestTrue(TEXT("Every start keeps its own position"), bSame);
		TestEqual(TEXT("Identity max move is zero"), Stats.MaxMove, 0.0);
		TestEqual(TEXT("Identity has no leftovers"), Stats.Leftovers, 0);
	}

	// 격자 전체를 5cm 밀면 모두 바로 옆(원래 칸) 목표로 간다.
	TArray<FVector> Shifted;

	for (const FVector& Point : Grid)
	{
		Shifted.Add(Point + FVector(5.0, 0.0, 0.0));
	}

	if (TestTrue(TEXT("Shifted assign succeeds"), FSwarmTargetAssigner::Assign(Shifted, Grid, Settings, Assignment, &Stats)))
	{
		bool bNeighbor = true;

		for (int32 Index = 0; Index < Shifted.Num(); ++Index)
		{
			bNeighbor &= Assignment[Index] == Index;
		}

		TestTrue(TEXT("Shifted grid maps to the adjacent targets"), bNeighbor);
		TestEqual(TEXT("Shifted max move is the shift"), Stats.MaxMove, 5.0, 1.e-6);
		TestEqual(TEXT("Shifted has no long moves"), Stats.LongMoves, 0);
	}

	TestFalse(TEXT("Count mismatch is rejected"),
		FSwarmTargetAssigner::Assign(TArray<FVector>{FVector::ZeroVector}, Grid, Settings, Assignment));
	TestEqual(TEXT("Rejected assign returns empty"), Assignment.Num(), 0);
	TestTrue(TEXT("Empty input succeeds"), FSwarmTargetAssigner::Assign({}, {}, Settings, Assignment));

	// 무작위 입력도 같은 입력이면 같은 결과이고, 모든 목표를 정확히 한 번 쓴다.
	FRandomStream Random(7);
	TArray<FVector> RandomStarts;
	TArray<FVector> RandomTargets;

	for (int32 Index = 0; Index < 300; ++Index)
	{
		RandomStarts.Add(Random.GetUnitVector() * Random.FRandRange(0.0, 200.0));
		RandomTargets.Add(Random.GetUnitVector() * Random.FRandRange(0.0, 150.0));
	}

	TArray<int32> First;
	TArray<int32> Second;
	FSwarmTargetAssigner::Assign(RandomStarts, RandomTargets, Settings, First);
	FSwarmTargetAssigner::Assign(RandomStarts, RandomTargets, Settings, Second);
	TestTrue(TEXT("Same input gives the same assignment"), First == Second);

	TArray<int32> Sorted = First;
	Sorted.Sort();
	bool bPermutation = Sorted.Num() == RandomTargets.Num();

	for (int32 Index = 0; bPermutation && Index < Sorted.Num(); ++Index)
	{
		bPermutation = Sorted[Index] == Index;
	}

	TestTrue(TEXT("Every target is used exactly once"), bPermutation);

	// 한 줄로 늘어선 목표 10개와, 한 칸씩 오른쪽으로 밀린 시작 위치 10개. 그리디는 9개를 제자리(이동 0)로 확정하고
	// 마지막 시작 위치(x = 220)는 후보 2개를 모두 뺏겨 남은 목표(x = 0)로 220cm를 간다.
	// 도미노는 이를 "모든 구체가 한 칸(22cm)씩 왼쪽으로" 바꾼다.
	TArray<FVector> LineTargets;
	TArray<FVector> LineStarts;

	for (int32 Index = 0; Index < 10; ++Index)
	{
		LineTargets.Add(FVector(Index * Spacing, 0.0, 0.0));
		LineStarts.Add(FVector((Index + 1) * Spacing, 0.0, 0.0));
	}

	FSwarmAssignSettings GreedyOnly = Settings;
	GreedyOnly.CandidateCount = 2;
	GreedyOnly.MaxLongMoveRepairs = 0;

	if (TestTrue(TEXT("Greedy line assign succeeds"), FSwarmTargetAssigner::Assign(LineStarts, LineTargets, GreedyOnly, Assignment, &Stats)))
	{
		TestEqual(TEXT("Last start falls back to the leftover target"), Assignment[9], 0);
		TestEqual(TEXT("One leftover"), Stats.Leftovers, 1);
		TestEqual(TEXT("One long move"), Stats.LongMoves, 1);
		TestEqual(TEXT("Nothing repaired without repairs"), Stats.Repaired, 0);
		TestEqual(TEXT("Leftover moves across the line"), Stats.MaxMove, 10.0 * Spacing, 1.e-6);
	}

	// 기준을 1.5칸으로 두면 한 칸 이동만 허용되어 사슬이 하나로 정해진다(기준 2칸이면 두 칸씩 건너뛰는 더 짧은 사슬이 나온다).
	FSwarmAssignSettings WithRepair = GreedyOnly;
	WithRepair.MaxLongMoveRepairs = 50;
	WithRepair.LongMoveThreshold = 1.5 * Spacing;

	if (TestTrue(TEXT("Repaired line assign succeeds"), FSwarmTargetAssigner::Assign(LineStarts, LineTargets, WithRepair, Assignment, &Stats)))
	{
		bool bShiftedByOne = true;

		for (int32 Index = 0; Index < 10; ++Index)
		{
			bShiftedByOne &= Assignment[Index] == Index;
		}

		TestTrue(TEXT("Domino shifts every start one slot"), bShiftedByOne);
		TestEqual(TEXT("Greedy long move is counted"), Stats.GreedyLongMoves, 1);
		TestEqual(TEXT("One repair"), Stats.Repaired, 1);
		TestEqual(TEXT("No long move remains"), Stats.LongMoves, 0);
		TestEqual(TEXT("Every move is one slot"), Stats.MaxMove, Spacing, 1.e-6);
	}

	// 기준 이내로 갈 수 있는 목표가 없으면 고칠 수 없어 그리디 결과를 그대로 둔다.
	const TArray<FVector> FarStarts{FVector(0.0), FVector(500.0, 0.0, 0.0)};
	const TArray<FVector> FarTargets{FVector(0.0), FVector(1000.0, 0.0, 0.0)};

	if (TestTrue(TEXT("Unrepairable assign succeeds"), FSwarmTargetAssigner::Assign(FarStarts, FarTargets, Settings, Assignment, &Stats)))
	{
		TestTrue(TEXT("Unrepairable keeps the greedy result"), Assignment[0] == 0 && Assignment[1] == 1);
		TestEqual(TEXT("Unrepairable long move stays"), Stats.LongMoves, 1);
		TestEqual(TEXT("Unrepairable is not counted as repaired"), Stats.Repaired, 0);
	}

	// 6×6×6 격자 가운데 줄(y = z = 2)의 구체 6개가 빠진 터널이 있고, 같은 수의 구체가 x = 5 면 바로 바깥(x = 6) 한 칸에 있다.
	// 바깥 구체의 가까운 목표는 이미 격자 구체가 차지해, 그리디는 이들을 터널 안 빈 목표로 멀리 보낸다.
	// 도미노는 바깥 구체가 면으로 한 칸 들어오고 격자 구체들이 터널 쪽으로 한 칸씩 밀리는 사슬로 바꿔야 한다.
	TArray<FVector> TunnelTargets;
	TArray<FVector> TunnelStarts;

	for (int32 X = 0; X < 6; ++X)
	{
		for (int32 Y = 0; Y < 6; ++Y)
		{
			for (int32 Z = 0; Z < 6; ++Z)
			{
				const FVector Point = FVector(X, Y, Z) * Spacing;
				TunnelTargets.Add(Point);

				if (Y != 2 || Z != 2)
				{
					TunnelStarts.Add(Point);
				}
			}
		}
	}

	for (const FIntPoint& Face : {FIntPoint(2, 2), FIntPoint(1, 2), FIntPoint(3, 2), FIntPoint(2, 1), FIntPoint(2, 3), FIntPoint(1, 1)})
	{
		TunnelStarts.Add(FVector(6, Face.X, Face.Y) * Spacing);
	}

	if (TestTrue(TEXT("Tunnel assign succeeds"), FSwarmTargetAssigner::Assign(TunnelStarts, TunnelTargets, Settings, Assignment, &Stats)))
	{
		AddInfo(FString::Printf(TEXT("Tunnel: Greedy long=%d Repaired=%d Final long=%d MaxMove=%.1f"),
			Stats.GreedyLongMoves, Stats.Repaired, Stats.LongMoves, Stats.MaxMove));
		TestTrue(TEXT("Tunnel has greedy long moves"), Stats.GreedyLongMoves > 0);
		TestEqual(TEXT("Domino removes every tunnel long move"), Stats.LongMoves, 0);
		TestTrue(TEXT("Tunnel max move is within the threshold"), Stats.MaxMove <= Settings.LongMoveThreshold + 1.e-6);

		TArray<int32> TunnelSorted = Assignment;
		TunnelSorted.Sort();
		bool bTunnelPermutation = true;

		for (int32 Index = 0; Index < TunnelSorted.Num(); ++Index)
		{
			bTunnelPermutation &= TunnelSorted[Index] == Index;
		}

		TestTrue(TEXT("Tunnel repair keeps every target used once"), bTunnelPermutation);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmAutoRebalanceTest, "SwarmBoss.Stage2.AutoRebalance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 손실 자동 재분배를 검증한다. 피해 구간이 열려 있으면 미루고, 닫힌 다음 Tick에 한 번 재분배한다.
// 구간 없이 지우면 다음 Tick에 바로 재분배하며, 자동을 끄면 재분배하지 않는다. 가까운 목표 배정이므로 평균 이동이 작다.
bool FSwarmAutoRebalanceTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);

	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = SpawnTwoPartManager(World, /*bAutoRebalanceOnLoss=*/true);

	if (!TestNotNull(TEXT("Manager"), Boss) || !TestTrue(TEXT("Generate"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	UInstancedStaticMeshComponent* Torso = Boss->GetPartInstances(ESwarmPartId::Torso);
	UInstancedStaticMeshComponent* Arm = Boss->GetPartInstances(ESwarmPartId::LeftArm);
	const float Duration = Boss->ReconstructionDuration;
	TestTrue(TEXT("Auto rebalance is on by default"), GetDefault<ASwarmBossManager>()->bAutoRebalanceOnLoss);
	TestFalse(TEXT("Nothing pending after generation"), Boss->IsRebalancePending());

	// 부위 구체들의 (목표 - 현재) 평균 거리. 재분배 직후에 재면 이번 배정의 평균 이동 거리다.
	auto AverageMove = [Boss](ESwarmPartId PartId)
	{
		double Sum = 0.0;
		int32 Count = 0;

		for (const FSwarmParticle& Particle : Boss->GetParticles())
		{
			if (Particle.bActive && Particle.PartId == PartId)
			{
				Sum += FVector::Dist(Particle.LocalPosition, Particle.TargetLocalPosition);
				++Count;
			}
		}

		return Count > 0 ? Sum / Count : 0.0;
	};

	// 피해 구간이 열려 있는 동안 지운 구체는 구간이 닫힐 때까지 재분배하지 않는다.
	Boss->BeginDamageWindow();

	for (int32 Index = 0; Index < 9; ++Index)
	{
		Boss->RemoveInstance(Arm, 0);
	}

	TestTrue(TEXT("Loss marks rebalance pending"), Boss->IsRebalancePending());
	TestTrue(TEXT("Loss enables Tick"), Boss->IsActorTickEnabled());
	Boss->Tick(0.01f);
	TestTrue(TEXT("Open window defers rebalance"), Boss->IsRebalancePending());
	TestEqual(TEXT("Arm keeps its reduced count while deferred"), Arm->GetInstanceCount(), 21);

	Boss->EndDamageWindow();
	Boss->Tick(0.0f);
	TestFalse(TEXT("Closing the window rebalances on the next Tick"), Boss->IsRebalancePending());
	TestEqual(TEXT("Torso sends six"), Torso->GetInstanceCount(), 54);
	TestEqual(TEXT("Arm receives six"), Arm->GetInstanceCount(), 27);

	const double TorsoAverageMove = AverageMove(ESwarmPartId::Torso);
	AddInfo(FString::Printf(TEXT("Torso average move after arm loss: %.1f cm"), TorsoAverageMove));
	// 몸통은 60 → 54개로 다시 샘플링되어 점 배치가 통째로 바뀐다. 배열 순서로 배정하면 평균 이동이 몸통 크기 수준이지만,
	// 가까운 목표끼리 짝지으면 구체 간격(22cm) 근처에 머문다. 여유를 두고 간격의 2배를 상한으로 본다.
	TestTrue(TEXT("Nearest assignment keeps torso moves short"), TorsoAverageMove < 44.0);

	Boss->Tick(Duration);
	TestFalse(TEXT("Movement finishes and Tick turns off"), Boss->IsActorTickEnabled());

	// 피해 구간 없이 지우면 다음 Tick에 바로 재분배한다.
	Boss->RemoveInstance(Torso, 0);
	Boss->Tick(0.0f);
	TestFalse(TEXT("Loss without a window rebalances on the next Tick"), Boss->IsRebalancePending());
	TestEqual(TEXT("Remainder stays on torso"), Torso->GetInstanceCount(), 54);
	TestEqual(TEXT("Arm gives one back"), Arm->GetInstanceCount(), 26);
	Boss->Tick(Duration);

	// 자동 재분배를 끄면 손실만 반영한다.
	Boss->SetAutoRebalanceOnLoss(false);
	Boss->RemoveInstance(Arm, 0);
	TestFalse(TEXT("Disabled auto rebalance does not mark pending"), Boss->IsRebalancePending());
	Boss->Tick(Duration);
	TestEqual(TEXT("Disabled auto rebalance keeps torso"), Torso->GetInstanceCount(), 54);
	TestEqual(TEXT("Disabled auto rebalance keeps arm"), Arm->GetInstanceCount(), 25);

	// 들어온 구체의 출발점은 현재 위치를 받는 부위 도형 안으로 붙인 점이다. 박스는 축마다, 구는 반지름으로 자르고 안은 그대로.
	const FVector Box(50.0, 30.0, 80.0);
	TestTrue(TEXT("Inside point is kept"), FSwarmShapeSampler::ClampInside(FVector(10.0, -5.0, 20.0),
		ESwarmPrimitive::Box, Box).Equals(FVector(10.0, -5.0, 20.0)));
	TestTrue(TEXT("Box clamps each axis"), FSwarmShapeSampler::ClampInside(FVector(120.0, -5.0, -200.0),
		ESwarmPrimitive::Box, Box).Equals(FVector(50.0, -5.0, -80.0)));
	TestTrue(TEXT("Sphere clamps to its radius"), FSwarmShapeSampler::ClampInside(FVector(0.0, 300.0, 400.0),
		ESwarmPrimitive::Sphere, FVector(50.0)).Equals(FVector(0.0, 30.0, 40.0), 1.e-6));

	World->DestroyWorld(false);
	return true;
}

namespace
{
	// 몸통 + 오른팔 + 오른손 형태 데이터. 오른팔은 몸통 오른쪽(+Y) 면 위쪽에 안쪽 면 위 모서리(0, -1, 1)로 붙어 아래로 늘어진다.
	// 오른손은 팔 아랫면에 윗면으로 붙고, HandRotation만큼 기본 회전이 있다.
	USwarmFormDefinition* CreateArmPoseDefinition(const FRotator& HandRotation)
	{
		USwarmFormDefinition* Definition = NewObject<USwarmFormDefinition>(GetTransientPackage());

		FSwarmPartDefinition& Torso = Definition->Parts.AddDefaulted_GetRef();
		Torso.PartId = ESwarmPartId::Torso;
		Torso.ParentPartId = ESwarmPartId::None;
		Torso.Shape = ESwarmPrimitive::Box;
		Torso.BoxHalfExtentsRatio = FVector(1.0, 0.75, 1.5);
		Torso.AllocationWeight = 4;

		FSwarmPartDefinition& Arm = Definition->Parts.AddDefaulted_GetRef();
		Arm.PartId = ESwarmPartId::RightArm;
		Arm.ParentPartId = ESwarmPartId::Torso;
		Arm.Shape = ESwarmPrimitive::Box;
		Arm.BoxHalfExtentsRatio = FVector(0.45, 0.45, 1.0);
		Arm.AllocationWeight = 2;
		Arm.ParentAnchor = FVector(0.0, 1.0, 1.0);
		Arm.ChildAnchor = FVector(0.0, -1.0, 1.0);
		Arm.Gap = 10.0f;

		FSwarmPartDefinition& Hand = Definition->Parts.AddDefaulted_GetRef();
		Hand.PartId = ESwarmPartId::RightHand;
		Hand.ParentPartId = ESwarmPartId::RightArm;
		Hand.Shape = ESwarmPrimitive::Box;
		Hand.BoxHalfExtentsRatio = FVector(0.6, 0.6, 1.0);
		Hand.AllocationWeight = 1;
		Hand.ParentAnchor = FVector(0.0, 0.0, -1.0);
		Hand.ChildAnchor = FVector(0.0, 0.0, 1.0);
		Hand.Gap = 5.0f;
		Hand.RelativeRotation = HandRotation;

		return Definition;
	}

	// 부위 ISM 기준 관절 피벗 점(박스): (0, 0, ChildAnchor.Z) × 반크기. 월드로 바꾸면 회전 전후로 같아야 한다.
	FVector PivotWorld(const ASwarmBossManager& Boss, ESwarmPartId PartId, const FSwarmPartDefinition& Definition,
		const FVector& HalfExtents)
	{
		return Boss.GetPartInstances(PartId)->GetComponentTransform().TransformPosition(
			FVector(0.0, 0.0, Definition.ChildAnchor.Z) * HalfExtents);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSwarmJointPoseTest, "SwarmBoss.Stage3.JointPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 관절 피벗과 부위 자세를 검증한다. 피벗 고정, 회전 방향(키 1·3), 축 혼합 쿼터니언 합성, 손자 부위·구체 연동,
// 기본 회전이 있는 부위, 회전된 Manager, 실제 재분배 뒤 자세 유지와 이동 완료, 자세와 무관한 바닥 높이,
// 이동 중 회전, 잘못된 입력 거부, 재생성 시 초기화.
bool FSwarmJointPoseTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);

	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}

	ASwarmBossManager* Boss = World->SpawnActor<ASwarmBossManager>();

	if (!TestNotNull(TEXT("Manager"), Boss))
	{
		World->DestroyWorld(false);
		return false;
	}

	Boss->SetFormDefinition(CreateArmPoseDefinition(FRotator(0.0, 30.0, 0.0)));
	Boss->SetAutoRebalanceOnLoss(false);
	Boss->InitialParticleCount = 140;

	if (!TestTrue(TEXT("Generate"), Boss->GenerateInitialParts()))
	{
		World->DestroyWorld(false);
		return false;
	}

	const float Duration = Boss->ReconstructionDuration;
	UInstancedStaticMeshComponent* Torso = Boss->GetPartInstances(ESwarmPartId::Torso);
	UInstancedStaticMeshComponent* Arm = Boss->GetPartInstances(ESwarmPartId::RightArm);
	UInstancedStaticMeshComponent* Hand = Boss->GetPartInstances(ESwarmPartId::RightHand);
	const FSwarmPart& ArmPart = Boss->Parts.FindChecked(ESwarmPartId::RightArm);
	const FSwarmPart& HandPart = Boss->Parts.FindChecked(ESwarmPartId::RightHand);
	const double Tolerance = 1.e-3;

	TestTrue(TEXT("Initial pose is identity"), Boss->GetPartPose(ESwarmPartId::RightArm).Equals(FQuat::Identity));

	// 1) 키 1(앞 축 +15도)은 팔 끝을 바깥(+Y)·위로 보낸다. 피벗은 그대로이고 손·구체는 피벗을 중심으로 같이 돈다.
	const FVector ArmPivot = PivotWorld(*Boss, ESwarmPartId::RightArm, ArmPart.Definition, ArmPart.HalfExtents);
	const FVector ArmCenterBefore = Arm->GetComponentLocation();
	const FVector HandCenterBefore = Hand->GetComponentLocation();
	const FTransform ArmTransformBefore = Arm->GetComponentTransform();
	const FVector ParticleLocal = ArmPart.InstanceToParticle.Num() > 0
		? Boss->GetParticles()[ArmPart.InstanceToParticle[0]].LocalPosition : FVector::ZeroVector;
	const FVector ParticleBefore = ArmTransformBefore.TransformPosition(ParticleLocal);
	const double TorsoZBefore = Torso->GetRelativeLocation().Z;
	const FQuat SideDelta(FVector::ForwardVector, FMath::DegreesToRadians(15.0));
	const FQuat ForwardDelta(FVector::RightVector, FMath::DegreesToRadians(-15.0));

	TestTrue(TEXT("Side delta applies"), Boss->AddPartPoseDelta(ESwarmPartId::RightArm, SideDelta));
	TestTrue(TEXT("Shoulder pivot stays fixed"),
		PivotWorld(*Boss, ESwarmPartId::RightArm, ArmPart.Definition, ArmPart.HalfExtents).Equals(ArmPivot, Tolerance));
	TestTrue(TEXT("Key 1 moves the arm outward (+Y)"), Arm->GetComponentLocation().Y > ArmCenterBefore.Y + 1.0);
	TestTrue(TEXT("Key 1 raises the arm"), Arm->GetComponentLocation().Z > ArmCenterBefore.Z + 1.0);

	// 부모(몸통) 축 기준 회전이므로 월드 회전은 몸통 회전으로 감싼 증분이다.
	const FQuat TorsoRotation = Torso->GetComponentQuat();
	const FQuat WorldDelta = TorsoRotation * SideDelta * TorsoRotation.Inverse();
	TestTrue(TEXT("Hand rotates about the shoulder"), Hand->GetComponentLocation().Equals(
		ArmPivot + WorldDelta.RotateVector(HandCenterBefore - ArmPivot), Tolerance));
	TestTrue(TEXT("Particles rotate with the arm"), Arm->GetComponentTransform().TransformPosition(ParticleLocal).Equals(
		ArmPivot + WorldDelta.RotateVector(ParticleBefore - ArmPivot), Tolerance));
	TestTrue(TEXT("Arm rotation is pose times rest rotation"), Arm->GetRelativeRotation().Quaternion().Equals(
		Boss->GetPartPose(ESwarmPartId::RightArm) * ArmPart.Definition.RelativeRotation.Quaternion(), 1.e-4));

	// 2) 키 3(오른쪽 축 -15도)은 기본 자세에서 팔 끝을 앞(+X)으로 보낸다.
	TestTrue(TEXT("Reset to rest"), Boss->SetPartPose(ESwarmPartId::RightArm, FRotator::ZeroRotator));
	TestTrue(TEXT("Rest pose restores the arm"), Arm->GetComponentLocation().Equals(ArmCenterBefore, Tolerance));
	TestTrue(TEXT("Forward delta applies"), Boss->AddPartPoseDelta(ESwarmPartId::RightArm, ForwardDelta));
	TestTrue(TEXT("Key 3 moves the arm forward (+X)"), Arm->GetComponentLocation().X > ArmCenterBefore.X + 1.0);

	// 3) 축 혼합 1 -> 3 -> 1은 부모 축 기준 쿼터니언 합성(뒤 증분을 앞에서 곱함)과 같아야 한다.
	Boss->SetPartPose(ESwarmPartId::RightArm, FRotator::ZeroRotator);
	Boss->AddPartPoseDelta(ESwarmPartId::RightArm, SideDelta);
	Boss->AddPartPoseDelta(ESwarmPartId::RightArm, ForwardDelta);
	Boss->AddPartPoseDelta(ESwarmPartId::RightArm, SideDelta);
	const FQuat Mixed = (SideDelta * ForwardDelta * SideDelta).GetNormalized();
	TestTrue(TEXT("Mixed axes compose as quaternions"), Boss->GetPartPose(ESwarmPartId::RightArm).Equals(Mixed, 1.e-5));
	TestTrue(TEXT("Shoulder pivot stays fixed after mixed input"),
		PivotWorld(*Boss, ESwarmPartId::RightArm, ArmPart.Definition, ArmPart.HalfExtents).Equals(ArmPivot, Tolerance));

	// 4) 기본 회전(Yaw 30도)이 있는 손도 손목 피벗을 중심으로 돈다.
	const FVector WristPivot = PivotWorld(*Boss, ESwarmPartId::RightHand, HandPart.Definition, HandPart.HalfExtents);
	TestTrue(TEXT("Hand with rest rotation accepts a pose"),
		Boss->SetPartPose(ESwarmPartId::RightHand, FRotator(20.0, 0.0, 10.0)));
	TestTrue(TEXT("Wrist pivot stays fixed with rest rotation"),
		PivotWorld(*Boss, ESwarmPartId::RightHand, HandPart.Definition, HandPart.HalfExtents).Equals(WristPivot, Tolerance));
	TestTrue(TEXT("Hand rotation is pose times rest rotation"), Hand->GetRelativeRotation().Quaternion().Equals(
		Boss->GetPartPose(ESwarmPartId::RightHand) * HandPart.Definition.RelativeRotation.Quaternion(), 1.e-4));

	// 5) 같은 도형 크기에서 자세만 바뀌면 몸통 높이(바닥 맞춤)는 그대로다.
	TestEqual(TEXT("Posing does not change torso height"), Torso->GetRelativeLocation().Z, TorsoZBefore, Tolerance);
	Boss->ApplyPartPlacement(/*bPreserveParticleWorldPositions=*/true);
	TestEqual(TEXT("Re-placement with a pose keeps the rest-pose floor"), Torso->GetRelativeLocation().Z, TorsoZBefore, Tolerance);
	Boss->Tick(Duration);

	// 6) 잘못된 입력은 거부하고 기존 자세를 유지한다.
	const FQuat PoseBeforeInvalid = Boss->GetPartPose(ESwarmPartId::RightArm);
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("Root pose is rejected"), Boss->SetPartPose(ESwarmPartId::Torso, FRotator(10.0, 0.0, 0.0)));
	TestFalse(TEXT("Missing part is rejected"), Boss->SetPartPose(ESwarmPartId::LeftLeg, FRotator(10.0, 0.0, 0.0)));
	TestFalse(TEXT("NaN rotation is rejected"), Boss->SetPartPose(ESwarmPartId::RightArm, FRotator(NaN, 0.0, 0.0)));
	TestFalse(TEXT("Infinite rotation is rejected"), Boss->SetPartPose(ESwarmPartId::RightArm,
		FRotator(std::numeric_limits<double>::infinity(), 0.0, 0.0)));
	TestFalse(TEXT("NaN delta is rejected"), Boss->AddPartPoseDelta(ESwarmPartId::RightArm, FQuat(NaN, 0.0, 0.0, 1.0)));
	TestTrue(TEXT("Rejected input keeps the pose"), Boss->GetPartPose(ESwarmPartId::RightArm).Equals(PoseBeforeInvalid));

	// 7) 실제 재분배: 팔 구체를 지워 개수를 어긋나게 만든 뒤 재분배한다. 자세는 유지되고 관절은 새 크기로 다시 계산되며,
	//    구체는 이전 월드 위치에서 출발해 이동이 끝나면 새 로컬 목표에 도착한다. 이동 도중 자세를 바꿔도 마찬가지다.
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Boss->RemoveInstance(Arm, 0);
	}

	const int32 ArmCountBeforeRebalance = Arm->GetInstanceCount();
	TestTrue(TEXT("Rebalance after arm loss"), Boss->RebalancePartsFromDefinition());
	TestNotEqual(TEXT("Rebalance really changed the arm count"), Arm->GetInstanceCount(), ArmCountBeforeRebalance);
	TestTrue(TEXT("Pose is kept through a real rebalance"),
		Boss->GetPartPose(ESwarmPartId::RightArm).Equals(PoseBeforeInvalid, 1.e-5));
	TestTrue(TEXT("Arm rotation after rebalance is pose times rest"), Arm->GetRelativeRotation().Quaternion().Equals(
		Boss->GetPartPose(ESwarmPartId::RightArm) * ArmPart.Definition.RelativeRotation.Quaternion(), 1.e-4));

	Boss->Tick(Duration * 0.5f);
	TestTrue(TEXT("Pose change during movement"), Boss->AddPartPoseDelta(ESwarmPartId::RightArm, ForwardDelta));
	Boss->Tick(Duration);
	Boss->Tick(Duration);

	bool bAllArrived = true;

	for (const FSwarmParticle& Particle : Boss->GetParticles())
	{
		bAllArrived &= !Particle.bActive || Particle.LocalPosition.Equals(Particle.TargetLocalPosition);
	}

	TestTrue(TEXT("All particles reach their local targets under the posed transform"), bAllArrived);

	// 8) 회전된 Manager에서도 키 1은 월드 Y가 아니라 몸통 로컬 오른쪽(+Y)으로 팔을 보낸다.
	ASwarmBossManager* Turned = World->SpawnActor<ASwarmBossManager>(FVector(5000.0, 0.0, 0.0), FRotator(0.0, 90.0, 0.0));

	if (TestNotNull(TEXT("Turned manager"), Turned))
	{
		Turned->SetFormDefinition(CreateArmPoseDefinition(FRotator::ZeroRotator));
		Turned->SetAutoRebalanceOnLoss(false);

		if (TestTrue(TEXT("Turned generate"), Turned->GenerateInitialParts()))
		{
			UInstancedStaticMeshComponent* TurnedArm = Turned->GetPartInstances(ESwarmPartId::RightArm);
			const FVector TorsoRight = Turned->GetPartInstances(ESwarmPartId::Torso)->GetRightVector();
			const FVector Before = TurnedArm->GetComponentLocation();
			Turned->AddPartPoseDelta(ESwarmPartId::RightArm, SideDelta);
			const FVector Moved = TurnedArm->GetComponentLocation() - Before;
			TestTrue(TEXT("Turned key 1 follows the torso right axis"), FVector::DotProduct(Moved, TorsoRight) > 1.0);
			TestTrue(TEXT("Turned key 1 is not along world Y"), FMath::Abs(Moved.Y) < FMath::Abs(FVector::DotProduct(Moved, TorsoRight)));
		}
	}

	// 9) 전체 재생성은 자세를 초기화한다.
	TestTrue(TEXT("Regenerate"), Boss->GenerateInitialParts());
	TestTrue(TEXT("Regenerate resets the arm pose"), Boss->GetPartPose(ESwarmPartId::RightArm).Equals(FQuat::Identity));
	TestTrue(TEXT("Regenerate resets the hand pose"), Boss->GetPartPose(ESwarmPartId::RightHand).Equals(FQuat::Identity));
	// 재생성은 부위 맵을 다시 맞추므로 앞에서 잡은 참조 대신 다시 찾는다.
	const FSwarmPart& RegeneratedArm = Boss->Parts.FindChecked(ESwarmPartId::RightArm);
	TestTrue(TEXT("Regenerated arm uses its rest rotation"),
		RegeneratedArm.Instances->GetRelativeRotation().Quaternion().Equals(
			RegeneratedArm.Definition.RelativeRotation.Quaternion(), 1.e-4));

	World->DestroyWorld(false);
	return true;
}

#endif
