/** Retained A730 mainline-family HFI queues. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/AdrenoPowerCrLib.h>

#define CR_ADRENO_HFI_VERSION 1U
#define CR_ADRENO_HFI_BYTES 16384U
#define CR_ADRENO_HFI_WORDS 160U
#define CR_ADRENO_HFI_MAX_BUDGET 32U
#define CR_ADRENO_HFI_MAX_TIMEOUT_US 1000000U
#define CR_ADRENO_HFI_CLOSED 0x80000000U
#define CR_ADRENO_HFI_ACTIVE 1U
#define CR_ADRENO_HFI_PREPARE_TOUCHED 1U
#define CR_ADRENO_HFI_PREPARED 2U
#define CR_ADRENO_HFI_TX_TOUCHED 4U
#define CR_ADRENO_HFI_TX_PUBLISHED 8U
#define CR_ADRENO_HFI_KICK_ATTEMPTED 16U
#define CR_ADRENO_HFI_KICK_ACKNOWLEDGED 32U
#define CR_ADRENO_HFI_RX_PUBLISHED 64U
#define CR_ADRENO_HFI_ACK_RECEIVED 128U
#define CR_ADRENO_HFI_RX_REQUESTED 256U

typedef enum {CrAdrenoHfiPrepare=1,CrAdrenoHfiTransmit,CrAdrenoHfiReceive} CR_ADRENO_HFI_ACCESS;
typedef enum {CrAdrenoHfiAcquire=1,CrAdrenoHfiRelease} CR_ADRENO_HFI_ORDER;
typedef CR_STATUS (*CR_ADRENO_HFI_OPERATION)(VOID *Argument);
typedef struct {
  VOID *Context;
  UINTN ContextBytes;
  UINT64 StartIncarnation;
  /* Validate and pin actual backing/translation/resource/power/Start under the
   * real owner's serialization, then call Operation synchronously exactly once
   * (or zero times on refusal). No retained callback/argument or reentry. Prepare
   * additionally requires actual reset/stop ownership. The operation is bounded
   * shared-memory work; callbacks below run inside that same admission. */
  CR_STATUS (*Execute)(VOID *,UINT64,CR_ADRENO_HFI_ACCESS,CR_ADRENO_HFI_OPERATION,VOID *);
  /* DMA visibility/cache ordering for this actual mapping, not a compiler-only
   * barrier. Do not allocate, wait, call providers or release the admitted pin. */
  CR_STATUS (*Barrier)(VOID *,UINT64,CR_ADRENO_HFI_ORDER);
  /* Exactly HOST2GMU_INTR_SET bit0 after published TX; validate actual access.
   * A nonexact status may mean an effect: no replay is permitted. */
  CR_STATUS (*Kick)(VOID *,UINT64);
} CR_ADRENO_HFI_IO;
typedef struct {volatile UINT32 *Cpu;UINT32 Bytes,GmuIova;} CR_ADRENO_HFI_BACKING;

/* Header word is initially zero. Only the bounded source commands below are
 * admitted: PERF4/59words, BW3/160, CORE_FW_START14/2, START10/1. */
typedef struct {UINT32 Id,Count,Words[CR_ADRENO_HFI_WORDS];} CR_ADRENO_HFI_COMMAND;
typedef struct {
  UINT32 Version,Size,ChipId,AcdEnabled,IfpcEnabled;
  CR_ADRENO_POWER_PLAN Power;
  /* Canonical CmdDB BCM addresses in SH0, MC0, ACV, CN0 order. No pointers. */
  UINT32 BcmAddress[4];
} CR_ADRENO_HFI_BOOT_PLAN;

/* Current A730 catalog has no ACD/IFPC entries. Nonzero policies are explicitly
 * unsupported, never ignored. Validates all used/unused perf rows and BCM
 * addresses (distinct aligned nonzero24-bit). No IO or actual firmware grant.
 * Build emits one of4 mainline boot messages in source order; failure unchanged. */
CR_STATUS CrAdrenoHfiValidateBoot(CONST CR_ADRENO_HFI_BOOT_PLAN *);
CR_STATUS CrAdrenoHfiBuildBootCommand(CONST CR_ADRENO_HFI_BOOT_PLAN *,UINT32 Index,
    CR_ADRENO_HFI_COMMAND *);

typedef struct {
  UINT32 Version,Size;
  UINT64 StartIncarnation,Transaction,DeadlineUs;
  UINT32 Header,Effects,Outstanding,Acknowledged,FirmwareError;
  UINT32 StaleMessages,DebugMessages,LastDebugHeader;
  UINT32 BootActive,BootIndex,BootComplete,Faulted;
  UINT32 RuntimeActive,FirmwareHfiVersion;
  CR_STATUS LastStatus,FirstFailure;
} CR_ADRENO_HFI_OUTCOME;
typedef struct _CR_ADRENO_HFI_CONTEXT {
  struct _CR_ADRENO_HFI_CONTEXT *Self;
  CONST CR_ADRENO_HFI_IO *OriginalIo;
  CONST CR_ADRENO_HFI_BACKING *OriginalBacking;
  CR_ADRENO_HFI_IO Io;
  CR_ADRENO_HFI_BACKING Backing;
  volatile UINT32 Gate;
  UINT32 NextSequence,Prepared,PrepareAttempted,Faulted;
  UINT32 TxIndex,TxReadObserved,RxIndex[2],BootTimeoutUs,RuntimeNextQueue;
  UINT64 LastNowUs;
  CR_ADRENO_HFI_OUTCOME Outcome;
  CR_ADRENO_HFI_COMMAND Command;
  CR_ADRENO_HFI_BOOT_PLAN Boot;
} CR_ADRENO_HFI_CONTEXT;

/* Exact unmoved zeroed nonpaged context and immutable original descriptors;
 * separate resident backing/context ranges, CPU and IOVA page aligned, exactly
 *16KiB and entirely32-bit addressable. Init performs no IO. Lifetime operations
 * externally serialized; operations below are nonblocking single-owner calls.
 * Execute supplies actual authority; this library invents no attestation and
 * authenticates no firmware. The owner pins storage through exact local drain
 * AND independent physical stop/domain retirement before freeing/reusing it. */
CR_STATUS CrAdrenoHfiInit(CR_ADRENO_HFI_CONTEXT *,CONST CR_ADRENO_HFI_IO *,
    CONST CR_ADRENO_HFI_BACKING *,UINT64 StartIncarnation);
CR_STATUS CrAdrenoHfiPrepareQueues(CR_ADRENO_HFI_CONTEXT *);
/* Monotonic actual clock samples in microseconds; timeout1..1,000,000us. One
 * outstanding transaction includes waiting for ring space. Submit/Poll return
 * NOT_READY while pending; SUCCESS only means exact matching successful ACK.
 * Outputs remain unchanged on invalid arguments; admitted calls report exact
 * retained progress even on error. Poll budget1..32 bounds response/debug work.
 * No reset/replay API. Sequence reuse is refused after4094 commands per context
 * so a delayed prior-generation ACK cannot match an exhausted sequence. */
CR_STATUS CrAdrenoHfiSubmit(CR_ADRENO_HFI_CONTEXT *,CONST CR_ADRENO_HFI_COMMAND *,
    UINT64 NowUs,UINT32 TimeoutUs,CR_ADRENO_HFI_OUTCOME *);
CR_STATUS CrAdrenoHfiPoll(CR_ADRENO_HFI_CONTEXT *,UINT64 NowUs,UINT32 Budget,
    CR_ADRENO_HFI_OUTCOME *);
/* Retains plan by value before effects. First Begin submits perf; BootPoll
 * advances exactly one ACK-dependent command at a time through BW/core/start.
 * No live hardware Ready flag: BootComplete means those4 wire ACKs only. */
CR_STATUS CrAdrenoHfiBootBegin(CR_ADRENO_HFI_CONTEXT *,CONST CR_ADRENO_HFI_BOOT_PLAN *,
    UINT64 NowUs,UINT32 TimeoutUs,CR_ADRENO_HFI_OUTCOME *);
CR_STATUS CrAdrenoHfiBootPoll(CR_ADRENO_HFI_CONTEXT *,UINT64 NowUs,UINT32 Budget,
    CR_ADRENO_HFI_OUTCOME *);
/* Query may run after seal/fault, serialized against the actual active call.
 * Seal and Drain do no IO: BUSY retains accepted callback/storage. Neither
 * success nor ACK is remote DMA/firmware/domain retirement permission. */
CR_STATUS CrAdrenoHfiQuery(CR_ADRENO_HFI_CONTEXT *,CR_ADRENO_HFI_OUTCOME *);
CR_STATUS CrAdrenoHfiSeal(CR_ADRENO_HFI_CONTEXT *);
CR_STATUS CrAdrenoHfiDrain(CR_ADRENO_HFI_CONTEXT *);

/* A730 upstream v2.1 wire profile. Observed from the running firmware DTCM by
 * the retained owner; no legacy version command or downstream IRQ assumption.
 * Transition is one-way after all four boot ACKs, without queue reset or
 * sequence reuse. This is wire/service admission, never render/CP authority. */
#define CR_ADRENO_HFI_A730_VERSION 0x20010000U
#define CR_ADRENO_HFI_EVENT_WORDS 255U
typedef enum {CrAdrenoHfiEventAck=1,CrAdrenoHfiEventStaleAck,
  CrAdrenoHfiEventFirmwareError,CrAdrenoHfiEventDebug} CR_ADRENO_HFI_EVENT_KIND;
typedef struct {
  UINT32 Kind,Queue,Count,Words[CR_ADRENO_HFI_EVENT_WORDS];
} CR_ADRENO_HFI_EVENT;
typedef struct {
  UINT32 Count,Probes,More;
  CR_ADRENO_HFI_OUTCOME Outcome;
} CR_ADRENO_HFI_SERVICE_RESULT;
CR_STATUS CrAdrenoHfiEnterRuntime(CR_ADRENO_HFI_CONTEXT *,UINT32 FirmwareHfiVersion,
    CR_ADRENO_HFI_OUTCOME *);
/* Always services response/debug queues, including no outstanding command.
 * Budget and Capacity1..32. Output events are fixed by-value packets, never DMA
 * pointers. Count records committed RX-tail publications even if later access
 * fails; uncounted slots are unspecified. More is conservative on either bound.
 * Copied firmware error is terminal. Opaque debug words are not fence evidence.
 * Command timeout, backwards time and malformed traffic retain the faulted
 * context; no packet replay, implicit reset, or physical retirement. */
CR_STATUS CrAdrenoHfiService(CR_ADRENO_HFI_CONTEXT *,UINT64 NowUs,UINT32 Budget,
    CR_ADRENO_HFI_EVENT *,UINT32 Capacity,CR_ADRENO_HFI_SERVICE_RESULT *);
/* Source ID30 blocking GX/BW vote, indices within the retained boot tables;
 * ID33 prepares slumber with source zero fields. The owner must separately
 * qualify real bandwidth, idle and stop sequencing. One outstanding command.
 * Success still requires Service's exact ACK; submission normally NOT_READY. */
CR_STATUS CrAdrenoHfiRuntimeVote(CR_ADRENO_HFI_CONTEXT *,UINT32 GxIndex,UINT32 BwIndex,
    UINT64 NowUs,UINT32 TimeoutUs,CR_ADRENO_HFI_OUTCOME *);
CR_STATUS CrAdrenoHfiPrepareSlumber(CR_ADRENO_HFI_CONTEXT *,UINT64 NowUs,UINT32 TimeoutUs,
    CR_ADRENO_HFI_OUTCOME *);
