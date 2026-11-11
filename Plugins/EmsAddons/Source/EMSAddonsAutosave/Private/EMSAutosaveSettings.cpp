//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAutosaveSettings.h"

#include "EMSCheckpointUtils.h"
#include "Engine/World.h"

UEMSAutosaveSettings::UEMSAutosaveSettings()
{
	CategoryName = TEXT("Easy Multi Save Addons");
	SectionName = TEXT("Checkpoint System");
}

bool UEMSAutosaveSettings::IsMapExcluded(const UWorld* World) const
{
	if (!World || ExcludedMaps.IsEmpty())
	{
		return false;
	}

	// The same PIE-free asset path checkpoint records use, so a PIE world matches its editor map.
	const FTopLevelAssetPath WorldPath = EMSCheckpoint::GetWorldAssetPath(World).GetAssetPath();
	for (const TSoftObjectPtr<UWorld>& Excluded : ExcludedMaps)
	{
		if (!Excluded.IsNull()
			&& FSoftObjectPath(UWorld::RemovePIEPrefix(Excluded.ToSoftObjectPath().ToString())).GetAssetPath() == WorldPath)
		{
			return true;
		}
	}
	return false;
}
