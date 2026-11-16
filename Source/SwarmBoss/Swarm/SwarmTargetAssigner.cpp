#include "Swarm/SwarmTargetAssigner.h"

#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	// 시작 위치 하나와 후보 목표 하나의 쌍. 비용은 거리 제곱이다(정렬 순서만 쓰므로 루트를 계산하지 않는다).
	struct FAssignPair
	{
		double DistSquared;
		int32 Start;
		int32 Target;
	};

	// 비용이 같으면 시작 번호, 목표 번호 순으로 정해 같은 입력이면 같은 결과가 나오게 한다.
	bool PairLess(const FAssignPair& A, const FAssignPair& B)
	{
		if (A.DistSquared != B.DistSquared)
		{
			return A.DistSquared < B.DistSquared;
		}

		return A.Start != B.Start ? A.Start < B.Start : A.Target < B.Target;
	}

	FIntVector ToCell(const FVector& Position, double CellSize)
	{
		return FIntVector(
			FMath::FloorToInt32(Position.X / CellSize),
			FMath::FloorToInt32(Position.Y / CellSize),
			FMath::FloorToInt32(Position.Z / CellSize));
	}

	// 목표 위치를 칸 크기 CellSize 격자에 넣어 두고, 한 점에서 가까운 목표 k개를 찾는다.
	class FTargetGrid
	{
	public:
		FTargetGrid(TConstArrayView<FVector> InTargets, double InCellSize)
			: Targets(InTargets)
			, CellSize(InCellSize)
		{
			MinCell = FIntVector(MAX_int32);
			MaxCell = FIntVector(MIN_int32);

			for (int32 Index = 0; Index < Targets.Num(); ++Index)
			{
				const FIntVector Cell = ToCell(Targets[Index], CellSize);
				Cells.FindOrAdd(Cell).Add(Index);
				MinCell = FIntVector(FMath::Min(MinCell.X, Cell.X), FMath::Min(MinCell.Y, Cell.Y), FMath::Min(MinCell.Z, Cell.Z));
				MaxCell = FIntVector(FMath::Max(MaxCell.X, Cell.X), FMath::Max(MaxCell.Y, Cell.Y), FMath::Max(MaxCell.Z, Cell.Z));
			}
		}

		// Point의 칸에서 시작해 한 겹(체비쇼프 거리 Ring)씩 넓혀 가며 목표를 모은다. 다음 겹의 칸은 Point에서
		// 적어도 Ring × CellSize 떨어져 있으므로, k번째로 가까운 목표가 그 안에 있으면 더 넓히지 않는다.
		// 겹이 목표 격자 전체를 덮으면 목표가 k개보다 적어도 멈춘다. 결과는 가까운 순으로 Out에 덧붙인다.
		void FindNearest(int32 StartIndex, const FVector& Point, int32 Count, TArray<FAssignPair>& Out) const
		{
			const FIntVector Center = ToCell(Point, CellSize);
			TArray<FAssignPair> Found;

			for (int32 Ring = 0;; ++Ring)
			{
				const FIntVector Lo(FMath::Max(Center.X - Ring, MinCell.X), FMath::Max(Center.Y - Ring, MinCell.Y),
					FMath::Max(Center.Z - Ring, MinCell.Z));
				const FIntVector Hi(FMath::Min(Center.X + Ring, MaxCell.X), FMath::Min(Center.Y + Ring, MaxCell.Y),
					FMath::Min(Center.Z + Ring, MaxCell.Z));

				for (int32 X = Lo.X; X <= Hi.X; ++X)
				{
					for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
					{
						for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
						{
							const int32 Chebyshev = FMath::Max3(FMath::Abs(X - Center.X), FMath::Abs(Y - Center.Y),
								FMath::Abs(Z - Center.Z));

							if (Chebyshev != Ring)
							{
								continue;
							}

							if (const TArray<int32>* CellTargets = Cells.Find(FIntVector(X, Y, Z)))
							{
								for (int32 Target : *CellTargets)
								{
									Found.Add({FVector::DistSquared(Point, Targets[Target]), StartIndex, Target});
								}
							}
						}
					}
				}

				const bool bCoversGrid = Center.X - Ring <= MinCell.X && Center.Y - Ring <= MinCell.Y
					&& Center.Z - Ring <= MinCell.Z && Center.X + Ring >= MaxCell.X && Center.Y + Ring >= MaxCell.Y
					&& Center.Z + Ring >= MaxCell.Z;

				if (Found.Num() >= Count)
				{
					Found.Sort(PairLess);
					const double SafeDistance = Ring * CellSize;

					if (Found[Count - 1].DistSquared <= SafeDistance * SafeDistance || bCoversGrid)
					{
						break;
					}
				}
				else if (bCoversGrid)
				{
					Found.Sort(PairLess);
					break;
				}
			}

			Out.Append(Found.GetData(), FMath::Min(Count, Found.Num()));
		}

		// Point에서 Radius 안에 있는 목표 번호를 가까운 순(같으면 번호 순)으로 Out에 담는다.
		void FindWithin(const FVector& Point, double Radius, TArray<int32>& Out) const
		{
			Out.Reset();
			const FIntVector Lo = ToCell(Point - FVector(Radius), CellSize);
			const FIntVector Hi = ToCell(Point + FVector(Radius), CellSize);
			const double RadiusSquared = Radius * Radius;
			TArray<TPair<double, int32>> Found;

			for (int32 X = FMath::Max(Lo.X, MinCell.X); X <= FMath::Min(Hi.X, MaxCell.X); ++X)
			{
				for (int32 Y = FMath::Max(Lo.Y, MinCell.Y); Y <= FMath::Min(Hi.Y, MaxCell.Y); ++Y)
				{
					for (int32 Z = FMath::Max(Lo.Z, MinCell.Z); Z <= FMath::Min(Hi.Z, MaxCell.Z); ++Z)
					{
						if (const TArray<int32>* CellTargets = Cells.Find(FIntVector(X, Y, Z)))
						{
							for (int32 Target : *CellTargets)
							{
								const double DistSquared = FVector::DistSquared(Point, Targets[Target]);

								if (DistSquared <= RadiusSquared)
								{
									Found.Emplace(DistSquared, Target);
								}
							}
						}
					}
				}
			}

			Found.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B)
			{
				return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
			});

			for (const TPair<double, int32>& Pair : Found)
			{
				Out.Add(Pair.Value);
			}
		}

	private:
		TConstArrayView<FVector> Targets;
		double CellSize;
		TMap<FIntVector, TArray<int32>> Cells;
		FIntVector MinCell;
		FIntVector MaxCell;
	};

	// 그리디 결과에서 LongMoveThreshold보다 먼 짝을 도미노(증가 경로)로 고친다. 고친 수를 반환한다.
	// 구체 수와 목표 수가 같으므로 먼 짝 (P, 먼 목표 G)를 풀면 빈 목표는 G 하나뿐이다. "구체 q가 기준 이내로 갈 수 있는 목표 t"를
	// 간선으로 보고 P에서 BFS를 하며, 주인이 있는 목표에 닿으면 그 주인이 밀려나 다음 목표를 찾는다. G에 처음 닿은 경로가
	// 건드리는 구체 수가 가장 적은 사슬이고, 사슬을 따라 짝을 한 칸씩 옮기면 사슬 안 모든 이동이 기준 이내다.
	// 비용 변화(새 거리 - 옛 거리)가 음수일 수 있어 다익스트라 대신 간선 수만 세는 BFS를 쓴다.
	// 경로를 못 찾거나 탐색 노드가 MaxSearchNodes를 넘으면 그 짝은 그리디 결과 그대로 둔다.
	int32 RepairLongMoves(TConstArrayView<FVector> Starts, TConstArrayView<FVector> Targets, const FTargetGrid& Grid,
		const FSwarmAssignSettings& Settings, TArray<int32>& Assigned)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Swarm_Domino);
		const int32 Count = Starts.Num();
		const double Threshold = Settings.LongMoveThreshold;
		const double ThresholdSquared = Threshold * Threshold;

		// 먼 짝을 먼 순서(같으면 구체 번호 순)로 모은다.
		TArray<TPair<double, int32>> LongStarts;

		for (int32 Start = 0; Start < Count; ++Start)
		{
			const double DistSquared = FVector::DistSquared(Starts[Start], Targets[Assigned[Start]]);

			if (DistSquared > ThresholdSquared)
			{
				LongStarts.Emplace(DistSquared, Start);
			}
		}

		if (LongStarts.IsEmpty() || Settings.MaxLongMoveRepairs <= 0)
		{
			return 0;
		}

		LongStarts.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B)
		{
			return A.Key != B.Key ? A.Key > B.Key : A.Value < B.Value;
		});

		TArray<int32> Owner;
		Owner.Init(INDEX_NONE, Count);

		for (int32 Start = 0; Start < Count; ++Start)
		{
			Owner[Assigned[Start]] = Start;
		}

		// 구체마다 기준 이내 목표 목록. 필요한 구체만 처음 쓸 때 만든다.
		TArray<TArray<int32>> Reachable;
		Reachable.SetNum(Count);
		TArray<bool> bReachableBuilt;
		bReachableBuilt.Init(false, Count);

		// 목표별 방문 표시는 시도 번호로 구분해 매번 배열을 비우지 않는다. Via[t]는 t로 옮겨 갈 구체다.
		TArray<int32> VisitStamp;
		VisitStamp.Init(INDEX_NONE, Count);
		TArray<int32> Via;
		Via.Init(INDEX_NONE, Count);
		TArray<int32> Queue;
		int32 Repaired = 0;
		int32 Attempts = 0;

		for (const TPair<double, int32>& Long : LongStarts)
		{
			if (Attempts >= Settings.MaxLongMoveRepairs)
			{
				break;
			}

			const int32 First = Long.Value;
			const int32 Goal = Assigned[First];

			// 앞선 사슬에 끼어 이미 기준 이내로 옮겨졌으면 건너뛴다.
			if (FVector::DistSquared(Starts[First], Targets[Goal]) <= ThresholdSquared)
			{
				continue;
			}

			++Attempts;
			Assigned[First] = INDEX_NONE;
			Owner[Goal] = INDEX_NONE;
			Queue.Reset();
			Queue.Add(First);
			bool bFound = false;

			for (int32 Head = 0; Head < Queue.Num() && Head < Settings.MaxSearchNodes && !bFound; ++Head)
			{
				const int32 Particle = Queue[Head];

				if (!bReachableBuilt[Particle])
				{
					Grid.FindWithin(Starts[Particle], Threshold, Reachable[Particle]);
					bReachableBuilt[Particle] = true;
				}

				for (int32 Target : Reachable[Particle])
				{
					if (VisitStamp[Target] == Attempts || Target == Assigned[Particle])
					{
						continue;
					}

					VisitStamp[Target] = Attempts;
					Via[Target] = Particle;

					if (Owner[Target] == INDEX_NONE)
					{
						bFound = true;
						break;
					}

					Queue.Add(Owner[Target]);
				}
			}

			if (!bFound)
			{
				Assigned[First] = Goal;
				Owner[Goal] = First;
				continue;
			}

			// 빈 목표(Goal)에서 거꾸로 따라가며 한 칸씩 민다. 각 구체는 자기 목표를 비우고 Via로 기록된 목표로 옮긴다.
			for (int32 Target = Goal; Target != INDEX_NONE;)
			{
				const int32 Particle = Via[Target];
				const int32 Previous = Assigned[Particle];
				Assigned[Particle] = Target;
				Owner[Target] = Particle;
				Target = Previous;
			}

			++Repaired;
		}

		return Repaired;
	}
}

// 1) 목표를 격자에 넣는다. 2) 시작 위치마다 가까운 후보 목표 k개를 찾는다.
// 3) 모든 후보 쌍을 가까운 순으로 정렬해, 시작 위치와 목표가 둘 다 비어 있으면 짝을 확정한다(그리디).
// 4) 후보를 모두 뺏긴 시작 위치는 남은 빈 목표 중 가장 가까운 것과 짝짓는다. 남은 수가 적으므로 전부 비교한다.
// 5) 기준보다 먼 짝을 도미노로 고친다(RepairLongMoves). 6) 이동 거리 통계를 계산한다.
bool FSwarmTargetAssigner::Assign(TConstArrayView<FVector> Starts, TConstArrayView<FVector> Targets,
	const FSwarmAssignSettings& Settings, TArray<int32>& OutTargetIndices, FSwarmAssignStats* OutStats)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Swarm_Assign);
	const double StartTime = FPlatformTime::Seconds();
	OutTargetIndices.Reset();

	if (Starts.Num() != Targets.Num() || Settings.CandidateCount < 1 || !(Settings.CellSize > 0.0))
	{
		return false;
	}

	const int32 Count = Starts.Num();
	FSwarmAssignStats Stats;

	if (Count > 0)
	{
		const FTargetGrid Grid(Targets, Settings.CellSize);
		const int32 CandidateCount = FMath::Min(Settings.CandidateCount, Count);
		TArray<FAssignPair> Pairs;
		Pairs.Reserve(Count * CandidateCount);

		for (int32 Start = 0; Start < Count; ++Start)
		{
			Grid.FindNearest(Start, Starts[Start], CandidateCount, Pairs);
		}

		Pairs.Sort(PairLess);

		OutTargetIndices.Init(INDEX_NONE, Count);
		TArray<bool> TargetTaken;
		TargetTaken.Init(false, Count);

		for (const FAssignPair& Pair : Pairs)
		{
			if (OutTargetIndices[Pair.Start] == INDEX_NONE && !TargetTaken[Pair.Target])
			{
				OutTargetIndices[Pair.Start] = Pair.Target;
				TargetTaken[Pair.Target] = true;
			}
		}

		TArray<int32> FreeTargets;

		for (int32 Target = 0; Target < Count; ++Target)
		{
			if (!TargetTaken[Target])
			{
				FreeTargets.Add(Target);
			}
		}

		for (int32 Start = 0; Start < Count; ++Start)
		{
			if (OutTargetIndices[Start] != INDEX_NONE)
			{
				continue;
			}

			int32 BestSlot = 0;
			double BestDistSquared = TNumericLimits<double>::Max();

			for (int32 Slot = 0; Slot < FreeTargets.Num(); ++Slot)
			{
				const double DistSquared = FVector::DistSquared(Starts[Start], Targets[FreeTargets[Slot]]);

				if (DistSquared < BestDistSquared)
				{
					BestDistSquared = DistSquared;
					BestSlot = Slot;
				}
			}

			OutTargetIndices[Start] = FreeTargets[BestSlot];
			FreeTargets.RemoveAt(BestSlot);
			++Stats.Leftovers;
		}

		for (int32 Start = 0; Start < Count; ++Start)
		{
			Stats.GreedyLongMoves += FVector::Dist(Starts[Start], Targets[OutTargetIndices[Start]]) > Settings.LongMoveThreshold ? 1 : 0;
		}

		Stats.Repaired = RepairLongMoves(Starts, Targets, Grid, Settings, OutTargetIndices);

		double MoveSum = 0.0;

		for (int32 Start = 0; Start < Count; ++Start)
		{
			const double Move = FVector::Dist(Starts[Start], Targets[OutTargetIndices[Start]]);
			MoveSum += Move;
			Stats.MaxMove = FMath::Max(Stats.MaxMove, Move);
			Stats.LongMoves += Move > Settings.LongMoveThreshold ? 1 : 0;
		}

		Stats.AverageMove = MoveSum / Count;
	}

	Stats.Milliseconds = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (OutStats)
	{
		*OutStats = Stats;
	}

	return true;
}
