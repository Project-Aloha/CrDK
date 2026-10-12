/** @file
  HDK8450 MDSS display resources. SPDX-License-Identifier: GPL-2.0-only
**/
#ifndef MDP_RESOURCES_H_
#define MDP_RESOURCES_H_

#include <Uefi.h>
#include <Protocol/EFIMdpDsiProtocol.h>
#include <Library/MdssHandoffCrLib.h>

EFI_STATUS
MdpInitializeResources (IN CONST EFI_MDP_DSI_TIMING *Timing);

/** Selected host remains queryable during retained failure cleanup; final
    resource release invalidates it. Does not imply DMA is safe to restart. */
EFI_STATUS
MdpGetActiveController (OUT UINT32 *Controller);

/** Host CLKOUT_TIMING_CTRL encoding from the initialized DPHY v4 timings.
    Valid only while resources are ready. v4 does not use clk_pre_inc_by_2. */
EFI_STATUS
MdpGetHostClockTiming (OUT UINT32 *Value);

/** Map the driver's reserved framebuffer read-only for the SM8450 MDSS SIDs.
    The returned IOVA is for DPU registers; GOP must retain the physical address.
    Call before enabling scanout, after MdpInitializeResources(). */
EFI_STATUS
MdpMapFrameBuffer (
  IN EFI_PHYSICAL_ADDRESS Physical,
  IN UINTN Size,
  OUT EFI_PHYSICAL_ADDRESS *DeviceAddress
  );

/** Acquire one owned page of common-buffer storage for DSI command DMA.
    CPU may format/reuse packets after mapping; caller cleans caches before DMA.
    Device access is read-only and the returned page-aligned IOVA is below 4G.
    A second acquire fails until release; failed cleanup forbids reuse. */
EFI_STATUS
MdpAcquireCommandBuffer (OUT VOID **Cpu, OUT EFI_PHYSICAL_ADDRESS *Device,
                         OUT UINTN *Capacity);

/** Caller must confirm command DMA idle. Failure retains ownership for retry. */
EFI_STATUS
MdpReleaseCommandBuffer (VOID);

/** Caller must stop DPU/DSI DMA first. Failed TLB invalidation pins resources. */
EFI_STATUS
MdpReleaseResources (VOID);

/** Data-only retained ownership snapshot, after the display's admission gate
    is closed. Returns NOT_READY when any resource journal is uncertain. */
EFI_STATUS MdpSnapshotResources (CR_MDSS_HANDOFF_PARTICIPANT *Snapshot, VOID **Map);

#endif
