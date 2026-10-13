/** @file
  Portable SM8450 DPU8.1/DSI6G display ownership and linear BGRX scanout.
  Register sequences follow Linux msm DPU/DSI and the native Crane driver.
  SPDX-License-Identifier: GPL-2.0-only
**/
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_MDSS_SOC_SM8450 0x8450U
#define CR_MDSS_MAX_RECTS 256U
#define CR_MDSS_VIDEO_LP_MASK 0x01119000U
typedef enum { CrMdssDpu, CrMdssDsi } CR_MDSS_WINDOW;
typedef struct {
  UINT32 Soc, Controller, Width, Height, Stride;
  UINT32 HSync, HBackPorch, HFrontPorch;
  UINT32 VSync, VBackPorch, VFrontPorch;
  UINT32 RefreshQ16, Lanes, VirtualChannel, TrafficMode, VideoFlags;
  UINT32 BorderColor;
  BOOLEAN InvertHsync, InvertVsync, HsaHseAfterVsVe, DisableEot;
} CR_MDSS_MODE;
typedef struct {
  INT32 Left, Top, Right, Bottom;
} CR_MDSS_RECT;
typedef struct {
  CR_MDSS_RECT Destination;
  INT32 SourceX, SourceY;
} CR_MDSS_MOVE;
typedef struct {
  VOID *Context;
  UINT32 (*Read32)(VOID *, CR_MDSS_WINDOW, UINT32);
  VOID (*Write32)(VOID *, CR_MDSS_WINDOW, UINT32, UINT32);
  VOID (*Fence)(VOID *);
  VOID (*DelayUs)(VOID *, UINT32);
  /* Acquire transfers exclusive pipe/DSI ownership and supplies a qualified
     DMA address and PHY clock timing. Owned must also be set on a partial
     acquisition error; Release must then remain callable until it succeeds.
     CPU physical address must already denote caller-owned reserved memory.
     No raw SMMU translation may be inferred from its physical address. */
  CR_STATUS (*Acquire)(VOID *, CONST CR_MDSS_MODE *, UINT64 CpuPhysical,
                       UINTN Bytes, UINT64 *DeviceAddress,
                       UINT32 *HostClockTiming, BOOLEAN *Owned);
  /* Called after the DSI command host is configured and before video starts.
     The provider owns PHY/PLL, bridge and panel commands, not DPU scanout.
     A failed Prepare still requires Unprepare. The provider must retain its
     command progress so Unprepare retries do not replay completed commands. */
  CR_STATUS (*PreparePanel)(VOID *, CONST CR_MDSS_MODE *);
  CR_STATUS (*UnpreparePanel)(VOID *, CONST CR_MDSS_MODE *);
  CR_STATUS (*Release)(VOID *);
  VOID (*PublishCpuWrites)(VOID *, VOID *, UINTN);
  /* Optional atomic ownership transfer of a running pipe to a platform
     managed POST owner. Success retains rails, DMA and reserved framebuffer
     in that owner; it must not retain this caller context. Failure changes
     no ownership. The platform must keep the POST lease across dereference
     of the caller's provider interface and a later adapter removal. */
  CR_STATUS (*TransferToPost)(VOID *);
  /* Optional complete checked IO group. Supply all four or none. The core
     accepts only exact synchronous success, suppresses later operations in
     the same attempt after an error, and retains touched cleanup phases.
     Providers with revocable borrowed mappings must supply this group;
     legacy callbacks remain for source-compatible non-revocable callers. */
  CR_STATUS (*Read32Checked)(VOID *, CR_MDSS_WINDOW, UINT32, UINT32 *);
  CR_STATUS (*Write32Checked)(VOID *, CR_MDSS_WINDOW, UINT32, UINT32);
  CR_STATUS (*FenceChecked)(VOID *);
  CR_STATUS (*DelayUsChecked)(VOID *, UINT32);
  /* Optional complete pipeline delegation. Revision must be exactly1 with
     both callbacks, or zero with neither. The permanent provider owns the
     real extended DPU/DSI/PHY/panel/native/DMA phases. Start and Stop preserve
     Owned on errors and accept only exact synchronous completion. The core
     never executes legacy MMIO/panel/resource callbacks in this mode; CPU
     present/black and durable POST remain common. */
  UINT32 PipelineRevision;
  CR_STATUS (*StartPipeline)(VOID *,CONST CR_MDSS_MODE *,UINT64 CpuPhysical,
      UINTN Bytes,UINT64 *DeviceAddress,BOOLEAN *Owned);
  CR_STATUS (*StopPipeline)(VOID *,BOOLEAN *Owned);
} CR_MDSS_IO;
typedef struct {
  CR_MDSS_IO Io;
  CR_MDSS_MODE Mode;
  VOID *FrameBuffer;
  UINTN BufferBytes;
  UINT64 CpuPhysical, DeviceAddress;
  UINT32 HostClockTiming;
  CR_STATUS IoStatus;
  BOOLEAN Initialized, ResourcesOwned, HostTouched, PanelAttempted;
  BOOLEAN ScanoutTouched, Active, CleanupPending;
} CR_MDSS;

/* External serialization covers every call and IO callback. Pure validation
   never acquires resources, maps memory or reads registers. */
CR_STATUS CrMdssValidateMode(CONST CR_MDSS_MODE *Mode, UINTN BufferBytes);
CR_STATUS CrMdssGetFetchStart(CONST CR_MDSS_MODE *Mode, UINT32 *FetchStart);
CR_STATUS CrMdssInit(CR_MDSS *Display, CONST CR_MDSS_IO *Io,
                     CONST CR_MDSS_MODE *Mode, VOID *CpuFrameBuffer,
                     UINT64 CpuPhysical, UINTN BufferBytes);
CR_STATUS CrMdssStart(CR_MDSS *Display);
CR_STATUS CrMdssStop(CR_MDSS *Display);
CR_STATUS CrMdssUninit(CR_MDSS *Display);
/* Only a running, healthy pipe can be transferred. Success forgets local
   ownership without disabling hardware; failure preserves every phase. */
CR_STATUS CrMdssTransferToPost(CR_MDSS *Display);
/* Pure present preflight, used before a hardware-orientation transaction. */
CR_STATUS CrMdssPresentValidate(CONST CR_MDSS *,CONST VOID *,UINTN,UINT32,
 CONST CR_MDSS_MOVE *,UINTN,CONST CR_MDSS_RECT *,UINTN);
CR_STATUS CrMdssPresent(CR_MDSS *Display, CONST VOID *Source,
                        UINTN SourceBytes, UINT32 SourceStride,
                        CONST CR_MDSS_MOVE *Moves, UINTN MoveCount,
                        CONST CR_MDSS_RECT *Dirty, UINTN DirtyCount);
typedef struct {
  VOID *Pixels, *Before;
  UINTN PixelBytes, BeforeBytes;
  UINT32 PixelsTransform, NextPixelsTransform; /* identity=0, rotate180=3 */
} CR_MDSS_PRESENT_STAGE;
/* CPU preparation only, before a writeback transaction. Both output buffers
   use the display stride and must hold the full raster, including padding.
   Snapshot the current framebuffer into Before, normalize visible Pixels for
   the requested orientation, then apply ordered moves followed by dirty data.
   All arguments are validated before either output changes. Outputs must not
   overlap any input or each other; the caller independently excludes physical
   aliases behind different virtual addresses and pins every range.

   Caller serializes the display and drains crash/other pixel writers before
   taking real CPU access to the primary and staging buffers. Release all such
   access before HAL mapping changes or waiting for WB. This routine does not
   call IO/publication hooks, change primary pixels or display/transform state,
   prove DMA availability, or authorize rollback after an uncertain transfer.
   Caller publishes staging caches and commits orientation only after exact WB
   completion, physical stop, READ restoration and scanout resume. */
CR_STATUS CrMdssPresentStage(CONST CR_MDSS *Display, CONST VOID *Source,
                        UINTN SourceBytes, UINT32 SourceStride,
                        CONST CR_MDSS_MOVE *Moves, UINTN MoveCount,
                        CONST CR_MDSS_RECT *Dirty, UINTN DirtyCount,
                        CONST CR_MDSS_PRESENT_STAGE *Stage);
CR_STATUS CrMdssBlack(CR_MDSS *Display);
