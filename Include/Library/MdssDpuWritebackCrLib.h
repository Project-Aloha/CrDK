/* SM8450 DMA1/LM2/PP2/CTL2/WB2 linear copy. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssDpuInterruptCrLib.h>
/* Maximum touched footprint, not the size of a retained primary mapping. */
#define CR_MDSS_WB_MAX_BYTES 0x1000000U
/* WB2 supports4096, but this recipe owns only one2560px LM2. */
#define CR_MDSS_WB_MAX_WIDTH 2560U
#define CR_MDSS_WB_JOURNAL_MAX 112U
/* Target Android SM8450 sde_hw_vbif: request is low16, ACK is high16.
   Mainline currently decodes low16; this target port does not guess both. */
#define CR_MDSS_WB_ACK_SHIFT 16U
typedef enum {CrMdssWbDpu,CrMdssWbVbif,CrMdssWbWindowCount} CR_MDSS_WB_WINDOW;
typedef struct {
 VOID *Context;
 UINT64 Physical[CrMdssWbWindowCount];
 UINTN Bytes[CrMdssWbWindowCount];
 /* Checked bounded nonpaged IO; each callback enforces actual current
    assignment/Hardware admission. Kick calls only these three at DIRQL:
    no PASSIVE Ready, provider locks, waits, allocation or delay is permitted. */
 CR_STATUS (*Read32)(VOID *,CR_MDSS_WB_WINDOW,UINT32,UINT32 *);
 CR_STATUS (*Write32)(VOID *,CR_MDSS_WB_WINDOW,UINT32,UINT32);
 CR_STATUS (*Fence)(VOID *);
 /* Prepare/Stop only, at PASSIVE. No allocator, provider or delay in Kick. */
 CR_STATUS (*DelayUs)(VOID *,UINT32);
} CR_MDSS_WB_IO;
typedef struct {
 CONST VOID *Owner;
 UINT64 InputIova,OutputIova,InputBytes,OutputBytes;
 UINT32 Width,Height,InputStride,OutputStride,Reflections,PixelsPerSecond;
} CR_MDSS_WB_JOB;
typedef enum {CrMdssWbIdle,CrMdssWbPrepared,CrMdssWbKickAttempted,
 CrMdssWbRunning,CrMdssWbIrqDone,CrMdssWbStopping,CrMdssWbStopped,
 CrMdssWbQuarantined} CR_MDSS_WB_STATE;
typedef struct {
 CR_MDSS_WB_WINDOW Window;
 UINT32 Offset,Mask,Original,Desired;
 BOOLEAN Touched,Restored;
} CR_MDSS_WB_REGISTER;
typedef struct {
 CR_MDSS_WB_IO Io;
 CONST CR_MDSS_WB_JOB *Identity;
 CR_MDSS_WB_JOB Job;
 CONST CR_MDSS_IRQ_TOKEN *IrqIdentity;
 CONST VOID *IrqOwner;
 CR_MDSS_IRQ_TOKEN IrqToken;
 UINT64 ArmGeneration,IrqSequence;
 CR_MDSS_WB_REGISTER Journal[CR_MDSS_WB_JOURNAL_MAX];
 UINT32 JournalCount;
 CR_MDSS_WB_STATE State;
 CR_STATUS LastError;
 BOOLEAN Initialized,Touched,CleanupPending,Halted,FlushObserved,InterruptObserved;
 BOOLEAN ResetAttempted,ResetAcknowledged;
} CR_MDSS_DPU_WRITEBACK;
/* Zeroed permanent storage; no IO/allocation. Actual owner has exclusively
   assigned DPU/VBIF, retired native/foreign DMA1/LM2/PP2/CTL2/WB writers,
   concrete clocks/domain/ICB/power and READ input + WRITE output mappings.
   All operations serialized with that owner; no callback or descriptor
   acts as a resource or DMA-stop grant. No physical=IOVA assumption. Actual
   mapper excludes physical input/output aliases independently of IOVA checks. */
CR_STATUS CrMdssDpuWritebackInit(CR_MDSS_DPU_WRITEBACK *,CONST CR_MDSS_WB_IO *);
/* PASSIVE: validates full geometry before IO, binds exact permanent Job,
   saves originals before possibly committed writes, fresh-halts XIN5/6,
   programs source/route/QoS/format and observes flush latch. Each touched
   footprint is <=16MiB; a larger valid reserved OutputBytes is permitted.
   Partial failures
   retain the same Job/journal; Stop then Retire required before a new job. */
CR_STATUS CrMdssDpuWritebackPrepare(CR_MDSS_DPU_WRITEBACK *,CONST CR_MDSS_WB_JOB *);
/* Only called by the future typed adapter WHILE holding the actual IRQ lock,
   immediately after actual CrMdssIrqArm for this exact job. Binds exact token
   and arm generation before START; no polling or arbitrary kick callback.
   IrqOwner is the real registered leaf owner (possibly distinct from Job.Owner).
   Any kick error keeps a possibly executing job and forbids new Prepare. */
CR_STATUS CrMdssDpuWritebackKick(CR_MDSS_DPU_WRITEBACK *,CONST CR_MDSS_WB_JOB *,
 CONST VOID *IrqOwner,CONST CR_MDSS_IRQ_TOKEN *,UINT64 ArmGeneration);
/* Metadata only after the actual synchronized IRQ ledger Take. Checks exact
   token/job/owner/registration/generation/sequence + bit4. It does not read
   status or turn idle/timeout into completion. Kick uncertainty is retained. */
CR_STATUS CrMdssDpuWritebackComplete(CR_MDSS_DPU_WRITEBACK *,CONST CR_MDSS_IRQ_EVENT *);
/* PASSIVE: close admission, remove routes/reset CTL2, fresh XIN5/6 halt+idle,
   then restore masked ordinary originals (never START/reset/flush/status).
   Leaves XIN5/6 halt request asserted with current ACK+idle; force fields
   restore only after that drain. CTL reset and detached routes are retained.
   No map/token/power
   retirement. Failure retains all journals and exact job; retry allowed only
   with actual current access. Quarantine never permits new MMIO. */
CR_STATUS CrMdssDpuWritebackStop(CR_MDSS_DPU_WRITEBACK *);
/* Read-only current off fence for an actual mapper's typed callback. Exact
   saved job + arm generation (zero if never kicked), no cached Ready grant.
   The owner supplies current resource/access/rundown and preserves storage. */
CR_STATUS CrMdssDpuWritebackCheckStopped(CR_MDSS_DPU_WRITEBACK *,
 CONST CR_MDSS_WB_JOB *,UINT64 ArmGeneration);
/* After outer exact job/IRQ+DPC/waiter/output/CPU rundown, rereads off/halt
   evidence without writes, then retires job metadata. It never unmaps/frees. */
CR_STATUS CrMdssDpuWritebackRetire(CR_MDSS_DPU_WRITEBACK *);
/* Caller drains all operations before this metadata-only irreversible seal.
   No DMA stop/completion is implied; uncertain job/storage remains retained. */
VOID CrMdssDpuWritebackQuarantine(CR_MDSS_DPU_WRITEBACK *);
CR_STATUS CrMdssDpuWritebackUninit(CR_MDSS_DPU_WRITEBACK *);
