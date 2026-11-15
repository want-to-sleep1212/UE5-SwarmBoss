#pragma once

#include "CoreMinimal.h"

// 목표 배정 설정. 길이는 cm.
struct FSwarmAssignSettings
{
	// 구체마다 살펴볼 가까운 목표 후보 수 k. 후보를 모두 다른 구체에 뺏기면 남은 빈 목표 중 가장 가까운 것을 받는다.
	int32 CandidateCount = 12;

	// 목표를 넣을 격자 칸 크기. 구체 중심 최소 간격 d(= 2 × 반지름 + 여유)를 쓴다.
	double CellSize = 22.0;

	// 이 거리보다 멀리 가는 배정을 "먼 이동"으로 센다(통계용).
	double LongMoveThreshold = 44.0;
};

// 배정 결과 통계. 거리는 cm.
struct FSwarmAssignStats
{
	double MaxMove = 0.0;
	double AverageMove = 0.0;

	// LongMoveThreshold보다 멀리 가는 배정 수.
	int32 LongMoves = 0;

	// 후보 k개를 모두 뺏겨 남은 빈 목표 중에서 받은 구체 수.
	int32 Leftovers = 0;

	double Milliseconds = 0.0;
};

/**
 * 구체 시작 위치와 같은 수의 목표 위치를 1:1로 짝짓는다. 엔진 상태와 무관한 순수 계산이다.
 * 가까운 후보 쌍부터 확정하는 그리디 방식이라 대부분 바로 옆 목표로 가지만, 마지막에 남은 몇 개는 멀리 갈 수 있다.
 */
struct SWARMBOSS_API FSwarmTargetAssigner
{
	// Starts[i]에 배정한 목표 번호를 OutTargetIndices[i]에 담는다. 모든 목표는 정확히 한 번 쓰인다.
	// 같은 입력이면 항상 같은 결과를 낸다. 개수가 다르거나 설정이 잘못되면 빈 배열과 false를 반환한다.
	static bool Assign(TConstArrayView<FVector> Starts, TConstArrayView<FVector> Targets,
		const FSwarmAssignSettings& Settings, TArray<int32>& OutTargetIndices, FSwarmAssignStats* OutStats = nullptr);
};
