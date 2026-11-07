//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSCheckpointUtils.h"

#include "EMSCheckpointSaveGame.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"

FSoftObjectPath EMSCheckpoint::GetWorldAssetPath(const UWorld* World)
{
	if (!World)
	{
		return FSoftObjectPath();
	}

	const FString PackageName = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	const FString AssetName = FPackageName::GetShortName(PackageName);
	return FSoftObjectPath(PackageName + TEXT(".") + AssetName);
}

bool EMSCheckpoint::IsSameWorld(const UWorld* World, const FSoftObjectPath& WorldPath)
{
	return World && !WorldPath.IsNull() && GetWorldAssetPath(World).GetAssetPath() == WorldPath.GetAssetPath();
}

bool EMSCheckpoint::IsLoadableLegacyRecord(const UEMSCheckpointSaveGame* Save)
{
	return Save
		&& Save->bHasCheckpoint
		&& Save->LatestCheckpoint.Version == FEMSCheckpointRecord::LegacyVersion
		&& Save->LatestCheckpoint.CheckpointId.IsValid()
		&& !Save->LatestCheckpoint.World.IsNull()
		&& Save->CheckpointGeneration.IsValid()
		&& Save->CheckpointGeneration == Save->SaveGeneration;
}

bool EMSCheckpoint::ResolveRecord(
	const UEMSCheckpointSaveGame* Save,
	const FString& HolderSlot,
	FEMSCheckpointRecord& OutRecord)
{
	OutRecord = FEMSCheckpointRecord();
	if (!Save || !Save->bHasCheckpoint)
	{
		return false;
	}

	FEMSCheckpointRecord Record = Save->LatestCheckpoint;

	// Loading a soft object path in PIE adds the PIE prefix to it. Records always store and
	// compare the plain world path (see GetWorldAssetPath), so remove it again.
	if (!Record.World.IsNull())
	{
		Record.World = FSoftObjectPath(UWorld::RemovePIEPrefix(Record.World.ToString()));
	}

	if (IsLoadableLegacyRecord(Save))
	{
		Record.Version = FEMSCheckpointRecord::CurrentVersion;
		Record.SaveSlot = HolderSlot;
	}

	if (!Record.IsValid())
	{
		return false;
	}
	OutRecord = Record;
	return true;
}
