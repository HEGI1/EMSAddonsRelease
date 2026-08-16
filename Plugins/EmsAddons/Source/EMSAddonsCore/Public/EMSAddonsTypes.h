//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAddonsTypes.generated.h"

UENUM(BlueprintType)
enum class EEMSAddonResultCode : uint8
{
	Success,
	Pending,
	Skipped,
	RestoreFailed,
	Canceled
};

/**
 * Shared restore lifecycle, used by every addon that restores asynchronously.
 *
 * Deliberately only the states callers can act on. What a restore happens to be
 * waiting for is diagnostic detail and belongs in the log line, not in a state
 * machine that each module would otherwise re-model with its own enum.
 */
UENUM()
enum class EEMSAddonRestorePhase : uint8
{
	Idle,
	Pending,
	Complete,
	Failed,
	Canceled
};

USTRUCT(BlueprintType)
struct EMSADDONSCORE_API FEMSAddonResult
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons")
	EEMSAddonResultCode Code = EEMSAddonResultCode::Success;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "EMS Addons")
	FText Message;

	FEMSAddonResult() = default;

	FEMSAddonResult(const EEMSAddonResultCode InCode, const FText& InMessage)
		: Code(InCode)
		, Message(InMessage)
	{
	}

	bool IsSuccess() const
	{
		return Code == EEMSAddonResultCode::Success;
	}

	static FEMSAddonResult Success()
	{
		return FEMSAddonResult(EEMSAddonResultCode::Success, FText::GetEmpty());
	}
};
