/* Single retained A730 kernel CP transaction. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/AdrenoPowerCrLib.h>
#define CR_ADRENO_CP_RING_BYTES 32768U
#define CR_ADRENO_CP_POWER_BYTES 8192U
#define CR_ADRENO_CP_SHADOW_BYTES 4096U
#define CR_ADRENO_CP_CLOSED 0x80000000U
typedef enum {CrAdrenoCpGpu=1,CrAdrenoCpGmu=2} CR_ADRENO_CP_SPACE;
typedef enum {CrAdrenoCpRing,CrAdrenoCpShadow,CrAdrenoCpPowerList,CrAdrenoCpSqe,CrAdrenoCpBuffers} CR_ADRENO_CP_BUFFER;
typedef enum {CrAdrenoCpPreflight=1,CrAdrenoCpAperture,CrAdrenoCpZap} CR_ADRENO_CP_SECURE_STAGE;
typedef enum {CrAdrenoCpNew=1,CrAdrenoCpOobPending,CrAdrenoCpInitPending,
 CrAdrenoCpSecurePending,CrAdrenoCpReady,CrAdrenoCpProbePending,CrAdrenoCpProbeComplete} CR_ADRENO_CP_PHASE;
typedef struct {volatile UINT32 *Cpu;UINT64 Iova;UINT32 Bytes;} CR_ADRENO_CP_REGION;
typedef struct {UINT32 ChipId;CR_ADRENO_CP_REGION Region[CrAdrenoCpBuffers];} CR_ADRENO_CP_BACKING;
typedef struct {
 VOID *Context;UINTN ContextBytes;UINT64 Incarnation;
 /* Entire public call executes under the actual parent's retained operation
    lock. Check validates exact child/runtime/domain/power/firmware identities.
    All callbacks are synchronous and may not retain this context's arguments. */
 CR_STATUS (*Check)(VOID *,UINT64);
 CR_STATUS (*Read)(VOID *,UINT64,CR_ADRENO_CP_SPACE,UINT32,UINT32 *);
 CR_STATUS (*Write)(VOID *,UINT64,CR_ADRENO_CP_SPACE,UINT32,UINT32);
 CR_STATUS (*Sync)(VOID *,UINT64,CR_ADRENO_CP_BUFFER,BOOLEAN ToDevice);
 /* Preflight is nonmutating, before OOB/CP effects: provider is present,
    retained, current and can execute BOTH required source-ordered operations.
    Aperture must use actual allocated-bank authority if required/available;
    absence only admitted by a separately qualified fixed-domain policy.
    Zap is invoked only after CP_INIT consumption, never confused with PAS1. */
 CR_STATUS (*Secure)(VOID *,UINT64,CR_ADRENO_CP_SECURE_STAGE);
} CR_ADRENO_CP_IO;
typedef struct {
 UINT64 Incarnation,DeadlineUs;
 UINT32 Phase,RegisterWrites,OobAttempted,OobAcknowledged,InitAttempted;
 UINT32 ApertureAttempted,SqeAttempted,ZapAttempted;
 UINT32 PublicationAttempts,PublicationAccepted,Wptr,Nonce,Sequence;
 UINT32 BvFence,Fence,Scratch,Faulted;
 CR_STATUS LastStatus,FirstFailure;
} CR_ADRENO_CP_OUTCOME;
typedef struct _CR_ADRENO_CP_CONTEXT {
 struct _CR_ADRENO_CP_CONTEXT *Self;CONST CR_ADRENO_CP_IO *OriginalIo;
 CONST CR_ADRENO_CP_BACKING *OriginalBacking;CR_ADRENO_CP_IO Io;CR_ADRENO_CP_BACKING Backing;
 volatile UINT32 Gate;UINT64 LastNowUs;UINT32 TimeoutUs;
 CR_ADRENO_CP_OUTCOME Outcome;
} CR_ADRENO_CP_CONTEXT;
/* Init does no IO. Zeroed address-bound context, immutable descriptors and all
   resident CPU/IOVA spans must be distinct. SQE is the source payload after its
   four-byte file prefix. Only exact A730.1 is supported in this first unit. */
CR_STATUS CrAdrenoCpInit(CR_ADRENO_CP_CONTEXT *,CONST CR_ADRENO_CP_IO *,CONST CR_ADRENO_CP_BACKING *);
CR_STATUS CrAdrenoCpBegin(CR_ADRENO_CP_CONTEXT *,UINT64 NowUs,UINT32 TimeoutUs,CR_ADRENO_CP_OUTCOME *);
CR_STATUS CrAdrenoCpPoll(CR_ADRENO_CP_CONTEXT *,UINT64 NowUs,CR_ADRENO_CP_OUTCOME *);
CR_STATUS CrAdrenoCpSubmitProbe(CR_ADRENO_CP_CONTEXT *,UINT32 Nonce,UINT64 NowUs,CR_ADRENO_CP_OUTCOME *);
CR_STATUS CrAdrenoCpQuery(CR_ADRENO_CP_CONTEXT *,CR_ADRENO_CP_OUTCOME *);
CR_STATUS CrAdrenoCpSeal(CR_ADRENO_CP_CONTEXT *);
CR_STATUS CrAdrenoCpDrain(CR_ADRENO_CP_CONTEXT *);
/* No reset/replay/free API. Complete means this one GPU marker/fence only;
   the owner separately stops SQE/GMU/GBIF and retires domains before recycling. */
