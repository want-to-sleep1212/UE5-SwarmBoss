#pragma once

#include "CoreMinimal.h"
#include "Swarm/SwarmPartTypes.h"

// 같은 출발 부위에서 같은 경로로 이동할 구체 묶음. Path[0]이 출발 부위, Path.Last()가 최종 소속 부위다.
// 경로 길이가 2이면 관절 하나만 건너고, 3 이상이면 구체가 모자란 길목 부위를 건너뛰어 그다음 부위로 바로 간다.
struct FSwarmTransferShipment
{
	TArray<ESwarmPartId> Path;
	int32 Count = 0;
};

/** 부위 트리에서 이웃 부위끼리만 구체를 넘기는 재분배 전달 계획을 만든다. 구체 선택·이동은 하지 않는다. */
struct SWARMBOSS_API FSwarmTransferPlanner
{
	// Parents: 부위 → 부모 부위(최상위는 None). CurrentCounts·TargetCounts: 부위별 현재·목표 구체 수.
	// 관절마다 자식 쪽 전체(자식과 그 아래 부위들)의 (현재 - 목표) 합만큼 구체가 지나간다. 양수면 자식 → 부모, 음수면 부모 → 자식.
	// 모든 관절의 전달은 동시에 시작한다고 보고, 부위는 자기 구체로 먼저 내보낸다. 내보낼 수가 가진 수보다 많으면
	// 그 부위로 들어오는 묶음 중 모자란 수만큼의 경로를 다음 부위까지 연장한다(길목 건너뛰기).
	// 입력이 트리가 아니거나 개수 합이 다르면 빈 배열과 false를 반환한다. 바꿀 것이 없으면 빈 배열과 true를 반환한다.
	static bool Plan(const TMap<ESwarmPartId, ESwarmPartId>& Parents, const TMap<ESwarmPartId, int32>& CurrentCounts,
		const TMap<ESwarmPartId, int32>& TargetCounts, TArray<FSwarmTransferShipment>& OutShipments);
};
