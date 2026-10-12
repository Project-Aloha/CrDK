/* Immutable SM8450 DSI PHY-to-DISPCC clock contract. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_CLOCK_DSI_PLAN_REVISION 1U
typedef struct {
  UINT32 Revision, Controller;
  UINT64 PixelHz, ByteHz, PixelSourceHz;
  UINT32 PixelM, PixelN, ByteInterfaceDivider, Reserved;
} CR_CLOCK_DSI_PLAN;
/* Pure qualification only. PHY programming/readiness remains the caller's
   independently owned prerequisite. PixelSourceHz is the actual PHY pclk
   parent before DISPCC M/N; no HID division is requested. */
CR_STATUS CrClockDsiPlanValidate(CONST CR_CLOCK_DSI_PLAN *);
BOOLEAN CrClockDsiPlanEqual(CONST CR_CLOCK_DSI_PLAN *,CONST CR_CLOCK_DSI_PLAN *);
