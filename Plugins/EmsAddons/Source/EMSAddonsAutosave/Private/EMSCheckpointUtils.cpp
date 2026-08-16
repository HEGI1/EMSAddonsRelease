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

bool EMSCheckpoint::IsCurrentGeneration(const UEMSCheckpointSaveGame* Save)
{
	return Save
		&& Save->bHasCheckpoint
		&& Save->LatestCheckpoint.IsValid()
		&& Save->CheckpointGeneration.IsValid()
		&& Save->CheckpointGeneration == Save->SaveGeneration;
}

bool EMSCheckpoint::BeginGeneration(
	UEMSCheckpointSaveGame* Save,
	const FGuid& Generation,
	TFunctionRef<bool()> WriteToDisk)
{
	if (!Save || !Generation.IsValid() || Generation == Save->SaveGeneration)
	{
		return false;
	}

	const bool bPreviousHasCheckpoint = Save->bHasCheckpoint;
	const FEMSCheckpointRecord PreviousRecord = Save->LatestCheckpoint;
	const FGuid PreviousSaveGeneration = Save->SaveGeneration;
	const FGuid PreviousCheckpointGeneration = Save->CheckpointGeneration;

	// A record already known to be stale must never become the fallback of a new
	// checkpoint attempt. Clear it as part of the reservation transaction. If the
	// reservation write itself fails, the exact previous metadata is restored.
	if (Save->bHasCheckpoint && !IsCurrentGeneration(Save))
	{
		Save->bHasCheckpoint = false;
		Save->LatestCheckpoint = FEMSCheckpointRecord();
		Save->CheckpointGeneration.Invalidate();
	}

	Save->SaveGeneration = Generation;
	if (WriteToDisk())
	{
		return true;
	}

	Save->bHasCheckpoint = bPreviousHasCheckpoint;
	Save->LatestCheckpoint = PreviousRecord;
	Save->SaveGeneration = PreviousSaveGeneration;
	Save->CheckpointGeneration = PreviousCheckpointGeneration;
	return false;
}

bool EMSCheckpoint::CommitRecord(
	UEMSCheckpointSaveGame* Save,
	const FEMSCheckpointRecord& Record,
	const FGuid& Generation,
	TFunctionRef<bool()> WriteToDisk)
{
	// Committing a generation the slot never reserved would claim a pairing that
	// no EMS save stands behind.
	if (!Save
		|| !Record.IsValid()
		|| !Generation.IsValid()
		|| Generation != Save->SaveGeneration)
	{
		return false;
	}

	const bool bPreviousHasCheckpoint = Save->bHasCheckpoint;
	const FEMSCheckpointRecord PreviousRecord = Save->LatestCheckpoint;
	const FGuid PreviousGeneration = Save->CheckpointGeneration;
	Save->bHasCheckpoint = true;
	Save->LatestCheckpoint = Record;
	Save->CheckpointGeneration = Generation;
	if (WriteToDisk())
	{
		return true;
	}

	Save->bHasCheckpoint = bPreviousHasCheckpoint;
	Save->LatestCheckpoint = PreviousRecord;
	Save->CheckpointGeneration = PreviousGeneration;
	return false;
}

bool EMSCheckpoint::AbandonGeneration(
	UEMSCheckpointSaveGame* Save,
	TFunctionRef<bool()> WriteToDisk)
{
	if (!Save || Save->SaveGeneration == Save->CheckpointGeneration)
	{
		return Save != nullptr;
	}

	const FGuid ReservedGeneration = Save->SaveGeneration;
	Save->SaveGeneration = Save->CheckpointGeneration;
	if (WriteToDisk())
	{
		return true;
	}

	Save->SaveGeneration = ReservedGeneration;
	return false;
}

bool EMSCheckpoint::ClearCurrentCheckpoint(
	UEMSCheckpointSaveGame* Save,
	TFunctionRef<bool()> WriteToDisk)
{
	if (!Save)
	{
		return false;
	}
	if (!IsCurrentGeneration(Save))
	{
		return true;
	}

	const bool bPreviousHasCheckpoint = Save->bHasCheckpoint;
	const FEMSCheckpointRecord PreviousRecord = Save->LatestCheckpoint;
	const FGuid PreviousSaveGeneration = Save->SaveGeneration;
	const FGuid PreviousCheckpointGeneration = Save->CheckpointGeneration;

	Save->bHasCheckpoint = false;
	Save->LatestCheckpoint = FEMSCheckpointRecord();
	Save->SaveGeneration.Invalidate();
	Save->CheckpointGeneration.Invalidate();
	if (WriteToDisk())
	{
		return true;
	}

	Save->bHasCheckpoint = bPreviousHasCheckpoint;
	Save->LatestCheckpoint = PreviousRecord;
	Save->SaveGeneration = PreviousSaveGeneration;
	Save->CheckpointGeneration = PreviousCheckpointGeneration;
	return false;
}
