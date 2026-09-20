//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"

class UWorld;
class UWorldPartitionRuntimeLevelStreamingCell;

namespace EMSAddonsWorldPartition
{
	/**
	 * What the generated cells say about one world location.
	 *
	 * A location is not owned by a single cell. Several supported cells at different
	 * grid levels can cover it, and the engine hides and shows them independently, so
	 * a managed actor belongs in the world for as long as any one of them is visible.
	 */
	struct FEMSWorldPartitionCellCoverage
	{
		/** A supported generated cell covers the location, whether visible or not. */
		bool bCovered = false;

		/** The deterministically chosen visible cell covering the location, if any. */
		const UWorldPartitionRuntimeLevelStreamingCell* VisibleCell = nullptr;

		bool IsVisible() const { return VisibleCell != nullptr; }
	};

	/**
	 * Describes the supported generated cells covering a location.
	 *
	 * CellToIgnore still contributes geometric coverage but is excluded from the
	 * visible-cell result. This lets a hiding-cell callback ask whether another
	 * visible cell will keep the location live without losing the fact that the
	 * departing cell was the location's final supported coverage.
	 */
	FEMSWorldPartitionCellCoverage QueryCellCoverage(
		const UWorld* World,
		const FVector& Location,
		const UWorldPartitionRuntimeLevelStreamingCell* CellToIgnore = nullptr);

	bool IsSupportedCell(const UWorldPartitionRuntimeLevelStreamingCell* Cell);
}
