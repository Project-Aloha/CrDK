/* Portable Qualcomm DWC3 sequencing. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#include <Interface/CrUsbHandoff.h>

#define CR_DWC3_MAX_WAIT_US 200000U
#define CR_DWC3_GLOBAL_OFFSET 0xc100U
#define CR_DWC3_GLOBAL_BYTES 0x610U
#define CR_DWC3_QSCRATCH_BYTES 0x34U

typedef enum { CrDwc3Host=1, CrDwc3Device=2 } CR_DWC3_ROLE;
typedef struct { UINTN Address; UINT32 Size; } CR_DWC3_WINDOW;
typedef struct {
  UINT32 Soc;
  /* Independent assigned virtual views, never physical addresses discovered
     by this library. Global.Address corresponds to DWC3 controller+0xc100. */
  CR_DWC3_WINDOW Global, Qscratch;
} CR_DWC3_REGIONS;
typedef struct {
  UINT32 Id, Revision, VersionType, EventBuffers;
  UINT32 Guctl1, Guctl3, LastValue;
  UINTN LastAddress;
  BOOLEAN HostGap, RetryDisable, Touched;
  UINT32 HwParams1; /* Raw diagnostic; NUM_INT is not a GEVNT loop bound. */
} CR_DWC3_REPORT;

/* All calls require exclusive outer serialization and live caller-owned
   mappings/clocks/domain/rails/PHY. Kernel calls require PASSIVE_LEVEL.
   Configure requires completed wrapper reset and retirement of both former
   role writers/DMA. It initializes only event ring 0, matching the supported
   function child and Linux/Android. EventBuffers counts managed rings, not
   GHWPARAMS1.NUM_INT. It neither starts a role child nor grants DMA ownership.
   Touched is set before the first write: failures then require controller
   cleanup/reset while retaining every prerequisite. QSCRATCH session-valid
   bits do not supply VBUS. SM8450 PHY/UTMI/host-erratum policy is explicit.
   Halt requires the active role driver's exclusion; Windows inbox xHCI/UFX
   owns its running controller and must finish its stop before platform reset. */
CR_STATUS CrDwc3Configure(CONST CR_DWC3_REGIONS *, CR_DWC3_ROLE,
    BOOLEAN SuperSpeed, CR_DWC3_REPORT *);
CR_STATUS CrDwc3Halt(CR_DWC3_ROLE, CONST CR_DWC3_WINDOW *, UINT32 *LastValue);
/* One-way controller retirement after every former software writer is frozen
   and drained. Preserves ring addresses/allocations and all prerequisites.
   Only a function child using event ring 0 is supported; other event-ring
   owners must retire their own rings before calling. Host retirement checks
   xHCI Run/INTE/Halted and also masks the retained function ring 0.
   Checks halt, interrupt disable and actual role, returning raw observations
   even on failure. Global must cover DALEPENA (0x624 bytes). No SMMU proof or
   OS ownership is implied. Outputs are distinct caller-owned storage. */
CR_STATUS CrDwc3Retire(CONST CR_DWC3_REGIONS *, CONST CR_DWC3_WINDOW *Xhci,
    CR_DWC3_ROLE, CR_USB_CONTROLLER_STOP *, CR_STATUS *HaltStatus,
    CR_STATUS *InterruptStatus);
/* Halt's window is xHCI capabilities for Host, Global for Device. */
CR_STATUS CrDwc3Wait32(CONST CR_DWC3_WINDOW *, UINT32 Offset, UINT32 Mask,
    UINT32 Expected, UINT32 TimeoutUs, UINT32 *LastValue);
