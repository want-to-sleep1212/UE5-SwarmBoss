#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "SwarmCrosshairHUD.generated.h"

/** 화면 중앙에 조준용 십자만 표시하는 HUD. */
UCLASS()
class SWARMBOSS_API ASwarmCrosshairHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
