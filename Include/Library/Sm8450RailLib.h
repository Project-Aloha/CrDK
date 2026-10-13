/* Qualified HDK8450 supply inventory. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/RpmhRailLib.h>
#define CR_SM8450_HDK_RAIL_COUNT 16U
/* Caller supplies empty nonpaged storage, retained through all leases.
   This builds private parent pointers only, without addresses, IO or votes.
   CmdDB addresses must be cached before absolute owner qualification. */
CR_STATUS CrSm8450HdkRailsInit(CR_RPMH_RAIL *, UINT32 Count);
