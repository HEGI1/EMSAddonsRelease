//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAutosaveSubsystem.h"

#include "EMSAutosaveSettings.h"
#include "Engine/World.h"
#include "TimerManager.h"

float UEMSAutosaveSubsystem::ResolvePeriodicAutosaveInterval() const
{
	if (PeriodicIntervalOverride >= 0.0f)
	{
		return PeriodicIntervalOverride;
	}

	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	return Settings ? FMath::Max(0.0f, Settings->PeriodicAutosaveInterval) : 0.0f;
}

void UEMSAutosaveSubsystem::SetPeriodicAutosaveInterval(const float IntervalSeconds)
{
	if (bIsShuttingDown)
	{
		return;
	}

	// The override lives on this game-instance subsystem, not on the settings object.
	// Project Settings and its config file are never touched, and the value still
	// survives map travel for the rest of this game session.
	PeriodicIntervalOverride = FMath::Max(0.0f, IntervalSeconds);

	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || World->bIsTearingDown)
	{
		return;
	}

	const UEMSAutosaveSettings* Settings = GetDefault<UEMSAutosaveSettings>();
	World->GetTimerManager().ClearTimer(PeriodicTimer);
	if (Settings && Settings->bEnableAutosave && PeriodicIntervalOverride > 0.0f)
	{
		World->GetTimerManager().SetTimer(
			PeriodicTimer,
			this,
			&UEMSAutosaveSubsystem::HandlePeriodicAutosave,
			PeriodicIntervalOverride,
			true);
	}
}

float UEMSAutosaveSubsystem::GetPeriodicAutosaveInterval() const
{
	return ResolvePeriodicAutosaveInterval();
}
