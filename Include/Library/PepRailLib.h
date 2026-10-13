/* Qualified PMICVREGVOTE journal bridge. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/PepJournalLib.h>
#include <Library/RpmhRailVoteLib.h>
typedef struct {
  UINT64 Arguments[5]; /* All AML opcode/voltage/enable/mode/selector fields. */
  CR_RPMH_RAIL_VALUE Value;
} CR_PEP_RAIL_PROFILE;
typedef struct {
  CR_PEP_TEXT Name, DriverName, VoteType;
  CONST CHAR8 *HardwareName;
  CONST CR_PEP_RAIL_PROFILE *Profiles;
  UINT32 ProfileCount;
  BOOLEAN AllowImplicitQualifier;
} CR_PEP_RAIL_BINDING;
typedef struct {
  VOID *Context;
  CR_STATUS (*Validate)(VOID *, CONST CR_PEP_RAIL_BINDING *, CONST CR_RPMH_RAIL_VALUE *);
  CR_STATUS (*Get)(VOID *, VOID *Owner, CONST CR_PEP_RAIL_BINDING *, CR_RPMH_RAIL_VALUE *, BOOLEAN *Pending);
  CR_STATUS (*Set)(VOID *, VOID *Owner, CONST CR_PEP_RAIL_BINDING *, CONST CR_RPMH_RAIL_VALUE *);
} CR_PEP_RAIL_IO;
typedef struct {
  CR_PEP_RAIL_IO Io;
  CONST CR_PEP_RAIL_BINDING *Bindings;
  UINT32 Count;
  BOOLEAN Initialized;
} CR_PEP_RAIL_PROVIDER;

/* PASSIVE, externally serialized immutable qualified binding/profile table.
   Every complete five-integer AML tuple maps explicitly to a VRM value; no
   guessed voltage units, enable semantics, mode ranking or ignored selectors.
   Qualify the display package's driver/vote strings, or opt into its omitted
   form used by Kodiak USB. Missing tuples/qualifiers decline before IO.
   Init/Resolve/Get do not allocate or vote. IO performs pure metadata/health
   qualification; capture/restore snapshots only this owner's logical value.
   Code/storage/bindings/IO outlive all journals and failed Close recovery. */
CR_STATUS CrPepRailProviderInit(CR_PEP_RAIL_PROVIDER *, CONST CR_PEP_RAIL_IO *,
                                CONST CR_PEP_RAIL_BINDING *, UINT32 Count);
CR_STATUS CrPepRailProviderGetJournal(CR_PEP_RAIL_PROVIDER *, CR_PEP_JOURNAL_PROVIDER *);
/* Pure full tuple/qualifier decoding for pre-publication rail reservations.
   Calls no IO; the borrowed binding and copied value do not prove readiness. */
CR_STATUS CrPepRailProviderDescribe(CR_PEP_RAIL_PROVIDER *, CONST CR_PEP_RESOURCE_OP *,
                                    CONST CR_PEP_RAIL_BINDING **, CR_RPMH_RAIL_VALUE *);
/* Only after journals close, callbacks drain and no permanent publication. */
CR_STATUS CrPepRailProviderUninit(CR_PEP_RAIL_PROVIDER *);
