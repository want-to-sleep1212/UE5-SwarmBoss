// Copyright Epic Games, Inc. All Rights Reserved.


#include "SwarmBossPlayerController.h"

#include "Components/InputComponent.h"
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
