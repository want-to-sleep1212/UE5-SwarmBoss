// SwarmShapeSampler.cpp
// FSwarmShapeSampler 구현.
// - GenerateTargets(): 설정 검증 → 필요 부피 V = N·d³/k로 도형 크기 계산 → Bridson 방식 샘플링
//   (활성점, d~2d 구각 후보, 셀 크기 d 격자의 27셀 이웃 검사) → 영역 전체를 채운 뒤 N개를 무작위 부분 선택.
//   개수가 부족하면 MaxExpansionRatio 안에서 영역을 넓혀 재시도하고, 후보 검사 예산을 넘으면 즉시 실패한다.
// - Contains(): 점이 구·박스 중심 허용 영역 안에 있는지 판정한다.
// - ValidateTargets(): 경계와 최소 간격을 O(N²) 전수 검사로 독립 검증한다. 생성 경로에서는 호출하지 않고 자동 테스트와 Preview 진단에서만 쓴다.

#include "SwarmShapeSampler.h"

#include "HAL/PlatformTime.h"

namespace
{
	// 공간을 한 변이 CellSize인 정육면체로 나누었을 때 Point가 들어가는 셀의 X·Y·Z 정수 번호를 반환한다.
	FIntVector CellFor(const FVector& Point, double CellSize)
	{
		return FIntVector(FMath::FloorToInt(Point.X / CellSize),
			FMath::FloorToInt(Point.Y / CellSize), FMath::FloorToInt(Point.Z / CellSize));
	}

	// Random을 사용해 길이가 1인 무작위 방향 벡터를 반환한다. 구면의 특정 방향에 치우치지 않게 뽑는다.
	FVector RandomDirection(FRandomStream& Random)
	{
		const double Z = Random.FRandRange(-1.0f, 1.0f);
		const double Angle = Random.FRand() * 2.0 * PI;
		const double R = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));

		return FVector(R * FMath::Cos(Angle), R * FMath::Sin(Angle), Z);
	}

	// Extents 크기의 도형 안에서 서로 Distance 이상 떨어진 구체 중심 좌표를 Points에 채운다.
	// 검사한 후보 수를 Tests에 누적하며 MaxCandidateTests에 도달하면 false를 반환한다.
	// true는 이 범위에서 점 생성을 마쳤다는 뜻이며, 요청 개수 충족 여부는 호출자가 확인한다.
	bool GenerateAttempt(const FSwarmSamplingSettings& Settings, const FVector& Extents,
		double Distance, FRandomStream& Random, int32& Tests, TArray<FVector>& Points)
	{
		// 셀 한 변을 d로 두고 인접 27셀을 검사한다. 셀에는 여러 점이 들어갈 수 있다.
		TMap<FIntVector, TArray<int32>> Grid;
		TArray<int32> Active;

		// 새 좌표를 Points에 추가하고, 그 주변에서 다음 좌표를 찾도록 Active에 인덱스를 넣는다.
		// 가까운 점을 검사할 때 찾을 수 있도록 Grid의 해당 셀에도 같은 인덱스를 등록한다.
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

// 원점을 중심으로 한 Shape 도형 안에 Point가 있는지 반환한다.
// HalfExtents는 구에서는 X값이 반지름이고, 박스에서는 각 축의 반길이다.
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

// Points의 모든 좌표가 지정 도형 안에 있고, 모든 두 점 사이 거리가 MinimumDistance 이상인지 검사한다.
// 실제 최소 거리를 OutMeasuredMinimum에 기록한다. 점이 0개나 1개이면 측정값은 0이다.
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

// S.ParticleCount와 구체 간 최소 간격으로 도형 크기를 계산하고, 도형 중심 기준의 구체 좌표를 생성한다.
// 점이 부족하면 허용 범위 내에서 도형을 확대해 재시도한다. 반환 결과의 bSuccess로 성공 여부를 확인한다.
FSwarmSamplingResult FSwarmShapeSampler::GenerateTargets(const FSwarmSamplingSettings& S)
{
	FSwarmSamplingResult Result;
	const double Start = FPlatformTime::Seconds();

	// Result에 생성 성공·실패의 설명과 함수 시작 후 경과 시간을 채워 반환한다.
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
			// N개에서 즉시 중단하면 시작점 주변만 채워진다. 섞은 뒤 균일하게 부분 선택한다.
			for (int32 I = 0; I < S.ParticleCount; ++I)
			{
				Result.Points.Swap(I, Random.RandRange(I, Result.Points.Num() - 1));
			}
			Result.Points.SetNum(S.ParticleCount, EAllowShrinking::No);

			// 간격·경계는 생성 중 격자 검사로 이미 지켰으므로 O(N²) 전수 검사(ValidateTargets)는 다시 하지 않는다.
			// 독립 검증은 자동 테스트가 맡는다(2026-11-11 결정, 2026-11-15 적용).
			Result.bSuccess = true;
			return Finish(TEXT("Success."));
		}
	}

	return Finish(TEXT("Not enough points within expansion limit; no result committed."));
}
