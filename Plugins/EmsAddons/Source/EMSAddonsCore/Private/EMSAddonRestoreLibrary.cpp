//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAddonRestoreLibrary.h"

#include "EMSAddonRestoreParticipant.h"

bool UEMSAddonRestoreLibrary::IsEMSAddonRestoreComplete(const UObject* Target)
{
	const IEMSAddonRestoreParticipant* Participant =
		Cast<IEMSAddonRestoreParticipant>(Target);
	return Participant ? Participant->IsEMSAddonRestoreComplete() : true;
}

FEMSAddonResult UEMSAddonRestoreLibrary::GetEMSAddonRestoreResult(
	const UObject* Target)
{
	const IEMSAddonRestoreParticipant* Participant =
		Cast<IEMSAddonRestoreParticipant>(Target);
	return Participant
		? Participant->GetEMSAddonRestoreResult()
		: FEMSAddonResult::Success();
}
