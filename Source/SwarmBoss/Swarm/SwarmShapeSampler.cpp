#include "SwarmShapeSampler.h"

#include "HAL/PlatformTime.h"

namespace
{
	FIntVector CellFor(const FVector& Point, double CellSize)
	{
		return FIntVector(FMath::FloorToInt(Point.X / CellSize),
			FMath::FloorToInt(Point.Y / CellSize), FMath::FloorToInt(Point.Z / CellSize));
	}

	FVector RandomDirection(FRandomStream& Random)
	{
		const double Z = Random.FRandRange(-1.0f, 1.0f);
		const double Angle = Random.FRand() * 2.0 * PI;
		const double R = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
		return FVector(R * FMath::Cos(Angle), R * FMath::Sin(Angle), Z);
	}

	bool GenerateAttempt(const FSwarmSamplingSettings& Settings, const FVector& Extents,
		double Distance, FRandomStream& Random, int32& Tests, TArray<FVector>& Points)
	{
		// 셀 한 변을 d로 두고 인접 27셀을 검사한다. 셀에는 여러 점이 들어갈 수 있다.
		TMap<FIntVector, TArray<int32>> Grid;
		TArray<int32> Active;
		auto Add = [&](const FVector& Point)
		{
			const int32 Index = Points.Add(Point);
			Active.Add(Index);
			Grid.FindOrAdd(CellFor(Point, Distance)).Add(Index);
		};

		FVector First;
		if (Settings.Shape == ESwarmPrimitive::Sphere)
		{
			First = RandomDirection(Random) * Extents.X * FMath::Pow(double(Random.FRand()), 1.0 / 3.0);
		}
		else
		{
			First = FVector(Random.FRandRange(-1.f, 1.f), Random.FRandRange(-1.f, 1.f),
				Random.FRandRange(-1.f, 1.f)) * Extents;
		}
		Add(First);
		while (!Active.IsEmpty())
		{
			const int32 ActiveSlot = Random.RandRange(0, Active.Num() - 1);
			const FVector Origin = Points[Active[ActiveSlot]];
			bool bAdded = false;
			for (int32 CandidateIndex = 0; CandidateIndex < Settings.CandidatesPerPoint; ++CandidateIndex)
			{
				if (Tests >= Settings.MaxCandidateTests)
				{
					return false;
				}
				++Tests;
				// d~2d 구각의 부피에 대해 균일한 반지름 분포.
				const double Radius = Distance * FMath::Pow(1.0 + 7.0 * Random.FRand(), 1.0 / 3.0);
				const FVector Candidate = Origin + RandomDirection(Random) * Radius;
				if (!FSwarmShapeSampler::Contains(Candidate, Settings.Shape, Extents))
				{
					continue;
				}
				const FIntVector Cell = CellFor(Candidate, Distance);
				bool bSeparated = true;
				for (int32 X = -1; X <= 1 && bSeparated; ++X)
				for (int32 Y = -1; Y <= 1 && bSeparated; ++Y)
				for (int32 Z = -1; Z <= 1 && bSeparated; ++Z)
				{
					if (const TArray<int32>* Neighbors = Grid.Find(Cell + FIntVector(X, Y, Z)))
					{
						for (const int32 Index : *Neighbors)
						{
							if (FVector::DistSquared(Candidate, Points[Index]) < Distance * Distance)
							{
								bSeparated = false;
								break;
							}
						}
					}
				}
				if (bSeparated)
				{
					Add(Candidate);
					bAdded = true;
					break;
				}
			}
			if (!bAdded)
			{
				Active.RemoveAtSwap(ActiveSlot, 1, EAllowShrinking::No);
			}
		}
		return true;
	}
}

bool FSwarmShapeSampler::Contains(const FVector& Point, ESwarmPrimitive Shape, const FVector& HalfExtents)
{
	if (Point.ContainsNaN())
	{
		return false;
	}
	if (Shape == ESwarmPrimitive::Sphere)
	{
		return Point.SizeSquared() <= HalfExtents.X * HalfExtents.X;
	}
	return FMath::Abs(Point.X) <= HalfExtents.X && FMath::Abs(Point.Y) <= HalfExtents.Y
		&& FMath::Abs(Point.Z) <= HalfExtents.Z;
}

bool FSwarmShapeSampler::ValidateTargets(const TArray<FVector>& Points, ESwarmPrimitive Shape,
	const FVector& HalfExtents, double MinimumDistance, double& OutMeasuredMinimum)
{
	// 1단계 검증용 독립 전수 검사. 매 프레임 실행하지 않는다.
	OutMeasuredMinimum = 0.0;
	double MinSquared = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		if (!Contains(Points[I], Shape, HalfExtents))
		{
			return false;
		}
		for (int32 J = 0; J < I; ++J)
		{
			MinSquared = FMath::Min(MinSquared, FVector::DistSquared(Points[I], Points[J]));
		}
	}
	if (Points.Num() > 1)
	{
		OutMeasuredMinimum = FMath::Sqrt(MinSquared);
		return OutMeasuredMinimum + 1.e-6 >= MinimumDistance;
	}
	return true;
}

FSwarmSamplingResult FSwarmShapeSampler::GenerateTargets(const FSwarmSamplingSettings& S)
{
	FSwarmSamplingResult Result;
	const double Start = FPlatformTime::Seconds();
	auto Finish = [&](const FString& Message)
	{
		Result.Message = Message;
		Result.ElapsedMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
		return Result;
	};
	if (S.ParticleCount < 0 || S.ParticleCount > 10000 || !FMath::IsFinite(S.ParticleRadius)
		|| S.ParticleRadius < 0.01f || !FMath::IsFinite(S.SpacingMargin) || S.SpacingMargin < 0
		|| !FMath::IsFinite(S.PackingCoefficient) || S.PackingCoefficient < 0.01f || S.PackingCoefficient > 10.f
		|| S.BoxHalfExtents.ContainsNaN() || S.BoxHalfExtents.GetMin() <= 0
		|| S.CandidatesPerPoint < 1 || S.CandidatesPerPoint > 100
		|| S.MaxAttempts < 1 || S.MaxAttempts > 16 || S.MaxCandidateTests < 100 || S.MaxCandidateTests > 10000000
		|| !FMath::IsFinite(S.MaxExpansionRatio) || S.MaxExpansionRatio < 0 || S.MaxExpansionRatio > 1
		|| (S.Shape != ESwarmPrimitive::Sphere && S.Shape != ESwarmPrimitive::Box))
	{
		return Finish(TEXT("Invalid sampling settings."));
	}
	Result.MinimumDistance = 2.0 * S.ParticleRadius + S.SpacingMargin;
	if (S.ParticleCount == 0)
	{
		Result.bSuccess = true;
		return Finish(TEXT("Empty shape."));
	}
	const double Volume = S.ParticleCount * FMath::Pow(Result.MinimumDistance, 3.0) / S.PackingCoefficient;
	FVector InitialExtents;
	if (S.Shape == ESwarmPrimitive::Sphere)
	{
		InitialExtents = FVector(FMath::Pow(Volume / (4.0 * PI / 3.0), 1.0 / 3.0));
	}
	else
	{
		// 기준 크기의 절대 단위 대신 종횡비만 사용해 중간 부피의 오버플로를 피한다.
		const FVector Ratio = S.BoxHalfExtents / S.BoxHalfExtents.GetMax();
		const double Scale = FMath::Pow(Volume / (8.0 * Ratio.X * Ratio.Y * Ratio.Z), 1.0 / 3.0);
		InitialExtents = Ratio * Scale;
	}
	if (InitialExtents.ContainsNaN() || InitialExtents.GetMax() > 1.e8 || InitialExtents.GetMin() <= 0
		|| InitialExtents.GetMax() / Result.MinimumDistance > 1.e6)
	{
		return Finish(TEXT("Shape dimensions exceed supported range."));
	}
	for (int32 Attempt = 0; Attempt < S.MaxAttempts; ++Attempt)
	{
		Result.Attempts = Attempt + 1;
		Result.ExpansionRatio = S.MaxAttempts > 1 ? double(S.MaxExpansionRatio) * Attempt / (S.MaxAttempts - 1) : 0.0;
		Result.HalfExtents = InitialExtents * (1.0 + Result.ExpansionRatio);
		Result.Points.Reset();
		FRandomStream Random(static_cast<int32>(uint32(S.Seed) + uint32(Attempt) * 196613u));
		if (S.ParticleCount == 1)
		{
			Result.Points.Add(FVector::ZeroVector);
		}
		else if (!GenerateAttempt(S, Result.HalfExtents, Result.MinimumDistance, Random,
			Result.CandidateTests, Result.Points))
		{
			return Finish(TEXT("Candidate budget exhausted; no result committed."));
		}
		if (Result.Points.Num() >= S.ParticleCount)
		{
			// N개에서 즉시 중단하면 시작점 주변만 채워진다. 영역을 샘플링한 뒤 균일하게 부분 선택한다.
			for (int32 I = 0; I < S.ParticleCount; ++I)
			{
				Result.Points.Swap(I, Random.RandRange(I, Result.Points.Num() - 1));
			}
			Result.Points.SetNum(S.ParticleCount, EAllowShrinking::No);
			Result.bSuccess = ValidateTargets(Result.Points, S.Shape, Result.HalfExtents,
				Result.MinimumDistance, Result.MeasuredMinimumDistance);
			return Finish(Result.bSuccess ? TEXT("Success.") : TEXT("Target validation failed."));
		}
	}
	return Finish(TEXT("Not enough points within expansion limit; no result committed."));
}
