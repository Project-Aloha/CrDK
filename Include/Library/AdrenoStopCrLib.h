/** SM8450 inherited Gen7 reset/AXI fence. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/Sm8450GpuResourcesCrLib.h>

#define CR_ADRENO_STOP_VERSION 1U
#define CR_ADRENO_STOP_POLICY_SM8450_ANDROID 1U
#define CR_ADRENO_STOP_CLOSED 0x80000000U
#define CR_ADRENO_STOP_ACTIVE 1U
#define CR_ADRENO_STOP_TCS_COUNT 10U

typedef enum {
  CrAdrenoStopGxStatus=0, CrAdrenoStopGpuIrqMask, CrAdrenoStopGmuIrqMask,
  CrAdrenoStopAoIrqMask, CrAdrenoStopKeepalive, CrAdrenoStopQactive,
  CrAdrenoStopAhbClear, CrAdrenoStopAhbFence, CrAdrenoStopCm3Reset,
  CrAdrenoStopGxHalt, CrAdrenoStopGxAck, CrAdrenoStopCxHalt, CrAdrenoStopCxAck,
  CrAdrenoStopGpuReset, CrAdrenoStopCxBusy, CrAdrenoStopRsccBusy,
  CrAdrenoStopTcs0, CrAdrenoStopTcs1, CrAdrenoStopTcs2, CrAdrenoStopTcs3,
  CrAdrenoStopTcs4, CrAdrenoStopTcs5, CrAdrenoStopTcs6, CrAdrenoStopTcs7,
  CrAdrenoStopTcs8, CrAdrenoStopTcs9, CrAdrenoStopRegisterCount
} CR_ADRENO_STOP_REGISTER;
#define CR_ADRENO_STOP_REG_READ 1U
#define CR_ADRENO_STOP_REG_WRITE 2U
#define CR_ADRENO_STOP_REG_GX 4U

typedef struct {UINT32 Namespace,WordOffset,Access,Reserved;} CR_ADRENO_STOP_REGISTER_INFO;
/* Pure target lookup and actual SM8450 resolver composition. Errors preserve
 * output; neither function maps memory or qualifies live access. */
CR_STATUS CrAdrenoSm8450StopDescribe(CR_ADRENO_STOP_REGISTER Register,
    CR_ADRENO_STOP_REGISTER_INFO *Output);
CR_STATUS CrAdrenoSm8450StopResolve(CONST CR_SM8450_GPU_LAYOUT *Layout,
    CR_ADRENO_STOP_REGISTER Register,CR_SM8450_GPU_REGISTER_SPAN *Output);

typedef struct {
  VOID *Context;
  UINTN ContextBytes;
  UINT64 StartIncarnation;
  CR_STATUS (*Read)(VOID *,UINT64,CR_ADRENO_STOP_REGISTER,UINT32 *);
  CR_STATUS (*Write)(VOID *,UINT64,CR_ADRENO_STOP_REGISTER,UINT32);
  CR_STATUS (*Barrier)(VOID *,UINT64);
  CR_STATUS (*DelayUs)(VOID *,UINT64,UINT32);
} CR_ADRENO_STOP_IO;

typedef enum {
  CrAdrenoStopPhaseNone=0, CrAdrenoStopPhaseMasks, CrAdrenoStopPhaseVotes,
  CrAdrenoStopPhaseCm3, CrAdrenoStopPhaseGx, CrAdrenoStopPhaseClient,
  CrAdrenoStopPhaseArb, CrAdrenoStopPhaseGpuReset, CrAdrenoStopPhaseVerify,
  CrAdrenoStopPhaseHeld
} CR_ADRENO_STOP_PHASE;

typedef struct {
  UINT32 Version,Size,Policy,GxState; /* GxState is raw bits0/1;0 means ON. */
  UINT64 StartIncarnation,Generation,ReadMask,AttemptedWrites,AcknowledgedWrites;
  UINT32 Values[CrAdrenoStopRegisterCount];
} CR_ADRENO_STOP_OBSERVATION;
CR_STATIC_ASSERT(sizeof(CR_ADRENO_STOP_OBSERVATION)==160,"Stop observation ABI");

typedef struct _CR_ADRENO_STOP_CONTEXT {
  struct _CR_ADRENO_STOP_CONTEXT *Self;
  CONST CR_ADRENO_STOP_IO *OriginalIo;
  CR_ADRENO_STOP_IO Io;
  UINT64 StartIncarnation,Generation,HeldGeneration;
  volatile UINT32 Gate;
  UINT32 Phase,FirstFailurePhase,BranchPinned;
  CR_STATUS FirstFailure,FirstRawStatus,LastStatus;
  CR_ADRENO_STOP_OBSERVATION Observation;
} CR_ADRENO_STOP_CONTEXT;

/* Initially zeroed, naturally aligned, independent permanent nonpaged context.
 * IO descriptor and backend Context[ContextBytes] remain immutable/resident
 * until after seal and successful drain. Init and storage retirement externally
 * serialized; all operations except Seal are PASSIVE owner operations. Context
 * is private: copying/tampering/reinitializing a live or sealed object is invalid.
 * Descriptor/context/backend ranges must not overlap. No allocation/IO in Init.
 * Actual backend must hold exact PDO/Start/power/assignment/IRQ/writer ownership
 * and validate it inside EVERY callback; the portable context grants none.
 * Before Fence: actual software/HFI/ring writers and IRQ/DPC/work are closed and
 * drained. Do not implement that prerequisite as a caller-supplied Ready flag.
 */
CR_STATUS CrAdrenoStopInit(CR_ADRENO_STOP_CONTEXT *,CONST CR_ADRENO_STOP_IO *,UINT64 StartIncarnation);
/* Fixed Android inherited SM8450 policy, all10 TCS mandatory. Masks, Qactive,
 * pending-vote checks, AHB allow, CM3 reset, qualified GX halt, client1 then ARB2,
 * optional GPU reset held>=100us, fresh verification. Final ARB control is2,
 * never invented mask3. No halt/reset release, RSCC sleep, power collapse, SMC,
 * SMMU fault-resume or firmware/queue operation. Every mutation is journaled
 * before callback; any nonexact status stops progression with ownership held.
 * Retry revalidates the same physical GX branch and does not clear retained
 * ARB2 to repeat client1. Failed operations never imply safe cleanup/free.
 */
CR_STATUS CrAdrenoStopFence(CR_ADRENO_STOP_CONTEXT *);
/* Read-only fresh verification under the same actual backend. Requires the
 * same completed run generation, but does not trust its cached register values.
 * Error leaves output unchanged and invalidates held evidence. Output must be
 * disjoint from this entire context, original IO and backend context. The result
 * is a held reset/AXI fence observation, not full power-off/HAL attach/free proof.
 * CX busy is recorded raw, not interpreted as a successful graceful slumber.
 */
CR_STATUS CrAdrenoStopCheck(CR_ADRENO_STOP_CONTEXT *,CR_ADRENO_STOP_OBSERVATION *);
/* Invalidate under the same serialized owner BEFORE any resume/domain/reset
 * transition; metadata only, cannot reopen a sealed object or erase effects.
 * Seal atomically closes new operations, never performs IO. CR_BUSY means seal
 * committed but an admitted operation/callback remains: retain all storage and
 * backend lifetime, drain that actual operation, then retry Seal. An admitted
 * callback may finish; no later phase is admitted after it observes closure.
 */
CR_STATUS CrAdrenoStopInvalidate(CR_ADRENO_STOP_CONTEXT *);
CR_STATUS CrAdrenoStopSeal(CR_ADRENO_STOP_CONTEXT *);
