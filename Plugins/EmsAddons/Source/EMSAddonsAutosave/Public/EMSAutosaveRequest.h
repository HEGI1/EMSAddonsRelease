//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAutosaveTypes.h"

/** Small deterministic queue used by the subsystem to coalesce save requests. */
struct EMSADDONSAUTOSAVE_API FEMSAutosavePendingRequest
{
	static float GetMinimumIntervalDelay(const double LastSaveSeconds, const double NowSeconds, const float MinimumInterval)
	{
		return FMath::Max(0.0, static_cast<double>(FMath::Max(0.0f, MinimumInterval)) - (NowSeconds - LastSaveSeconds));
	}

	void Enqueue(const FName InReason, const FString& InSaveSlot, const FEMSCheckpointRecord* InCheckpoint)
	{
		const bool bDifferentSlot = bPending && !SaveSlot.Equals(InSaveSlot, ESearchCase::IgnoreCase);
		if (bDifferentSlot)
		{
			Reset();
		}

		if (InCheckpoint)
		{
			Reason = InReason;
			SaveSlot = InSaveSlot;
			Checkpoint = *InCheckpoint;
			bCheckpoint = true;
			bPending = true;
		}
		else if (!bPending || !bCheckpoint)
		{
			Reason = InReason;
			SaveSlot = InSaveSlot;
			Checkpoint = FEMSCheckpointRecord();
			bCheckpoint = false;
			bPending = true;
		}
	}

	bool Consume(FName& OutReason, FString& OutSaveSlot, bool& bOutCheckpoint, FEMSCheckpointRecord& OutCheckpoint)
	{
		if (!bPending)
		{
			return false;
		}
		OutReason = Reason;
		OutSaveSlot = SaveSlot;
		bOutCheckpoint = bCheckpoint;
		OutCheckpoint = Checkpoint;
		Reset();
		return true;
	}

	void Reset()
	{
		Reason = NAME_None;
		SaveSlot.Reset();
		Checkpoint = FEMSCheckpointRecord();
		bPending = false;
		bCheckpoint = false;
	}

	bool IsPending() const { return bPending; }
	bool IsCheckpoint() const { return bCheckpoint; }
	bool IsForSlot(const FString& InSaveSlot) const
	{
		return bPending && SaveSlot.Equals(InSaveSlot, ESearchCase::IgnoreCase);
	}
	FName GetReason() const { return Reason; }
	const FString& GetSaveSlot() const { return SaveSlot; }

private:
	FName Reason;
	FString SaveSlot;
	FEMSCheckpointRecord Checkpoint;
	bool bPending = false;
	bool bCheckpoint = false;
};
