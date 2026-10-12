/* Internal SM8350 storage target data. SPDX-License-Identifier: MIT */
#pragma once
#include <CrDalDevice.h>
CONST CR_DAL_RPMH_ARC_INVENTORY *CrSm8350GetArcInventory(VOID);
CONST CR_DAL_RPMH_RAILS *CrSm8350GetStorageRails(VOID);
CONST CR_DAL_RPMH_TOPOLOGY *CrSm8350GetStorageTopology(VOID);
