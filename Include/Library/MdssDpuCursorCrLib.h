/* SM8450 DMA2/LM0 ARGB pointer plane. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssDpu2dCrLib.h>
#define CR_MDSS_DPU_CURSOR_MAX 256U
#define CR_MDSS_DPU_CURSOR_STRIDE (CR_MDSS_DPU_CURSOR_MAX*4U)
#define CR_MDSS_DPU_CURSOR_BYTES (CR_MDSS_DPU_CURSOR_STRIDE*CR_MDSS_DPU_CURSOR_MAX)
typedef enum {CrMdssCursorDpu,CrMdssCursorVbif,CrMdssCursorWindowCount} CR_MDSS_CURSOR_WINDOW;
typedef struct {
 VOID *Context;
 UINT64 Physical[CrMdssCursorWindowCount];
 UINTN Bytes[CrMdssCursorWindowCount];
 CR_STATUS (*Ready)(VOID *);
 CR_STATUS (*Read32)(VOID *,CR_MDSS_CURSOR_WINDOW,UINT32,UINT32 *);
 CR_STATUS (*Write32)(VOID *,CR_MDSS_CURSOR_WINDOW,UINT32,UINT32);
 CR_STATUS (*Fence)(VOID *);
 CR_STATUS (*DelayUs)(VOID *,UINT32);
} CR_MDSS_DPU_CURSOR_IO;
typedef struct {
 UINT64 DeviceAddress;
 UINT32 Width,Height;
 INT32 X,Y;
 UINT32 Transform;
 BOOLEAN Visible,Premultiplied;
} CR_MDSS_DPU_CURSOR_IMAGE;
typedef struct {
 CR_MDSS_DPU_CURSOR_IO Io;
 CR_MDSS_MODE Mode;
 UINT64 PrimaryAddress,DeviceAddress[2];
 CR_MDSS_DPU_CURSOR_IMAGE Image;
 UINT32 ForceOriginal;
 BOOLEAN Initialized,Touched,CleanupPending,Visible,Halted,ForceTouched;
} CR_MDSS_DPU_CURSOR;
/* Zeroed permanent storage. Actual caller owns the whole pipeline, both
   assigned exclusive DPU/VBIF windows and two READ-only OS/HAL buffers. All
   methods are serialized with pipeline start/stop, CPU buffer users and fetch
   orientation. Ready must requalify window/pipeline grants before every
   access; the caller also qualifies both live buffer grants before Set.
   Stop/Retire may verify off evidence during partial buffer withdrawal.
   Device addresses are actual DMA-provider outputs, not CPU physical.
   Fixed 256x256 is this implementation's bounded policy, not a hardware max. */
CR_STATUS CrMdssDpuCursorInit(CR_MDSS_DPU_CURSOR *,CONST CR_MDSS_DPU_CURSOR_IO *,
 CONST CR_MDSS_MODE *,UINT64 PrimaryAddress,CONST UINT64 DeviceAddress[2],UINTN BufferBytes);
/* Linear A8R8G8B8, fixed 1024-byte stride, no scaling/mono/XOR/compression.
   Caller fills only an inactive buffer under the real CPU mapping gate, then
   publishes writes before Set. Set removes any prior cursor, flushes and
   halts XIN9 before reprogramming; completion requires route/source readback,
   cleared flush and frame progress. Only identity or both reflections. */
CR_STATUS CrMdssDpuCursorSet(CR_MDSS_DPU_CURSOR *,CONST CR_MDSS_DPU_CURSOR_IMAGE *);
/* Actual off fence: remove DMA2 from CTL/LM0, qualify flush/frame (or observed
   full pipeline off), assert VBIF XIN9 halt and wait for drain acknowledgement.
   Failure retains the journal and both buffer/resource grants; retry permitted.
   Success permits the provider to retire either exact cursor buffer. */
CR_STATUS CrMdssDpuCursorStop(CR_MDSS_DPU_CURSOR *);
/* Guarded readback then metadata retirement after actual Stop; performs no
   writes, leaves XIN9 halted and clears no provider ownership. Call with live
   assigned windows before their retirement. Never replace an uncertain journal. */
CR_STATUS CrMdssDpuCursorRetire(CR_MDSS_DPU_CURSOR *);
