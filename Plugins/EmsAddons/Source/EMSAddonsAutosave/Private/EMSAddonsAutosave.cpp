//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAddonsAutosave.h"

#include "EMSAutosaveMapTravel.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogEMSAddonsAutosave);

class FEMSAddonsAutosaveModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMapWithContext.AddStatic(
			&EMSAutosaveMapTravel::HandlePreLoadMap);
	}

	virtual void ShutdownModule() override
	{
		if (PreLoadMapHandle.IsValid())
		{
			FCoreUObjectDelegates::PreLoadMapWithContext.Remove(PreLoadMapHandle);
			PreLoadMapHandle.Reset();
		}
	}

private:
	FDelegateHandle PreLoadMapHandle;
};

IMPLEMENT_MODULE(FEMSAddonsAutosaveModule, EMSAddonsAutosave)
