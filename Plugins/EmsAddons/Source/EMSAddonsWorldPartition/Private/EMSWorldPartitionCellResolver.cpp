//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionCellResolver.h"

#include "EMSAddonsWorldPartition.h"
#include "EMSWorldPartitionRuntimeTypes.h"
#include "Engine/World.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeCellInterface.h"
#include "WorldPartition/WorldPartitionRuntimeLevelStreamingCell.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"

bool EMSAddonsWorldPartition::IsSupportedCell(const UWorldPartitionRuntimeLevelStreamingCell* Cell)
{
	return IsValid(Cell)
		&& Cell->IsSpatiallyLoaded()
		&& !Cell->GetIsHLOD()
		&& !Cell->HasDataLayers();
}

EMSAddonsWorldPartition::FEMSWorldPartitionCellCoverage EMSAddonsWorldPartition::QueryCellCoverage(
	const UWorld* World,
	const FVector& Location,
	const UWorldPartitionRuntimeLevelStreamingCell* CellToIgnore)
{
	FEMSWorldPartitionCellCoverage Coverage;
	if (!IsValid(World) || Location.ContainsNaN())
	{
		return Coverage;
	}

	const UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!IsValid(WorldPartition))
	{
		return Coverage;
	}

	//A zero radius makes the engine emit no query shape at all, which would intersect no 
	//cells. The radius only has to be large enough to produce a shape, because the
	//candidates below are still filtered down to the ones containing the location.
	FWorldPartitionStreamingQuerySource Source(Location);
	Source.Radius = 1.0f;
	Source.bUseGridLoadingRange = false;
	Source.bSpatialQuery = true;
	Source.bIncludeAnyDataLayer = true;

	TArray<const IWorldPartitionCell*> QueryCells;
	if (!WorldPartition->GetIntersectingCells({Source}, QueryCells))
	{
		return Coverage;
	}

	//Ranks holds every candidate for the coverage answer; VisibleRanks is the visible
	//subset, ranked by the same order so the chosen cell does not depend on query order.
	TArray<FEMSWorldPartitionCellCandidateRank> Ranks;
	TArray<FEMSWorldPartitionCellCandidateRank> VisibleRanks;
	TArray<const UWorldPartitionRuntimeLevelStreamingCell*> VisibleCells;
	Ranks.Reserve(QueryCells.Num());
	for (const IWorldPartitionCell* QueryCell : QueryCells)
	{
		const UWorldPartitionRuntimeLevelStreamingCell* Cell =
			Cast<UWorldPartitionRuntimeLevelStreamingCell>(QueryCell);
		if (Cell && Cell == CellToIgnore)
		{
			continue;
		}

		FEMSWorldPartitionCellCandidateRank& Rank = Ranks.AddDefaulted_GetRef();
		Rank.bLevelStreamingCell = Cell != nullptr;
		if (!Cell)
		{
			continue;
		}

		const FBox CellBounds = Cell->GetCellBounds();
		const FBox StreamingBounds = Cell->GetStreamingBounds();
		const FBox RankingBounds = StreamingBounds.IsValid ? StreamingBounds : CellBounds;
		Rank.bContainsLocation = (CellBounds.IsValid && CellBounds.IsInsideOrOn(Location))
			|| (StreamingBounds.IsValid && StreamingBounds.IsInsideOrOn(Location));
		Rank.bSpatiallyLoaded = Cell->IsSpatiallyLoaded();
		Rank.bHLOD = Cell->GetIsHLOD();
		Rank.bHasDataLayers = Cell->HasDataLayers();
		Rank.CellGuid = Cell->GetGuid();
		Rank.DebugName = Cell->GetDebugName();
		if (RankingBounds.IsValid)
		{
			const FVector Size = RankingBounds.GetSize().GetAbs();
			Rank.XYArea = Size.X * Size.Y;
			Rank.Volume = Size.X * Size.Y * Size.Z;
		}

		if (Rank.IsSupported())
		{
			Coverage.bCovered = true;
			if (Cell->IsVisible())
			{
				VisibleRanks.Add(Rank);
				VisibleCells.Add(Cell);
			}
		}
	}

	const int32 BestVisibleIndex = SelectBestCellCandidate(VisibleRanks);
	Coverage.VisibleCell = VisibleCells.IsValidIndex(BestVisibleIndex) ? VisibleCells[BestVisibleIndex] : nullptr;

	//Diagnostic only. Enable with -LogCmds="LogEMSAddonsWorldPartition Verbose".
	if (UE_LOG_ACTIVE(LogEMSAddonsWorldPartition, Verbose))
	{
		UE_LOG(
			LogEMSAddonsWorldPartition,
			Verbose,
			TEXT("Cell coverage at %s: %d cell(s) queried, covered=%d, visible cell=%s."),
			*Location.ToCompactString(),
			QueryCells.Num(),
			Coverage.bCovered ? 1 : 0,
			Coverage.VisibleCell ? *Coverage.VisibleCell->GetDebugName() : TEXT("None"));
		for (const FEMSWorldPartitionCellCandidateRank& Rank : Ranks)
		{
			UE_LOG(
				LogEMSAddonsWorldPartition,
				Verbose,
				TEXT("  %s LevelStreaming=%d Contains=%d Spatial=%d HLOD=%d DataLayers=%d XYArea=%.0f"),
				*Rank.DebugName,
				Rank.bLevelStreamingCell ? 1 : 0,
				Rank.bContainsLocation ? 1 : 0,
				Rank.bSpatiallyLoaded ? 1 : 0,
				Rank.bHLOD ? 1 : 0,
				Rank.bHasDataLayers ? 1 : 0,
				Rank.XYArea);
		}
	}

	return Coverage;
}
