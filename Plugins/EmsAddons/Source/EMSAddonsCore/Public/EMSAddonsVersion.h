//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "EMSTypes.h"

/**
 * Easy Multi Save Addons always requires the latest Easy Multi Save release.
 * The value uses EMS's own EMS_VERSION_NUMBER scheme: 187 is Easy Multi Save 1.87.
 * Every addon module that uses EMS includes this header, so building against an
 * older EMS stops with the message below instead of unrelated API errors.
 */
#define EMSADDONS_REQUIRED_EMS_VERSION 187

static_assert(EMS_VERSION_NUMBER >= EMSADDONS_REQUIRED_EMS_VERSION,
	"Easy Multi Save Addons requires Easy Multi Save 1.87. Install the latest Easy Multi Save into Plugins/EasyMultiSave.");
