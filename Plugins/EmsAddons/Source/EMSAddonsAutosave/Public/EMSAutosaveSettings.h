//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EMSTypes.h"
#include "EMSAutosaveSettings.generated.h"

class UWorld;

/** Project-wide configuration for the EMS Addons checkpoint and autosave helper. */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Autosave and Checkpoints"))
class EMSADDONSAUTOSAVE_API UEMSAutosaveSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UEMSAutosaveSettings();

	/**
	 * Enables the general autosave helper, including Request Autosave and automatic triggers.
	 * Checkpoints always save when activated; normal EMS save/load calls are independent as well.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "General")
	bool bEnableAutosave = true;

	/**
	 * EMS data groups saved and loaded by the checkpoint/autosave helper.
	 * This only controls operations started by this addon; it does not change EMS project settings.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "General", meta = (Bitmask, BitmaskEnum = "/Script/EasyMultiSave.ESaveTypeFlags"))
	int32 SaveDataFlags = static_cast<int32>(ESaveTypeFlags::SF_Player) | static_cast<int32>(ESaveTypeFlags::SF_Level);

	/**
	 * Maps on which the addon never saves, such as the main menu, front-end and loading maps.
	 * EMS keeps the last played slot current, so a save on such a map would write that map into the player's profile.
	 * Request Autosave, the automatic triggers (Autosave When Leaving Map checks the map being left),
	 * Manual Save and checkpoint activation are refused there. Load Save and Load Last Checkpoint still work.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "General", meta = (AllowedClasses = "/Script/Engine.World"))
	TArray<TSoftObjectPtr<UWorld>> ExcludedMaps;

	/** True when World is one of the Excluded Maps. PIE worlds match their editor map. */
	bool IsMapExcluded(const UWorld* World) const;

	/**
	 * Minimum number of seconds between successful addon autosaves.
	 * Requests made sooner are kept pending and started when the interval expires. Zero disables throttling.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Save Timing", meta = (ClampMin = "0.0", Units = "s"))
	float MinimumTimeBetweenSaves = 5.0f;

	/**
	 * Automatically requests an autosave at this interval in seconds.
	 * Zero disables periodic autosaves. The interval can also be changed at runtime from Blueprint.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Automatic Triggers", meta = (ClampMin = "0.0", Units = "s"))
	float PeriodicAutosaveInterval = 0.0f;

	/** Requests an addon autosave shortly after a normal map load completes. */
	UPROPERTY(Config, EditAnywhere, Category = "Automatic Triggers")
	bool bAutosaveAfterMapLoad = false;

	/** Delay in seconds before the optional post-map-load autosave request is issued. */
	UPROPERTY(Config, EditAnywhere, Category = "Automatic Triggers", meta = (EditCondition = "bAutosaveAfterMapLoad", ClampMin = "0.0", Units = "s"))
	float MapLoadAutosaveDelay = 1.0f;

	/**
	 * Saves the configured EMS data synchronously immediately before normal map travel begins.
	 * Off by default because the save can briefly hitch while the current map is still valid.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Automatic Triggers")
	bool bAutosaveWhenLeavingMap = false;

	/** Upper limit for Autosave Slot Count. */
	static constexpr int32 MaxAutosaveSlots = 10;

	/**
	 * Number of rotating autosave slots per profile.
	 * Zero: autosaves write the profile slot itself ("<Profile>"), as in 1.1.
	 * N: autosaves write lowercase "<profile>_autosave_0" to "<profile>_autosave_<N-1>", replacing the oldest.
	 * Every autosave makes its slot the current EMS slot, so a normal EMS load continues from the newest save.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Save Slots", meta = (ClampMin = "0", ClampMax = "10", UIMin = "0", UIMax = "10"))
	int32 AutosaveSlotCount = 0;

	/**
	 * Slot-bound EMS custom save files (their File Name) that every addon save carries from the previous slot
	 * into the slot it writes, so game data kept in custom saves stays with the progress.
	 * Can be extended at runtime with Add Carried Custom File.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Save Slots")
	TArray<FString> CarriedCustomFiles;

	virtual FName GetCategoryName() const override { return TEXT("Easy Multi Save Addons"); }
};
