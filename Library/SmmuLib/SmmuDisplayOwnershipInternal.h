/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/SmmuDisplayOwnershipCrLib.h>
#include <oskal/cr_string.h>
#define BSBIT(n) (1U<<(n))
#define BST(x) do { CR_STATUS BsStatus=(x); if(BsStatus!=CR_SUCCESS)return BsStatus; } while(0)
CR_STATUS BsReady(CR_SMMU_DISPLAY_GROUP_CORE *);
CR_STATUS BsRead(CR_SMMU_DISPLAY_GROUP_CORE *,UINT32,UINT32 *);
CR_STATUS BsWrite(CR_SMMU_DISPLAY_GROUP_CORE *,UINT32,UINT32);
CR_STATUS BsFence(CR_SMMU_DISPLAY_GROUP_CORE *);
CR_STATUS BsDelay(CR_SMMU_DISPLAY_GROUP_CORE *,UINT32);
CR_STATUS BsChecked(CR_SMMU_DISPLAY_GROUP_CORE *,UINT32,UINT32,UINT32);
/* Inventory owner implements these pure operations. Capture clears Out and
 * uses bounded real Device geometry. Admit validates source/capability/global
 * policy, derives exact stream/bank/ASID sets and rejects foreign aliases.
 * During retained retries, AllowOwnedReservations only exempts precisely the
 * Before-owned indices/ASIDs, never all reserved resources. SameOwned permits
 * unrelated clients to change only when full re-admission still excludes
 * sharing; it compares all globals and exact owned stable registers. */
CR_STATUS BsCapture(CR_SMMU_DISPLAY_GROUP_CORE *,CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *Out);
CR_STATUS BsAdmit(CR_SMMU_DISPLAY_GROUP_CORE *,CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *,BOOLEAN AllowOwnedReservations);
CR_STATUS BsSameOwned(CONST CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *,CONST CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *);
