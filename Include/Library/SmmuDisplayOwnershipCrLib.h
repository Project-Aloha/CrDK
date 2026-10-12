/* Shared local retirement of inherited nonsecure display SMMU contexts.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/smmu.h>

#define CR_SMMU_DISPLAY_GROUP_CORE_MAX_STREAMS 128U
#define CR_SMMU_DISPLAY_GROUP_CORE_MAX_BANKS 128U
#define CR_SMMU_DISPLAY_GROUP_CORE_MAX_OWNED 4U

typedef struct {
  UINT32 Sctlr,Cbar,Cba2r,Tcr,Tcr2,Mair0,Mair1,Actlr,ContextId;
  UINT64 Ttbr0,Ttbr1;
} CR_SMMU_DISPLAY_GROUP_CORE_BANK;
typedef enum {
  CrSmmuDisplayNoConflict,CrSmmuDisplayReservedStream,
  CrSmmuDisplayReservedBank,CrSmmuDisplayReservedAsid,
  CrSmmuDisplaySharedBank,CrSmmuDisplaySharedAsid
} CR_SMMU_DISPLAY_CONFLICT;
typedef struct {
  UINT32 Id0,Id1,Id2,Control;
  UINT32 Smr[CR_SMMU_DISPLAY_GROUP_CORE_MAX_STREAMS];
  UINT32 S2cr[CR_SMMU_DISPLAY_GROUP_CORE_MAX_STREAMS];
  CR_SMMU_DISPLAY_GROUP_CORE_BANK Banks[CR_SMMU_DISPLAY_GROUP_CORE_MAX_BANKS];
  /* Pure admission derives these indices; callers never seed ownership. */
  UINT16 Streams[CR_SMMU_DISPLAY_GROUP_CORE_MAX_OWNED];
  UINT16 OwnedBanks[CR_SMMU_DISPLAY_GROUP_CORE_MAX_OWNED];
  UINT16 Asids[CR_SMMU_DISPLAY_GROUP_CORE_MAX_OWNED];
  UINT16 StreamCount,BankCount;
  /* Refusal provenance only; never an ownership input. Index identifies a
     stream for ReservedStream/SharedBank, otherwise a context bank. */
  CR_SMMU_DISPLAY_CONFLICT Conflict;
  UINT16 ConflictIndex,ConflictAsid;
} CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT;
typedef struct {
  VOID *Context;
  /* Real producer access/serialization/incarnation, no passive rail work.
   * All offsets are relative to exact 0x15000000/0x100000 resource. */
  CR_STATUS (*Ready)(VOID *,UINT64 Generation);
  CR_STATUS (*Read32)(VOID *,UINT32 Offset,UINT32 *Value);
  CR_STATUS (*Write32)(VOID *,UINT32 Offset,UINT32 Value);
  CR_STATUS (*Fence)(VOID *);
  CR_STATUS (*DelayUs)(VOID *,UINT32 Us);
} CR_SMMU_DISPLAY_GROUP_CORE_IO;

typedef struct _CR_SMMU_DISPLAY_GROUP_CORE {
  struct _CR_SMMU_DISPLAY_GROUP_CORE *Self;
  SmmuDevice *Device;
  CR_SMMU_DISPLAY_GROUP_CORE_IO Io;
  UINT64 Generation;
  UINT16 Sids[4], SidBase, SidMask;
  CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT Before,Scratch;
  /* Per-operation stages are retained before every possibly posted write. */
  UINT32 Stage,Index,Step;
  CR_STATUS LastStatus;
  BOOLEAN Initialized,Inventoried,Reserved,Touched,Retired,Failed;
} CR_SMMU_DISPLAY_GROUP_CORE;

/* These producer-internal calls require its exclusive SMMU writer lock.
 * IO callbacks must not sleep, allocate or invoke RPMh/clock providers.
 * Init is pure and allows Io self-alias; context storage remains pinned.
 * No function reads or frees old tables or framebuffer allocations. */
CR_STATUS CrSmmuDisplayGroupInit(CR_SMMU_DISPLAY_GROUP_CORE *,SmmuDevice *,
  CONST CR_SMMU_DISPLAY_GROUP_CORE_IO *,CONST UINT16 Sids[4],UINT64 Generation);
/* Pure hardware inventory. Initial failure preserves an incomplete diagnostic
 * snapshot; repeats after success verify the immutable Before identity.
 * No old translation/mapping proof. */
CR_STATUS CrSmmuDisplayGroupObserve(CR_SMMU_DISPLAY_GROUP_CORE *);
/* Recheck a previously admitted inventory without replacing its identity. */
CR_STATUS CrSmmuDisplayGroupCheckInventory(CR_SMMU_DISPLAY_GROUP_CORE *);
/* Internal native producer boundary, never a public caller-supplied flag:
 * its pending owner must have invoked its bound physical stop callback outside
 * the SMMU lock while retaining its display mutex and sealed writer admission.
 * The producer keeps that mutex/admission through this commit, revalidates
 * the incarnation under its lock and blocks ordinary Attach of these SIDs.
 * Retire independently rescans exclusivity before its first write. It then
 * faults exact streams, disables/syncs exclusive banks, invalidates exact
 * SMRs only under observed USFCFG and retains all old reservations forever.
 * All partial effects retain ownership, never rollback to live DMA. The
 * same pinned owner may retry stage-aware operations after fresh independent
 * physical stop. Sync timeout is terminal; no old resources are released. */
CR_STATUS CrSmmuDisplayGroupRetire(CR_SMMU_DISPLAY_GROUP_CORE *);
/* Pure terminal old-context check; permits independently owned fresh streams,
 * but rejects any valid stream reusing an old CB. Never resubmits old ASIDs. */
CR_STATUS CrSmmuDisplayGroupCheckRetired(CR_SMMU_DISPLAY_GROUP_CORE *);
/* Only before any physical display retirement effect or SMMU effect, as
 * established by the native owner; never releases old hardware resources. */
CR_STATUS CrSmmuDisplayGroupAbort(CR_SMMU_DISPLAY_GROUP_CORE *);
