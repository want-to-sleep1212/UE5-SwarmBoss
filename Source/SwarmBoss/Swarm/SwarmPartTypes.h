#pragma once

#include "CoreMinimal.h"
#include "SwarmPartTypes.generated.h"

// 거인 부위 식별자. 기존 Preview/Torso/LeftArm의 숫자 값은 저장된 데이터와의 호환성을 위해 유지한다.
UENUM(BlueprintType)
enum class ESwarmPartId : uint8
{
	Preview = 0 UMETA(Hidden),
	Torso = 1,
	LeftArm = 2,
	Head,
	RightArm,
	LeftHand,
	RightHand,
	LeftLeg,
	RightLeg,
	LeftFoot,
	RightFoot,

	// 부모가 없는 최상위 부위 또는 아직 지정하지 않은 부위를 나타낸다. 실제 부위로 등록하지 않는다.
	None
};
