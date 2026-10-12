/* Private early GPU retirement. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Interface/CrAdrenoHandoff.h>
#define CR_ADRENO_BOOT_STREAMS 24U
#define CR_ADRENO_BOOT_CONTEXTS 32U
typedef struct {
 UINT32 Id[3],Control,StreamCount,ContextCount,ContextBase;
 UINT32 Smr[24],S2cr[24],Cbar[32],Cba2r[32],Sctlr[32],TtbrAsid[32];
 UINT32 SlotMask,ContextMask,AliasMask;
} CR_ADRENO_BOOT_INVENTORY;
typedef struct {
 VOID *Context;UINTN ContextBytes;UINT64 Incarnation;
 /* Every operation verifies the exact retained native clock/access owner.
  * MMIO offsets are inside Adreno non-secure SMMU 0x03da0000/0x40000.
  * This adapter cannot be installed as a late EBS observer. */
 CR_STATUS (*Check)(VOID *,UINT64);
 CR_STATUS (*Read)(VOID *,UINT64,UINT32,UINT32 *);
 CR_STATUS (*Write)(VOID *,UINT64,UINT32,UINT32);
 CR_STATUS (*Barrier)(VOID *,UINT64);
 CR_STATUS (*DelayUs)(VOID *,UINT64,UINT32);
} CR_ADRENO_BOOT_IO;
typedef struct {
 VOID *Self;CONST CR_ADRENO_BOOT_IO *OriginalIo;CR_ADRENO_BOOT_IO Io;
 CR_ADRENO_STOP_CONTEXT *Stop;UINT64 Incarnation;UINT32 Stage;
 BOOLEAN Attempted,Retired,Uncertain,Closed;
 CR_STATUS LastStatus;
 CR_ADRENO_BOOT_INVENTORY Before,After;
 CR_ADRENO_STOP_OBSERVATION StopEvidence;
 UINT32 FaultAttempted,FaultDone,DisableAttempted,DisableDone,TlbiDone;
} CR_ADRENO_BOOT_CONTEXT;
/* Zeroed independent permanent storage. Sole native owner serializes all
 * calls/descriptor lifetime before boot services exit. No external caller may
 * mutate this private context. A failed side effect makes the object terminal;
 * retained access, record page and all original firmware backing remain pinned.
 * No rollback/retry of unknown MMIO, no global reset, no secure access. */
CR_STATUS CrAdrenoBootInit(CR_ADRENO_BOOT_CONTEXT *,CONST CR_ADRENO_BOOT_IO *,CR_ADRENO_STOP_CONTEXT *);
/* Inventories all visible slots/CBs FIRST; unknown active routes => no writes
 * including no GPU stop. Then actual StopFence+fresh Check, fault only exact
 * own slots, complete each own ASID maintenance, disable only own CB, repeat
 * TLBI and full fresh verification. Unsupported fault-write quirk is terminal.
 * Invalid SMR/unmatched streams already fault (USFCFG required). */
CR_STATUS CrAdrenoBootRetire(CR_ADRENO_BOOT_CONTEXT *);
/* Fresh stop/retired route proof; no write. Must run directly before producer
 * permanently closes native admission and commits its immutable GPUH+IORT.
 * Record is left unchanged on error. Publication itself stays native-owned. */
CR_STATUS CrAdrenoBootRecord(CR_ADRENO_BOOT_CONTEXT *,CONST CR_ADRENO_ROUTE_FACTS *,UINT64 RecordPhysical,CR_ADRENO_HANDOFF_RECORD *);
