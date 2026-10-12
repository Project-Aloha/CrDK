/* Exact NPARESOURCE profiles and absolute owner journal. MIT */
#pragma once
#include <Library/PepJournalLib.h>

typedef enum { CrPepNpaArcPowerLevel, CrPepNpaArcXoLevel } CR_PEP_NPA_KIND;
typedef struct { UINT64 Arguments[2]; } CR_PEP_NPA_PROFILE;
typedef struct {
  CR_PEP_TEXT Name;
  CONST CHAR8 *HardwareName;
  CR_PEP_NPA_KIND Kind;
  CONST CR_PEP_NPA_PROFILE *Profiles;
  UINT32 ProfileCount;
} CR_PEP_NPA_BINDING;
/* Backend-native corner index, distinct from the AML virtual level. Full width
   prevents implicit narrowing at an OS boundary. Absent is always state zero. */
typedef struct { UINT64 State; BOOLEAN Present; } CR_PEP_NPA_VALUE;
typedef struct {
  VOID *Context;
  /* Pure cached metadata qualification and virtual level -> hardware corner.
     No allocation, reservation or vote. Missing metadata is unavailable. */
  CR_STATUS (*ResolveLevel)(VOID *, CONST CR_PEP_NPA_BINDING *, UINT64 Level,
                            CR_PEP_NPA_VALUE *);
  CR_STATUS (*Get)(VOID *, VOID *Owner, CONST CR_PEP_NPA_BINDING *,
                   CR_PEP_NPA_VALUE *, BOOLEAN *Pending);
  /* Absolute logical owner value. Failed writes/restores retain prerequisites
     and uncertainty until retry; never withdraw another owner's vote. */
  CR_STATUS (*Set)(VOID *, VOID *Owner, CONST CR_PEP_NPA_BINDING *,
                   CONST CR_PEP_NPA_VALUE *);
} CR_PEP_NPA_IO;
typedef struct {
  CR_PEP_NPA_IO Io;
  CONST CR_PEP_NPA_BINDING *Bindings;
  UINT32 Count;
  BOOLEAN Initialized;
} CR_PEP_NPA_PROVIDER;

/* PASSIVE and externally serialized. Immutable bindings/IO/code outlive every
   journal, including failed Close. Exact opcode/name/full-width tuple match;
   no unknown-name no-op, nonzero-as-enable or millivolt/mode interpretation.
   Describe performs no IO and cannot establish hardware readiness. Resolve
   checks cached backend metadata and a reserved owner's current vote. */
CR_STATUS CrPepNpaProviderInit(CR_PEP_NPA_PROVIDER *, CONST CR_PEP_NPA_IO *,
                               CONST CR_PEP_NPA_BINDING *, UINT32 Count);
CR_STATUS CrPepNpaProviderDescribe(CR_PEP_NPA_PROVIDER *, CONST CR_PEP_RESOURCE_OP *,
                                   CONST CR_PEP_NPA_BINDING **, CONST CR_PEP_NPA_PROFILE **);
CR_STATUS CrPepNpaProviderGetJournal(CR_PEP_NPA_PROVIDER *, CR_PEP_JOURNAL_PROVIDER *);
/* Only after journals close and callbacks drain; publication pins the storage. */
CR_STATUS CrPepNpaProviderUninit(CR_PEP_NPA_PROVIDER *);
/* Original Kodiak AML name/tuple inventory, not proof of a board backend.
   CX0/256 and XO0/128 are virtual levels. XO128 requires real CmdDB aux mapping
   to hardware index3; an absent or mismatched table must decline. */
VOID CrPepNpaGetKodiakBindings(CONST CR_PEP_NPA_BINDING **, UINT32 *Count);
