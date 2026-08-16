//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSWorldPartitionRuntimeTypes.h"

namespace
{
	bool IsBetter(
		const FEMSWorldPartitionCellCandidateRank& Candidate,
		const FEMSWorldPartitionCellCandidateRank& Current)
	{
		if (Candidate.XYArea != Current.XYArea)
		{
			return Candidate.XYArea < Current.XYArea;
		}
		if (Candidate.Volume != Current.Volume)
		{
			return Candidate.Volume < Current.Volume;
		}
		const FString CandidateGuid = Candidate.CellGuid.ToString(EGuidFormats::Digits);
		const FString CurrentGuid = Current.CellGuid.ToString(EGuidFormats::Digits);
		const int32 GuidOrder = CandidateGuid.Compare(CurrentGuid, ESearchCase::CaseSensitive);
		return GuidOrder < 0
			|| (GuidOrder == 0 && Candidate.DebugName.Compare(Current.DebugName, ESearchCase::CaseSensitive) < 0);
	}
}

int32 EMSAddonsWorldPartition::SelectBestCellCandidate(
	const TArray<FEMSWorldPartitionCellCandidateRank>& Candidates)
{
	int32 BestIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		if (!Candidates[Index].IsSupported())
		{
			continue;
		}
		if (BestIndex == INDEX_NONE || IsBetter(Candidates[Index], Candidates[BestIndex]))
		{
			BestIndex = Index;
		}
	}
	return BestIndex;
}
