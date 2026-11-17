// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Swarm/SwarmPartTypes.h"
#include "SwarmBossPlayerController.generated.h"

class UInputMappingContext;
class UUserWidget;

/**
 *  Simple first person Player Controller
 *  Manages the input mapping context.
 *  Overrides the Player Camera Manager class.
 */
UCLASS(abstract, config="Game")
class SWARMBOSS_API ASwarmBossPlayerController : public APlayerController
{
	GENERATED_BODY()
	
public:

	/** Constructor */
	ASwarmBossPlayerController();

protected:

	/** Input Mapping Contexts */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	/** Input Mapping Contexts */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<UInputMappingContext*> MobileExcludedMappingContexts;

	/** Mobile controls widget to spawn */
	UPROPERTY(EditAnywhere, Category="Input|Touch Controls")
	TSubclassOf<UUserWidget> MobileControlsWidgetClass;

	/** Pointer to the mobile controls widget */
	UPROPERTY()
	TObjectPtr<UUserWidget> MobileControlsWidget;

	/** If true, the player will use UMG touch controls even if not playing on mobile platforms */
	UPROPERTY(EditAnywhere, Config, Category = "Input|Touch Controls")
	bool bForceTouchControls = false;

	/** Gameplay initialization */
	virtual void BeginPlay() override;

	/** Input mapping context setup */
	virtual void SetupInputComponent() override;

	// 왼쪽 버튼을 누를 때마다 한 발 쏜다(단발). 마지막 발사 후 FireCooldown이 지나지 않았으면 쏘지 않는다.
	void TryFireProjectile();

	// 다음 발사까지 기다려야 하는 시간(초).
	UPROPERTY(EditAnywhere, Category="Swarm|Attack", meta=(ClampMin="0", Units="s"))
	float FireCooldown = 2.0f;

	// 투사체 속도(cm/s).
	UPROPERTY(EditAnywhere, Category="Swarm|Attack", meta=(ClampMin="100", Units="cm/s"))
	float ProjectileSpeed = 1500.0f;

	// 관통 구체 투사체의 반지름(cm). 겉모습과 충돌 크기가 같으며, 지나간 자리에 지름 2 × (반지름 + 구체 반지름)의 터널이 생긴다.
	UPROPERTY(EditAnywhere, Category="Swarm|Attack", meta=(ClampMin="1", Units="cm"))
	float ProjectileRadius = 15.0f;

	// 카메라에서 이만큼 앞에서 투사체를 생성한다(cm).
	UPROPERTY(EditAnywhere, Category="Swarm|Attack", meta=(ClampMin="0", Units="cm"))
	float MuzzleOffset = 50.0f;

	// 마지막 발사 시각(월드 시간, 초). 아직 쏘지 않았으면 음수다.
	double LastFireTime = -1.0;

	// T 키로 레벨의 Manager마다 모든 부위 생존 수를 형태 데이터의 가중치 비율로 맞추고 새 도형 목표로 이동시킨다.
	void RebalanceSwarmPartsForTest();

	// 부위 자세 확인용 입력(1~4). 오른팔을 어깨 관절 기준으로 PoseStepDegrees씩 돌린다.
	// 1: 옆으로 들기, 2: 옆으로 내리기(몸통 부모의 앞 방향 X축 기준), 3: 앞으로 들기, 4: 뒤로 들기(오른쪽 방향 Y축 기준).
	void RaiseRightArmSideForTest();
	void LowerRightArmSideForTest();
	void RaiseRightArmForwardForTest();
	void RaiseRightArmBackwardForTest();

	// 레벨의 Manager마다 PartId 부위 자세에 부모 부위 로컬 Axis 기준 Degrees만큼 회전 증분을 더한다.
	void AdjustSwarmPartPoseForTest(ESwarmPartId PartId, const FVector& Axis, float Degrees);

	// 자세 확인 키 한 번에 돌리는 각도(도).
	UPROPERTY(EditAnywhere, Category="Swarm|Pose", meta=(ClampMin="1", ClampMax="90", Units="deg"))
	float PoseStepDegrees = 15.0f;

	/** Returns true if the player should use UMG touch controls */
	bool ShouldUseTouchControls() const;
};
