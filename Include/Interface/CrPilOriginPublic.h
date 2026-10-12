/* Exact PAS1 origin and existing physical-owner lease. SPDX-License-Identifier: MIT */
#pragma once
#include <Interface/WdfPilPublic.h>
#include <Library/PILCrLib.h>

#define CR_PIL_ORIGIN_REVISION 1U
DEFINE_GUID(GUID_PIL_ORIGIN_PRODUCER_CR_INTERFACE,0xb496294a,0x588a,0x48f5,0x8b,0x2d,0x51,0x88,0xfa,0xf5,0xdd,0x71);
DEFINE_GUID(GUID_PIL_AUDIO_ORIGIN_CR_INTERFACE,0x4ec8cf4a,0x1a11,0x469b,0x86,0x82,0xa1,0x1b,0x7e,0xb5,0x79,0x75);

/* Trusted permanent PEP wiring, never obtained from a caller's request. The
 * Common adapter calls these while holding its actual platform lock; callbacks
 * only take the ledger leaf lock and never acquire any outer/provider lock. */
typedef struct {
  PVOID Context;
  NTSTATUS (*Bound)(PVOID,PVOID Owner,CONST CR_PIL_TARGET *,CONST CR_PIL_IO *);
  NTSTATUS (*Observe)(PVOID,PVOID BindingContext,PVOID Owner,CONST CR_PIL_SMC_ARGS *Original,
                     CONST CR_PIL_SMC_ARGS *Result,CR_STATUS Transport,BOOLEAN Interrupted,
                     BOOLEAN MaybeOwned,BOOLEAN Quarantined);
  NTSTATUS (*Retired)(PVOID,PVOID BindingContext,PVOID Owner);
  VOID (*Seal)(PVOID,PVOID BindingContext);
} CR_PIL_ORIGIN_OBSERVER;

typedef struct {PVOID Provider;UINT64 Token;} CR_PIL_ORIGIN_START;
typedef struct {
  CONST VOID *MetadataVa;
  UINT64 MetadataPa;
  SIZE_T MetadataBytes;
  CONST CR_PIL_CONTEXT *ExactCore;
  UINT32 ImageSpan; /* Actual validated CR_PIL_IMAGE.Span, never a caller range. */
  BOOLEAN Relocatable;
  /* Actual native allocation owner. Reference pins independent native storage.
   * Check is a no-wait leaf called under the platform/SCM/origin locks: the
   * allocation is private, sealed, immutable and still exactly associated with
   * the tuple. No mutable/writable pointer is exported by this interface. */
  INTERFACE Allocation;
  NTSTATUS (*CheckAllocation)(PVOID,CONST VOID *,UINT64,SIZE_T);
} CR_PIL_ORIGIN_IMAGE;
typedef enum {CrPilAudioNotAccepted=0,CrPilAudioAccepted=1,CrPilAudioUnknown=2} CR_PIL_AUDIO_ACCEPTANCE;
typedef struct {CR_PIL_AUDIO_ACCEPTANCE Acceptance;NTSTATUS Status;} CR_PIL_AUDIO_EXECUTION;
/* PRIVATE trusted synchronous token/copy closure. Called while B->G->native
 * PIL Operations holds; no B/G reacquisition, provider call, wait, allocation,
 * transport poll, callback dispatch or SMC. Never supplied by an IOCTL/user.
 * Once entered, explicit Accepted/Unknown survives asynchronous source loss. */
typedef NTSTATUS (*CR_PIL_AUDIO_OPERATION)(PVOID,CR_PIL_AUDIO_EXECUTION *);
typedef struct {
  INTERFACE Header;
  NTSTATUS (*PrepareImage)(PVOID,PVOID Owner,CONST CR_PIL_IO *ExactBinding,
                           CONST CR_PIL_ORIGIN_IMAGE *,CR_PIL_ORIGIN_START *);
  NTSTATUS (*CompleteStart)(PVOID,PVOID Owner,CONST CR_PIL_IO *ExactBinding,
                            CR_PIL_ORIGIN_START *,CONST CR_PIL_CONTEXT *);
  /* NULL closes admission before Stop. Actual copied shutdown evidence may
   * then be supplied; whole binding retirement remains a separate observer. */
  NTSTATUS (*CloseStart)(PVOID,PVOID Owner,CONST CR_PIL_IO *ExactBinding,
                        CR_PIL_ORIGIN_START *,CONST CR_PIL_SHUTDOWN_EVIDENCE *);
  NTSTATUS (*GetOrigin)(PVOID,PVOID Owner,CONST CR_PIL_IO *,CONST CR_PIL_ORIGIN_START *,
                        UINT64 *BindingSerial,UINT64 *BootSerial,BOOLEAN *Qualified);
  NTSTATUS (*NextLease)(PVOID,PVOID Owner,CONST CR_PIL_IO *,UINT64 *Serial);
  NTSTATUS (*Execute)(PVOID,PVOID Owner,CONST CR_PIL_IO *,CONST CR_PIL_ORIGIN_START *,
                      UINT64 ExpectedBootSerial,CR_PIL_AUDIO_OPERATION,PVOID,CR_PIL_AUDIO_EXECUTION *);
  VOID (*SealOwner)(PVOID,PVOID Owner);
  /* Pure permanent token issuance before any requested Bind effect. It conveys
   * no binding, metadata, PAS, power or boot authority. */
  NTSTATUS (*NextControllerSerial)(PVOID,PVOID Owner,UINT64 *Serial);
} WDF_PIL_ORIGIN_PRODUCER_INTERFACE;

typedef struct {PVOID Provider;UINT64 Token;} CR_PIL_AUDIO_LEASE;
typedef struct {
  WDF_PIL_CR_STATE Pil;
  UINT64 BindingSerial,BootSerial;
  BOOLEAN Qualified;
} WDF_PIL_AUDIO_OBSERVATION;
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Acquire)(PVOID,PVOID PhysicalOwner,CR_PIL_AUDIO_LEASE *,WDF_PIL_AUDIO_OBSERVATION *);
  NTSTATUS (*GetState)(PVOID,PVOID PhysicalOwner,CONST CR_PIL_AUDIO_LEASE *,WDF_PIL_AUDIO_OBSERVATION *);
  NTSTATUS (*Execute)(PVOID,PVOID PhysicalOwner,CONST CR_PIL_AUDIO_LEASE *,UINT64 ExpectedBootSerial,
                      CR_PIL_AUDIO_OPERATION,PVOID,CR_PIL_AUDIO_EXECUTION *);
  NTSTATUS (*Release)(PVOID,PVOID PhysicalOwner,CR_PIL_AUDIO_LEASE *);
} WDF_PIL_AUDIO_ORIGIN_INTERFACE;
/* PASSIVE, exact address-bound storage, never moved/copied while owned. Acquire
 * occupies the SAME physical Owners[] slot as v1, never both. Ordinary READY
 * may succeed with Qualified=FALSE. A nonzero lease on failure remains owned.
 * Release is local lease cleanup only; no graph/DMA/PAS retirement is implied. */
