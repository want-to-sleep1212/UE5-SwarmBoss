// Copyright Epic Games, Inc. All Rights Reserved.


#include "SwarmBossPlayerController.h"

#include "Animation/AnimSequence.h"
#include "Components/InputComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "SwarmCrosshairHUD.h"
#include "InputCoreTypes.h"
#include "Boss/SwarmBossManager.h"
#include "Projectiles/SwarmProjectile.h"

#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "SwarmBossCameraManager.h"
#include "Blueprint/UserWidget.h"
#include "SwarmBoss.h"
#include "Widgets/Input/SVirtualJoystick.h"

// 플레이어가 사용할 카메라 매니저 클래스를 지정한다.
ASwarmBossPlayerController::ASwarmBossPlayerController()
{
	// set the player camera manager class
	PlayerCameraManagerClass = ASwarmBossCameraManager::StaticClass();

	// 5 키 순환에 쓰는 마네킹 애니메이션(Git LFS로 추적, Docs/DemoAssets.md).
	static ConstructorHelpers::FObjectFinder<UAnimSequence> Idle(TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> Walk(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Walk/MF_Unarmed_Walk_Fwd.MF_Unarmed_Walk_Fwd"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> Attack(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01.MM_Attack_01"));

	IdleAnimation = Idle.Succeeded() ? Idle.Object : nullptr;
	WalkAnimation = Walk.Succeeded() ? Walk.Object : nullptr;
	AttackAnimation = Attack.Succeeded() ? Attack.Object : nullptr;
}

// 로컬 플레이어의 조준선을 설정하고, 터치 입력을 쓰는 환경에서는 터치 위젯을 표시한다.
void ASwarmBossPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalPlayerController())
	{
		ClientSetHUD(ASwarmCrosshairHUD::StaticClass());
	}


	
	// only spawn touch controls on local player controllers
	if (ShouldUseTouchControls() && IsLocalPlayerController())
	{
		// spawn the mobile controls widget
		MobileControlsWidget = CreateWidget<UUserWidget>(this, MobileControlsWidgetClass);

		if (MobileControlsWidget)
		{
			// add the controls to the player screen
			MobileControlsWidget->AddToPlayerScreen(0);

		} else {

			UE_LOG(LogSwarmBoss, Error, TEXT("Could not spawn mobile controls widget."));

		}

	}
}

// 로컬 플레이어의 투사체 발사(왼쪽 버튼 단발)와 T 키 재분배 테스트 입력, 입력 매핑을 등록한다.
void ASwarmBossPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// 현재 단계의 디버그 입력. 실제 발사 Input Action은 이후 별도로 연결한다.
	if (IsLocalPlayerController() && InputComponent)
	{
		InputComponent->BindKey(
			EKeys::LeftMouseButton,
			IE_Pressed,
			this,
			&ASwarmBossPlayerController::TryFireProjectile);

		InputComponent->BindKey(
			EKeys::T,
			IE_Pressed,
			this,
			&ASwarmBossPlayerController::RebalanceSwarmPartsForTest);

		// 부위 자세 확인용: 오른팔을 어깨 관절 기준으로 옆(1·2), 앞뒤(3·4)로 돌린다.
		InputComponent->BindKey(EKeys::One, IE_Pressed, this, &ASwarmBossPlayerController::RaiseRightArmSideForTest);
		InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &ASwarmBossPlayerController::LowerRightArmSideForTest);
		InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &ASwarmBossPlayerController::RaiseRightArmForwardForTest);
		InputComponent->BindKey(EKeys::Four, IE_Pressed, this, &ASwarmBossPlayerController::RaiseRightArmBackwardForTest);

		// 애니메이션 자세 전달 확인용: 끔 → Idle → 걷기 → 공격 → 끔 순환.
		InputComponent->BindKey(EKeys::Five, IE_Pressed, this, &ASwarmBossPlayerController::CyclePoseAnimationForTest);

	}


	// only add IMCs for local player controllers
	if (IsLocalPlayerController())
	{
		// Add Input Mapping Context
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}

			// only add these IMCs if we're not using mobile touch input
			if (!ShouldUseTouchControls())
			{
				for (UInputMappingContext* CurrentContext : MobileExcludedMappingContexts)
				{
					Subsystem->AddMappingContext(CurrentContext, 0);
				}
			}
		}
	}
	
}

// 가상 조이스틱 표시 조건이나 강제 설정에 따라 터치 조작을 사용할지 판정한다.
bool ASwarmBossPlayerController::ShouldUseTouchControls() const
{
	// are we on a mobile platform? Should we force touch?
	return SVirtualJoystick::ShouldDisplayTouchInterface() || bForceTouchControls;
}

// 쿨타임이 지났으면 플레이어 카메라 위치에서 MuzzleOffset만큼 앞에 관통 구체 투사체를 만들고 카메라 정면으로 발사한다.
// 쿨타임 중이면 아무것도 하지 않는다. 피격 판정과 구체 제거는 투사체(ASwarmProjectile)가 맡는다.
void ASwarmBossPlayerController::TryFireProjectile()
{
	UWorld* World = GetWorld();

	if (!World || !IsLocalPlayerController())
	{
		return;
	}

	const double Now = World->GetTimeSeconds();

	if (LastFireTime >= 0.0 && Now - LastFireTime < FireCooldown)
	{
		return;
	}

	FVector ViewLocation;
	FRotator ViewRotation;
	GetPlayerViewPoint(ViewLocation, ViewRotation);

	const FVector Direction = ViewRotation.Vector();
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = this;
	SpawnParameters.Instigator = GetPawn();
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ASwarmProjectile* Projectile = World->SpawnActor<ASwarmProjectile>(
		ViewLocation + Direction * MuzzleOffset, ViewRotation, SpawnParameters);

	if (Projectile)
	{
		Projectile->Launch(Direction, ProjectileSpeed, ProjectileRadius);
		LastFireTime = Now;
	}
}

// 회전 증분은 축·각으로 만든 쿼터니언이다. 오른팔은 몸통의 +Y 쪽에 붙어 아래(-Z)로 늘어져 있으므로,
// FQuat(축, 각)의 회전식(y' = y·cos - z·sin 등)에 팔 끝 방향 (0, 0, -1)을 넣으면
// +X(앞) 축 기준 양의 각은 팔 끝을 +Y(바깥)로, +Y(오른쪽) 축 기준 음의 각은 팔 끝을 +X(앞)로 보낸다.
// 부호는 JointPose 테스트에서 기본 자세 기준으로 확인한다.
void ASwarmBossPlayerController::RaiseRightArmSideForTest()
{
	AdjustSwarmPartPoseForTest(ESwarmPartId::RightArm, FVector::ForwardVector, PoseStepDegrees);
}

void ASwarmBossPlayerController::LowerRightArmSideForTest()
{
	AdjustSwarmPartPoseForTest(ESwarmPartId::RightArm, FVector::ForwardVector, -PoseStepDegrees);
}

void ASwarmBossPlayerController::RaiseRightArmForwardForTest()
{
	AdjustSwarmPartPoseForTest(ESwarmPartId::RightArm, FVector::RightVector, -PoseStepDegrees);
}

void ASwarmBossPlayerController::RaiseRightArmBackwardForTest()
{
	AdjustSwarmPartPoseForTest(ESwarmPartId::RightArm, FVector::RightVector, PoseStepDegrees);
}

// 축은 부모 부위(오른팔이면 몸통) 로컬 기준이다. Manager가 저장된 쿼터니언에 증분을 앞에서 곱한다.
void ASwarmBossPlayerController::AdjustSwarmPartPoseForTest(ESwarmPartId PartId, const FVector& Axis, float Degrees)
{
	UWorld* World = GetWorld();

	if (!World || !IsLocalPlayerController())
	{
		return;
	}

	const FQuat Delta(Axis.GetSafeNormal(), FMath::DegreesToRadians(Degrees));

	for (TActorIterator<ASwarmBossManager> Boss(World); Boss; ++Boss)
	{
		// 애니메이션 자세 전달 중에는 매 프레임 절대 자세로 덮어써지므로 수동 자세를 받지 않는다.
		if (Boss->IsPoseAnimationActive())
		{
			UE_LOG(LogSwarmBoss, Display, TEXT("Manual pose ignored on %s while a pose animation is playing."), *Boss->GetName());
			continue;
		}

		if (!Boss->AddPartPoseDelta(PartId, Delta))
		{
			UE_LOG(LogSwarmBoss, Warning, TEXT("Pose change failed for %s on %s."), *UEnum::GetValueAsString(PartId),
				*Boss->GetName());
		}
	}
}

// 상태를 한 칸 넘기고 레벨의 Manager마다 같은 애니메이션을 재생하거나 멈춘다. 재생에 실패하면 경고를 남긴다.
void ASwarmBossPlayerController::CyclePoseAnimationForTest()
{
	UWorld* World = GetWorld();

	if (!World || !IsLocalPlayerController())
	{
		return;
	}

	PoseAnimationState = (PoseAnimationState + 1) % 4;

	for (TActorIterator<ASwarmBossManager> Boss(World); Boss; ++Boss)
	{
		bool bOk = true;

		switch (PoseAnimationState)
		{
		case 1:
			bOk = Boss->PlayPoseAnimation(IdleAnimation, /*bLoop=*/true);
			break;
		case 2:
			bOk = Boss->PlayPoseAnimation(WalkAnimation, /*bLoop=*/true);
			break;
		case 3:
			bOk = Boss->PlayPoseAnimation(AttackAnimation, /*bLoop=*/false);
			break;
		default:
			Boss->StopPoseAnimation();
			break;
		}

		if (!bOk)
		{
			UE_LOG(LogSwarmBoss, Warning, TEXT("Pose animation state %d failed on %s."), PoseAnimationState, *Boss->GetName());
		}
	}
}

// T 키로 각 Manager에 형태 데이터의 부위 가중치에 맞춘 현재 생존 수 재분배를 요청한다.
// 가중치를 읽고, 초과 부위에서 부족 부위로 필요한 만큼 전달하고 목표를 배정하는 결정은 Manager가 담당한다.
void ASwarmBossPlayerController::RebalanceSwarmPartsForTest()
{
	UWorld* World = GetWorld();

	if (!World || !IsLocalPlayerController())
	{
		return;
	}

	for (TActorIterator<ASwarmBossManager> Boss(World); Boss; ++Boss)
	{
		if (!Boss->RebalancePartsFromDefinition())
		{
			UE_LOG(LogSwarmBoss, Warning, TEXT("Rebalance failed for %s."), *Boss->GetName());
		}
	}
}
