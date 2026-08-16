//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameTime.h"
#include "UObject/SoftObjectPath.h"
#include "EMSLevelSequenceTypes.generated.h"

UENUM()
enum class EEMSLevelSequencePlaybackStatus : uint8
{
	Stopped,
	Paused,
	Playing,
	Finished
};

UENUM(BlueprintType)
enum class EEMSLevelSequencePositionRestorePolicy : uint8
{
	JumpAndEvaluate,
	TraverseAndEvaluate
};

USTRUCT()
struct EMSADDONSLEVELSEQUENCE_API FEMSLevelSequencePlaybackState
{
	GENERATED_BODY()

	/** 2: Position and tick resolution are stored as scalars. See below. */
	static constexpr int32 CurrentVersion = 2;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	int32 Version = CurrentVersion;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	bool bHasValidData = false;

	/**
	 * The playback time, split into the scalars FFrameTime is made of.
	 *
	 * EMS serializes with ArIsSaveGame, which drops every property that is not
	 * flagged SaveGame, including the members of a nested struct. The engine
	 * handles that for FTransform and FVector by flagging their members, but
	 * FFrameTime, FFrameRate and FFrameNumber flag none of theirs and register
	 * no native struct serializer either. Stored directly they only survived a
	 * save while the EMS 'Auto Save Structs' setting was enabled, because that
	 * forces the flag on at runtime. Scalars we can flag ourselves are saved
	 * either way.
	 */
	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	int32 PositionFrame = 0;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	float PositionSubFrame = 0.0f;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	int32 TickResolutionNumerator = 0;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	int32 TickResolutionDenominator = 0;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	EEMSLevelSequencePlaybackStatus PlaybackStatus =
		EEMSLevelSequencePlaybackStatus::Stopped;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	float PlayRate = 1.0f;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	bool bReversePlayback = false;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	bool bHasStarted = false;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	int32 CompletedLoops = 0;

	UPROPERTY(SaveGame, VisibleAnywhere, Category = "EMS Addons|Level Sequence")
	FSoftObjectPath SequenceAsset;

	/**
	 * Whether the saved scalars describe a usable time.
	 *
	 * A zero numerator is rejected as well, which FFrameRate::IsValid does not
	 * do. It is never produced by a capture and would transform every saved
	 * position to zero.
	 */
	bool HasValidTimeScalars() const
	{
		return TickResolutionNumerator > 0
			&& TickResolutionDenominator > 0
			&& FMath::IsFinite(PositionSubFrame)
			&& PositionSubFrame >= 0.0f
			&& PositionSubFrame < 1.0f;
	}

	/** Only meaningful once HasValidTimeScalars has passed. */
	FFrameTime GetPosition() const
	{
		return FFrameTime(FFrameNumber(PositionFrame), PositionSubFrame);
	}

	FFrameRate GetTickResolution() const
	{
		return FFrameRate(TickResolutionNumerator, TickResolutionDenominator);
	}

	void SetPosition(const FFrameTime InPosition)
	{
		PositionFrame = InPosition.GetFrame().Value;
		PositionSubFrame = InPosition.GetSubFrame();
	}

	void SetTickResolution(const FFrameRate InTickResolution)
	{
		TickResolutionNumerator = InTickResolution.Numerator;
		TickResolutionDenominator = InTickResolution.Denominator;
	}

	void Reset()
	{
		*this = FEMSLevelSequencePlaybackState();
	}
};
