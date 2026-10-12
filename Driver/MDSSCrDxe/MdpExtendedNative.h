/* Native extended display provider composition. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "MdpExtendedPanel.h"
#include "MdpNativeResources.h"
#include <Library/MdssDualCrLib.h>
#include <Library/CrTargetMdssLib.h>
#include <Protocol/EFIClockExternalCrProtocol.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/IoMmu.h>
#include <Pi/PiDxeCis.h>
typedef MDP_NATIVE_PROVIDERS MDP_EXTENDED_PROVIDERS;
typedef struct {
  CONST CR_MDSS_TARGET *Target;
  UINT64 MdpHz, AverageKBps[2], PeakKBps[2];
  /* Exact board panel owner. Ready/Acquire/Release/Reset are mandatory;
     Delay/cache publication are supplied by this native adapter. Optional
     PublishPixels is an actual post-reset bridge prepare callback invoked
     before timing enable. No XML pin inference. */
  CR_MDSS_PANEL_IO Panel;
} MDP_EXTENDED_NATIVE_CONFIG;
typedef struct {
  MDP_NATIVE_RESOURCES Resources;
  CONST CR_MDSS_HW_PROFILE *Hardware;
  MDP_EXTENDED_PROVIDERS Providers;
  MDP_EXTENDED_NATIVE_CONFIG Config;
  CR_MDSS_EXT_DISPLAY Display;
  CR_MDSS_DSI_PANEL Panel;
  CR_MDSS_DSI_PLAN Plan;
  VOID *FrameCpu, *FrameMap, *CommandCpu, *CommandMap;
  UINTN FrameBytes;
  UINT64 FrameIova, CommandIova, Incarnation;
  INTERCONNECT_PATH_HANDLE Paths[2];
  BOOLEAN RailOwned[2], ClockOwned[7], ExternalOwned, PanelLeaseOwned;
  BOOLEAN Initialized, AccessReady, Closing, Frozen, FreezeAccess;
  BOOLEAN FreezeComplete;
  BOOLEAN OutputPanelAcquired;
  EFI_STATUS FreezeStatus;
} MDP_EXTENDED_NATIVE;
/* Stable zeroed storage and external caller serialization. Explicit profile
   payload/reset arrays, panel owner and reserved framebuffer stay alive through
   exact successful Close. Providers are actual installed native protocols;
   no identity DMA address or inherited hardware adoption is accepted.
   Open may retain Initialized on failure: retry Close before disposing it.
   This does not publish a GOP or register a native EBS handoff participant. */
EFI_STATUS MdpExtendedNativeOpen(MDP_EXTENDED_NATIVE *, CONST MDP_EXTENDED_PROVIDERS *,
    CONST MDP_EXTENDED_NATIVE_CONFIG *, CONST CR_MDSS_PANEL_PROFILE *,
    EFI_PHYSICAL_ADDRESS FramePhysical, UINTN FrameBytes);
EFI_STATUS MdpExtendedNativeStart(MDP_EXTENDED_NATIVE *);
EFI_STATUS MdpExtendedNativeClose(MDP_EXTENDED_NATIVE *);
/* Called only after the output owner has closed and drained its public gate.
   At <=TPL_NOTIFY, stops DPU and transient host DMA without rail/panel RPC,
   unmap, free, clock retirement, or publication changes. All resources remain
   retained through EBS. A failure remains retryable only through Freeze;
   ordinary Start/Close decline once Frozen. No bonded MDSH grant is inferred. */
EFI_STATUS MdpExtendedNativeFreeze(MDP_EXTENDED_NATIVE *);
