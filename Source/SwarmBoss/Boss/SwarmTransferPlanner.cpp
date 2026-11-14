#include "Boss/SwarmTransferPlanner.h"

namespace
{
	struct FFlowEdge
	{
		ESwarmPartId To = ESwarmPartId::None;
		int32 Count = 0;
	};
}

bool FSwarmTransferPlanner::Plan(const TMap<ESwarmPartId, ESwarmPartId>& Parents,
	const TMap<ESwarmPartId, int32>& CurrentCounts, const TMap<ESwarmPartId, int32>& TargetCounts,
	TArray<FSwarmTransferShipment>& OutShipments)
{
	OutShipments.Reset();

	// 부위 순서를 enum 값으로 고정해 같은 입력이면 항상 같은 계획을 만든다.
	TArray<ESwarmPartId> PartIds;
	Parents.GetKeys(PartIds);
	PartIds.Sort();

	if (PartIds.IsEmpty() || CurrentCounts.Num() != PartIds.Num() || TargetCounts.Num() != PartIds.Num())
	{
		return false;
	}

	int64 CountDifference = 0;
	int32 RootCount = 0;
	TMap<ESwarmPartId, int32> Depths;

	for (ESwarmPartId PartId : PartIds)
	{
		const int32* Current = CurrentCounts.Find(PartId);
		const int32* Target = TargetCounts.Find(PartId);
		const ESwarmPartId ParentId = Parents.FindChecked(PartId);

		if (!Current || !Target || *Current < 0 || *Target < 0
			|| (ParentId != ESwarmPartId::None && !Parents.Contains(ParentId)))
		{
			return false;
		}

		CountDifference += *Current - *Target;
		RootCount += ParentId == ESwarmPartId::None ? 1 : 0;

		// 부모를 따라 올라가며 깊이를 잰다. 부위 수보다 많이 올라가면 순환이다.
		int32 Depth = 0;

		for (ESwarmPartId Ancestor = ParentId; Ancestor != ESwarmPartId::None; Ancestor = Parents.FindChecked(Ancestor))
		{
			if (++Depth > PartIds.Num())
			{
				return false;
			}
		}

		Depths.Add(PartId, Depth);
	}

	if (CountDifference != 0 || RootCount != 1)
	{
		return false;
	}

	// 깊은 부위부터 (현재 - 목표)를 부모에게 더해 각 부위 아래쪽 전체의 합을 구한다.
	TArray<ESwarmPartId> DeepestFirst = PartIds;
	DeepestFirst.StableSort([&Depths](ESwarmPartId A, ESwarmPartId B) { return Depths[A] > Depths[B]; });

	TMap<ESwarmPartId, int64> SubtreeSurplus;

	for (ESwarmPartId PartId : PartIds)
	{
		SubtreeSurplus.Add(PartId, int64(CurrentCounts[PartId]) - TargetCounts[PartId]);
	}

	TMap<ESwarmPartId, TArray<FFlowEdge>> OutEdges;
	TMap<ESwarmPartId, int32> InDegrees;

	for (ESwarmPartId PartId : PartIds)
	{
		OutEdges.Add(PartId);
		InDegrees.Add(PartId, 0);
	}

	for (ESwarmPartId PartId : DeepestFirst)
	{
		const ESwarmPartId ParentId = Parents[PartId];

		if (ParentId == ESwarmPartId::None)
		{
			continue;
		}

		const int64 Flow = SubtreeSurplus[PartId];
		SubtreeSurplus[ParentId] += Flow;

		if (Flow > 0)
		{
			OutEdges[PartId].Add({ParentId, int32(Flow)});
			++InDegrees[ParentId];
		}
		else if (Flow < 0)
		{
			OutEdges[ParentId].Add({PartId, int32(-Flow)});
			++InDegrees[PartId];
		}
	}

	for (TPair<ESwarmPartId, TArray<FFlowEdge>>& Pair : OutEdges)
	{
		Pair.Value.Sort([](const FFlowEdge& A, const FFlowEdge& B) { return A.To < B.To; });
	}

	// 흐름 방향은 트리 위에서 한 방향이므로 순환이 없다. 들어오는 흐름을 모두 계획한 부위부터 내보내는 몫을 정한다.
	// 각 부위는 자기 구체로 먼저 내보내고, 모자라면 자기에게 들어오는 묶음의 경로를 연장한다.
	// 내보내는 수 - 들어오는 수 = 현재 - 목표 ≤ 현재이므로, 자기 구체와 들어오는 묶음으로 항상 채울 수 있다.
	TMap<ESwarmPartId, TArray<int32>> Arrivals;
	TArray<ESwarmPartId> Ready;

	for (ESwarmPartId PartId : PartIds)
	{
		Arrivals.Add(PartId);

		if (InDegrees[PartId] == 0)
		{
			Ready.Add(PartId);
		}
	}

	int32 ProcessedCount = 0;

	for (int32 ReadyIndex = 0; ReadyIndex < Ready.Num(); ++ReadyIndex)
	{
		const ESwarmPartId PartId = Ready[ReadyIndex];
		int32 OwnRemaining = CurrentCounts[PartId];
		++ProcessedCount;

		for (const FFlowEdge& Edge : OutEdges[PartId])
		{
			const int32 OwnCount = FMath::Min(Edge.Count, OwnRemaining);
			int32 RelayCount = Edge.Count - OwnCount;
			OwnRemaining -= OwnCount;

			if (OwnCount > 0)
			{
				Arrivals[Edge.To].Add(OutShipments.Add({{PartId, Edge.To}, OwnCount}));
			}

			TArray<int32>& Incoming = Arrivals[PartId];

			while (RelayCount > 0)
			{
				if (Incoming.IsEmpty())
				{
					OutShipments.Reset();
					return false;
				}

				const int32 ShipmentIndex = Incoming[0];
				const int32 ExtendCount = FMath::Min(RelayCount, OutShipments[ShipmentIndex].Count);
				RelayCount -= ExtendCount;

				if (ExtendCount == OutShipments[ShipmentIndex].Count)
				{
					OutShipments[ShipmentIndex].Path.Add(Edge.To);
					Incoming.RemoveAt(0);
					Arrivals[Edge.To].Add(ShipmentIndex);
				}
				else
				{
					FSwarmTransferShipment Extended = OutShipments[ShipmentIndex];
					Extended.Path.Add(Edge.To);
					Extended.Count = ExtendCount;
					OutShipments[ShipmentIndex].Count -= ExtendCount;
					Arrivals[Edge.To].Add(OutShipments.Add(MoveTemp(Extended)));
				}
			}

			if (--InDegrees[Edge.To] == 0)
			{
				Ready.Add(Edge.To);
			}
		}
	}

	if (ProcessedCount != PartIds.Num())
	{
		OutShipments.Reset();
		return false;
	}

	return true;
}
