//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogEMSAddonsWorldPartition, Log, All);

class FEMSAddonsWorldPartitionModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

#if WITH_EDITOR
private:
	FDelegateHandle PropertyChangedHandle;
	FDelegateHandle PreSaveHandle;
	FDelegateHandle TransactedHandle;
	void EnforcePlacement(UObject* Object);
	void HandleObjectPropertyChanged(UObject* Object, struct FPropertyChangedEvent& Event);
#endif
};
