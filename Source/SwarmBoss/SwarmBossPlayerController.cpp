// Copyright Epic Games, Inc. All Rights Reserved.


#include "SwarmBossPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "SwarmCrosshairHUD.h"
#include "InputCoreTypes.h"
#include "Boss/SwarmBossManager.h"

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

// 로컬 플레이어의 클릭 제거와 T 키 부위 전달 테스트 입력, 입력 매핑을 등록한다.
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

// 플레이어 카메라 정면으로 SwarmSphere 충돌 타입을 검사해 처음 맞은 구체를 찾는다.
// 피격 Actor가 ASwarmBossManager이면 메시 컴포넌트와 Hit.Item 인스턴스 번호를 직접 넘겨 구체 제거를 요청한다.
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

	ASwarmBossManager* Boss = Cast<ASwarmBossManager>(Hit.GetActor());

	if (!Boss || !Boss->RemoveInstance(Hit.GetComponent(), Hit.Item))
	{
		UE_LOG(LogSwarmBoss, Warning,
			TEXT("Swarm trace: invalid boss manager or instance. Actor=%s Instance=%d"),
			*GetNameSafe(Hit.GetActor()),
			Hit.Item);
	}
}

// T 키로 각 Manager의 몸통·왼팔을 현재 생존 수 기준 2:1로 재분배하도록 요청한다.
// 초과 부위에서 부족 부위로 필요한 만큼 전달하고 목표를 배정하는 결정은 Manager가 담당한다.
void ASwarmBossPlayerController::RebalanceSwarmPartsForTest()
{
	UWorld* World = GetWorld();

	if (!World || !IsLocalPlayerController())
	{
		return;
	}

	for (TActorIterator<ASwarmBossManager> Boss(World); Boss; ++Boss)
	{
		if (!Boss->RebalanceParts(ESwarmPartId::Torso, ESwarmPartId::LeftArm, 2, 1))
		{
			UE_LOG(LogSwarmBoss, Warning, TEXT("Rebalance failed for %s."), *Boss->GetName());
		}
	}
}
