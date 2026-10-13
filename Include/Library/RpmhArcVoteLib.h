/* Fixed-owner active-state ARC/XO votes. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_RPMH_ARC_MAX_LEVELS 32U
#define CR_RPMH_ARC_MAX_RESOURCES 16U
typedef enum { CrRpmhArcPower=1, CrRpmhArcXo=2 } CR_RPMH_ARC_KIND;
typedef struct {
  CHAR8 Name[9];
  UINT32 Address;
  CR_RPMH_ARC_KIND Kind;
  UINT32 LevelCount;
  UINT16 Levels[CR_RPMH_ARC_MAX_LEVELS];
} CR_RPMH_ARC_RESOURCE;
typedef struct {UINT32 Corner;BOOLEAN Present;} CR_RPMH_ARC_VALUE;
typedef struct {VOID *Owner;UINT32 Resource;CR_RPMH_ARC_VALUE Value;BOOLEAN RecoveryPending,Inherited;} CR_RPMH_ARC_ENTRY;
typedef struct {UINT32 Applied;BOOLEAN Known,Pending,Touched;} CR_RPMH_ARC_STATE;
typedef struct {VOID *Context;CR_STATUS (*Write)(VOID *,UINT32 Address,UINT32 Corner);} CR_RPMH_ARC_IO;
typedef struct {
  CR_RPMH_ARC_IO Io;
  CONST CR_RPMH_ARC_RESOURCE *Resources;
  UINT32 Count,Capacity;
  CR_RPMH_ARC_ENTRY *Entries;
  CR_RPMH_ARC_STATE States[CR_RPMH_ARC_MAX_RESOURCES];
  BOOLEAN Initialized;
} CR_RPMH_ARC_VOTER;
/* Names/addresses/table inventory are immutable caller-owned nonpaged storage.
   Voter storage is zeroed before the first Init; a live Init is refused. All
   calls are PASSIVE and share one external serialization. Immutable raw
   address inventory may be read under the enclosing provider rundown gate.
   No allocation or IO in Qualify/Init/Describe/Resolve/Reserve/
   Get. Hardware corner is an INDEX, not millivolts or virtual performance level.
   CmdDB accelerator3 and 4-byte canonical addresses are required. Power tables
   are LE16, start at0, monotonic with trailing zero padding; duplicates retain
   their original index. XO raw votes are only0/3 (Linux clk-rpmh). Optional XO
   aux data is preserved, but Resolve requires it prove the resulting0/3 index.
   Only ACTIVE votes are supported; sleep/wake caches need a separate backend.
   Fresh hardware state is unknown: Get reports pending until an explicit Set
   establishes a state. Init never assumes bootloader votes are absent or clears
   them. Caller must qualify ownership/handoff before that first hardware Set.
   Reservations and old logical values survive uncertain writes. The next Set
   restores the exact old aggregate before attempting a new value. A first Set
   writes the desired aggregate directly; releasing an untouched absent slot
   does not clear unknown bootloader hardware. Max owners
   aggregate independently; no owner can erase another demand. Release only after
   consumer/journal drain; failed cleanup keeps reservation/code/storage alive. */
CR_STATUS CrRpmhArcQualify(CR_RPMH_ARC_RESOURCE *,CONST CHAR8 *Name,UINT32 Address,
                          CR_RPMH_ARC_KIND,CONST UINT8 *Aux,UINT32 Length);
CR_STATUS CrRpmhArcVotesInit(CR_RPMH_ARC_VOTER *,CONST CR_RPMH_ARC_IO *,
                             CONST CR_RPMH_ARC_RESOURCE *,UINT32 Count,
                             CR_RPMH_ARC_ENTRY *,UINT32 Capacity);
/* Pure, atomic import of a complete trusted native aggregate manifest. Every
 * resource must be still unknown/untouched with no live reservations; Count
 * equals the immutable inventory count. Caller validates provenance, tables,
 * final native owner drain and hardware aggregate before invoking this API.
 * Import is not a readback-derived ownership proof. The inherited permanent
 * owner's entries cannot be mutated/released by generic consumer APIs. They
 * pin baseline demand through all foreign owner writes, failures and teardown.
 * A nonzero baseline is intentionally retained until reboot; no implicit
 * Drain can withdraw demand on behalf of software absent from Windows. */
CR_STATUS CrRpmhArcImportBaseline(CR_RPMH_ARC_VOTER *,VOID *PermanentOwner,
    CONST UINT32 *Corners,UINT32 Count);
CR_STATUS CrRpmhArcDescribe(CR_RPMH_ARC_VOTER *,CONST CHAR8 *,CR_RPMH_ARC_RESOURCE *);
CR_STATUS CrRpmhArcResolveLevel(CR_RPMH_ARC_VOTER *,CONST CHAR8 *,UINT32 Level,UINT32 *Corner);
CR_STATUS CrRpmhArcReserve(CR_RPMH_ARC_VOTER *,VOID *Owner,CONST CHAR8 *);
CR_STATUS CrRpmhArcGet(CR_RPMH_ARC_VOTER *,VOID *Owner,CONST CHAR8 *,CR_RPMH_ARC_VALUE *,BOOLEAN *Pending);
CR_STATUS CrRpmhArcSet(CR_RPMH_ARC_VOTER *,VOID *Owner,CONST CHAR8 *,CONST CR_RPMH_ARC_VALUE *);
CR_STATUS CrRpmhArcReleaseOwner(CR_RPMH_ARC_VOTER *,VOID *Owner);
CR_STATUS CrRpmhArcDrain(CR_RPMH_ARC_VOTER *);
CR_STATUS CrRpmhArcUninit(CR_RPMH_ARC_VOTER *);
