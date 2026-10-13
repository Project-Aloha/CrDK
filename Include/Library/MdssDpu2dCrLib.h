/* SM8450 linear DMA0 fetch reflections. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssCrLib.h>
#define CR_MDSS_DPU_REFLECT_X 1U
#define CR_MDSS_DPU_REFLECT_Y 2U
#define CR_MDSS_DPU_REFLECT_MASK 3U
/* Caller serializes with full-pipeline Start/Stop and retains the actual
   native/DMA/power/clock leases until Retire succeeds after the real pipe
   has stopped. Ready must requalify those leases before every operation.
   This API changes only DMA0 fetch orientation. It performs no DMA writes,
   memory blits, scaling, 90-degree rotation or compressed-format conversion. */
typedef struct {
 VOID *Context;
 UINT64 Physical;
 UINTN Bytes;
 CR_STATUS (*Ready)(VOID *);
 CR_STATUS (*Read32)(VOID *,UINT32 Offset,UINT32 *Value);
 CR_STATUS (*Write32)(VOID *,UINT32 Offset,UINT32 Value);
 CR_STATUS (*Fence)(VOID *);
 CR_STATUS (*DelayUs)(VOID *,UINT32);
} CR_MDSS_DPU_FETCH_IO;
typedef struct {
 CR_MDSS_DPU_FETCH_IO Io;
 CR_MDSS_MODE Mode;
 UINT64 DeviceAddress;
 UINT32 Transform;
 BOOLEAN Initialized,Confirmed,Touched,CleanupPending;
} CR_MDSS_DPU_FETCH;
/* Pure preflight; caller supplies zeroed storage or successfully retired state.
   A live/uncertain journal cannot be initialized over. Exact SM8450
   DMA0/CTL0/DSI0 linear BGRX, one source rectangle.
   DeviceAddress is supplied by the independent DMA provider, never physical. */
CR_STATUS CrMdssDpuFetchInit(CR_MDSS_DPU_FETCH *,CONST CR_MDSS_DPU_FETCH_IO *,
 CONST CR_MDSS_MODE *,UINT64 DeviceAddress,UINTN BufferBytes);
/* Verify the active source, format, size, route and orientation without writes.
   Out remains unchanged on any failure. */
CR_STATUS CrMdssDpuFetchQuery(CR_MDSS_DPU_FETCH *,UINT32 *Transform);
/* RMW preserves other source controls, source flush + observed frame progress
   qualifies completion. Any failure after a write retains CleanupPending;
   only full-pipeline Stop + Retire can reopen that journal. */
CR_STATUS CrMdssDpuFetchSet(CR_MDSS_DPU_FETCH *,UINT32 Transform);
/* Observe actual INTF enable/status off before retiring metadata. No mutation,
   no resource release. Refuse while scanout remains live, including timeout. */
CR_STATUS CrMdssDpuFetchRetire(CR_MDSS_DPU_FETCH *);

/* CPU backing-store transition between identity and 180-degree fetch.
   Swaps visible BGRX pixels in place, preserving stride padding. Caller owns
   the sole current CPU alias and drains concurrent crash writers first.
   This is a transition helper, not advertised as DPU memory-copy hardware. */
CR_STATUS CrMdssDpuReversePixels(VOID *,CONST CR_MDSS_MODE *,UINTN Bytes);
