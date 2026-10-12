/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
#include <Library/clock.h>
#include <oskal/cr_assert.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_interrupt.h>
#include <oskal/cr_status.h>
#include <oskal/cr_string.h>
#include <oskal/cr_types.h>

CR_STATUS ClockRecoverConfigurationLocked(ClockDriverContext *Context,ClockNode *Node);
CR_STATUS ClockRestoreRcgSnapshotLocked(ClockDriverContext *Context,ClockNode *Node);

/* Caller holds Context->Lock across selection and acquisition. */
CR_STATUS ClockSelectExternalSourceLocked(ClockDriverContext *Context,ClockNode *Node,
  UINT32 Source,UINT32 Divider,UINT32 M,UINT32 N,UINT32 TwiceD,BOOLEAN RequireUnowned);

/* Internal, caller holds Context->Lock; no public provider API change. */
ClockNode *ClockConfigSource(ClockNode *Node);
CR_STATUS ClockRestoreMuxSnapshotLocked(ClockDriverContext *Context,ClockNode *Node);

STATIC inline BOOLEAN ClockHasConditionalParents(CONST ClockNode *Node)
{
  return (Node->Type==CLOCK_NODE_TYPE_BRANCH ||
          Node->Type==CLOCK_NODE_TYPE_BRANCH_2) && Node->ParentMinimumRates!=NULL;
}

/* Immutable graph descriptors remain valid until the final cleanup succeeds. */
STATIC inline CR_STATUS ClockParentDescription(CONST ClockNode *Node)
{
  if (Node->ParentCount>32 || (Node->ParentCount && Node->Parents==NULL))
    return CR_INVALID_PARAMETER;
  if (Node->ParentRates &&
      (((UINTN)Node->ParentRates&(sizeof(UINT64)-1U)) ||
       (UINTN)Node->ParentRates>(UINTN)-1-Node->ParentCount*sizeof(UINT64)))
    return CR_INVALID_PARAMETER;
  if (ClockHasConditionalParents(Node) &&
      (((UINTN)Node->ParentMinimumRates&(sizeof(UINT64)-1U)) ||
       (UINTN)Node->ParentMinimumRates>(UINTN)-1-Node->ParentCount*sizeof(UINT64)))
    return CR_INVALID_PARAMETER;
  for (UINTN I=0;I<Node->ParentCount;++I)
    if (Node->Parents[I]==NULL) return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}

STATIC inline BOOLEAN ClockNeedsParent(CONST ClockNode *Node,UINTN Index,UINT64 RateHz)
{
  return !ClockHasConditionalParents(Node) || RateHz>=Node->ParentMinimumRates[Index];
}

STATIC inline UINT64 ClockParentRate(CONST ClockNode *Node,UINTN Index,UINT64 RateHz)
{
  if (Node->ParentRates && Node->ParentRates[Index]) return Node->ParentRates[Index];
  return Node->Parents[Index]->Type==CLOCK_NODE_TYPE_FIXED_PLL ?
    Node->Parents[Index]->FixedPll.Description.FrequencyHz : RateHz;
}
