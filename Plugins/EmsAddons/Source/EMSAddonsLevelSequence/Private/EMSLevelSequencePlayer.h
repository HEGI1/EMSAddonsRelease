//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "LevelSequencePlayer.h"
#include "EMSLevelSequencePlayer.generated.h"

/** Internal player exposing the engine's actual loop progress to its owning actor. */
UCLASS(NotBlueprintable, Transient, HideDropdown)
class UEMSLevelSequencePlayer : public ULevelSequencePlayer
{
	GENERATED_BODY()

public:
	int32 GetCompletedLoopCount() const { return CurrentNumLoops; }
	/** Restores direction without PlayInternal's playback/evaluation side effects. */
	void RestorePlaybackDirection(const bool bReverse) { bReversePlayback = bReverse; }
	DECLARE_EVENT(UEMSLevelSequencePlayer, FLoopsCompleted);
	FLoopsCompleted& OnLoopsCompleted() { return LoopsCompleted; }

protected:
	virtual void OnLooped() override
	{
		Super::OnLooped();
		LoopsCompleted.Broadcast();
	}

private:
	FLoopsCompleted LoopsCompleted;
};
