#include "Swarm/SwarmTargetAssigner.h"

#include "HAL/PlatformTime.h"

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

	private:
		TConstArrayView<FVector> Targets;
		double CellSize;
		TMap<FIntVector, TArray<int32>> Cells;
		FIntVector MinCell;
		FIntVector MaxCell;
	};
}

// 1) 목표를 격자에 넣는다. 2) 시작 위치마다 가까운 후보 목표 k개를 찾는다.
// 3) 모든 후보 쌍을 가까운 순으로 정렬해, 시작 위치와 목표가 둘 다 비어 있으면 짝을 확정한다(그리디).
// 4) 후보를 모두 뺏긴 시작 위치는 남은 빈 목표 중 가장 가까운 것과 짝짓는다. 남은 수가 적으므로 전부 비교한다.
// 5) 이동 거리 통계를 계산한다.
bool FSwarmTargetAssigner::Assign(TConstArrayView<FVector> Starts, TConstArrayView<FVector> Targets,
	const FSwarmAssignSettings& Settings, TArray<int32>& OutTargetIndices, FSwarmAssignStats* OutStats)
{
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
