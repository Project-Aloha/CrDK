/* Immutable display resources supplied by a target, usable by any OS adapter.
 * These identities do not grant access, clocks, DMA or inherited ownership.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_MDSS_TARGET_REVISION 1U
#define CR_MDSS_TARGET_CLOCKS 7U
#define CR_MDSS_TARGET_RAILS 2U
#define CR_MDSS_TARGET_PATHS 2U
#define CR_MDSS_TARGET_STREAMS 4U
typedef struct {
  CONST CHAR8 *Name;
  UINT32 Millivolts;
  UINT8 Mode;
} CR_MDSS_TARGET_RAIL;
typedef struct {
  CONST CHAR8 *Provider;
  UINT32 Source, Destination;
} CR_MDSS_TARGET_PATH;
typedef struct {
  CONST CHAR8 *Controller, *Name;
} CR_MDSS_TARGET_CLOCK;
typedef struct {
  UINT32 Revision, Size, Soc;
  UINT64 TopBase, VbifBase, SmmuBase;
  UINT32 TopBytes, VbifBytes, Interrupt;
  UINT16 Streams[CR_MDSS_TARGET_STREAMS];
  /* A shared inherited SMR/context bank must be retired as one qualified
     transaction. Ordinary per-SID Attach is not a substitute for that grant. */
  BOOLEAN RequiresStoppedDisplay;
  CR_MDSS_TARGET_RAIL Rails[CR_MDSS_TARGET_RAILS];
  CR_MDSS_TARGET_PATH Paths[CR_MDSS_TARGET_PATHS];
  CR_MDSS_TARGET_CLOCK Clocks[CR_MDSS_TARGET_CLOCKS];
} CR_MDSS_TARGET;
CONST CR_MDSS_TARGET *CrTargetGetMdssContext(VOID);
