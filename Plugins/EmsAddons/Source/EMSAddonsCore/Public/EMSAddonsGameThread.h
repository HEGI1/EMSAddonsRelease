//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Async/TaskGraphInterfaces.h"
#include "Templates/Function.h"

namespace EMSAddons
{
	/**
	 * Runs Work on the game thread and blocks until it has finished.
	 *
	 * EMS only marshals Pre-Save to the game thread when its Pre-Save On Game
	 * Thread setting is enabled, and that setting defaults to off. Addon capture
	 * touches actors and components, so it has to be safe on its own whenever
	 * Multi-Thread Saving is on and that setting is not.
	 */
	inline void RunOnGameThread(TUniqueFunction<void()> Work)
	{
		if (IsInGameThread())
		{
			Work();
			return;
		}

		const FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
			MoveTemp(Work),
			TStatId(),
			nullptr,
			ENamedThreads::GameThread);
		if (Task.IsValid())
		{
			Task->Wait();
		}
	}
}
