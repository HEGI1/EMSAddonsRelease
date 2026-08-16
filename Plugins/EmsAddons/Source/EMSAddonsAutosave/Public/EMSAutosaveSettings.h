//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EMSTypes.h"
#include "EMSAutosaveSettings.generated.h"

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

	virtual FName GetCategoryName() const override { return TEXT("Easy Multi Save Addons"); }
};
