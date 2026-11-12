// Copyright Epic Games, Inc. All Rights Reserved.


#include "SwarmBossPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "SwarmCrosshairHUD.h"
#include "InputCoreTypes.h"
#include "Swarm/SwarmShapePreview.h"

#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "SwarmBossCameraManager.h"
#include "Blueprint/UserWidget.h"
#include "SwarmBoss.h"
#include "Widgets/Input/SVirtualJoystick.h"

ASwarmBossPlayerController::ASwarmBossPlayerController()
{
	// set the player camera manager class
	PlayerCameraManagerClass = ASwarmBossCameraManager::StaticClass();
}

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
			&ASwarmBossPlayerController::TraceSwarmParticle);

		InputComponent->BindKey(
			EKeys::T,
			IE_Pressed,
			this,
			&ASwarmBossPlayerController::MoveSwarmParticlesForPreview);

		InputComponent->BindKey(
			EKeys::R,
			IE_Pressed,
			this,
			&ASwarmBossPlayerController::ReconstructSwarmForPreview);
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

bool ASwarmBossPlayerController::ShouldUseTouchControls() const
{
	// are we on a mobile platform? Should we force touch?
	return SVirtualJoystick::ShouldDisplayTouchInterface() || bForceTouchControls;
}

void ASwarmBossPlayerController::TraceSwarmParticle()
{
	UWorld* World = GetWorld();

	if (!World || !IsLocalPlayerController())
	{
		return;
	}

	// 마우스 커서 위치가 아닌 플레이어 카메라의 정면을 사용한다.
	FVector ViewLocation;
	FRotator ViewRotation;
	GetPlayerViewPoint(ViewLocation, ViewRotation);

	constexpr float TraceDistance = 10000.0f;
	const FVector TraceEnd = ViewLocation + ViewRotation.Vector() * TraceDistance;

	// SwarmSphere는 Trace Channel이 아닌 Object Channel이다.
	// 디버그 식별 검사이므로 벽 등 다른 Object Type은 검사 대상에서 제외한다.
	FCollisionObjectQueryParams ObjectQuery;
	ObjectQuery.AddObjectTypesToQuery(ECC_GameTraceChannel3);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SwarmParticleTrace), false);

	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByObjectType(
		Hit,
		ViewLocation,
		TraceEnd,
		ObjectQuery,
		QueryParams);

	if (!bHit)
	{
		UE_LOG(LogSwarmBoss, Display, TEXT("Swarm trace: no sphere hit."));
		return;
	}

	ASwarmShapePreview* Preview = Cast<ASwarmShapePreview>(Hit.GetActor());

	if (!Preview || !Preview->RemoveInstance(Hit.GetComponent(), Hit.Item))
	{
		UE_LOG(LogSwarmBoss, Warning,
			TEXT("Swarm trace: invalid preview or instance. Actor=%s Instance=%d"),
			*GetNameSafe(Hit.GetActor()),
			Hit.Item);
	}
}

void ASwarmBossPlayerController::MoveSwarmParticlesForPreview()
{
	UWorld* World = GetWorld();

	if (!World)
	{
		return;
	}

	const FVector LocalOffset(1000.0, 0.0, 0.0);

	for (TActorIterator<ASwarmShapePreview> Preview(World); Preview; ++Preview)
	{
		Preview->OffsetActiveParticleTargets(LocalOffset);
	}
}

void ASwarmBossPlayerController::ReconstructSwarmForPreview()
{
	UWorld* World = GetWorld();

	if (!World)
	{
		return;
	}

	for (TActorIterator<ASwarmShapePreview> Preview(World); Preview; ++Preview)
	{
		Preview->ReconstructPreview();
	}
}
