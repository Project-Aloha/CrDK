/* Shared fixed-contract VRM supply graph and retained cleanup. MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

typedef enum { CrRpmhRailVoltage, CrRpmhRailMode, CrRpmhRailEnable } CR_RPMH_RAIL_VOTE_KIND;
typedef struct _CR_RPMH_RAIL CR_RPMH_RAIL;
struct _CR_RPMH_RAIL {
  CONST CHAR8 *Name;
  UINT32 Address;
  BOOLEAN VoltageRequested, ModeRequested, EnableRequested;
  UINTN ReferenceCount;
  UINT32 VoltageMv;
  UINT8 Mode;
  BOOLEAN Ready;
  CR_RPMH_RAIL *Parent;
  UINT32 ParentMv;
  UINT8 ParentMode;
  BOOLEAN ParentOwned;
};
typedef struct {
  VOID *Context;
  CR_STATUS (*GetAddress)(VOID *, CONST CHAR8 *Name, UINT32 *Address);
  CR_STATUS (*Write)(VOID *, CONST CR_RPMH_RAIL *, CR_RPMH_RAIL_VOTE_KIND, UINT32 Value);
} CR_RPMH_RAIL_IO;

/* PASSIVE, caller-owned nonpaged nodes; serialize ALL graph/raw operations
   under one outer lock. Immutable qualified parent profiles and IO/storage
   outlive every lease and retained recovery. Validation is pure, bounded to64
   nodes and rejects cycles/parent aliases; no allocation, lookup or writes.
   Acquire uses the existing fixed-contract native semantics: sharing requires
   exact voltage/mode, without an inferred voltage or mode aggregation policy.
   Owned is TRUE even on partial failure once a cleanup reference is reserved.
   Release stops the child, mode and voltage before releasing its input supply;
   failed withdrawal retains flags/parent owner for retry. Cached zero address
   resolves lazily only when voting (native optional rails preserve availability).
   Ready is false until the entire acquisition succeeds. */
CR_STATUS CrRpmhRailValidate(CONST CR_RPMH_RAIL *);
CR_STATUS CrRpmhRailAcquire(CONST CR_RPMH_RAIL_IO *, CR_RPMH_RAIL *, UINT32 VoltageMv,
                            UINT8 Mode, BOOLEAN *Owned);
CR_STATUS CrRpmhRailRelease(CONST CR_RPMH_RAIL_IO *, CR_RPMH_RAIL *);
/* Low-level legacy setter: tracks uncertain writes in the selected request
   flag. It does not establish an acquisition lease or infer a Ready state.
   Use only within the same outer ownership protocol as Acquire/Release. */
CR_STATUS CrRpmhRailVote(CONST CR_RPMH_RAIL_IO *, CR_RPMH_RAIL *, CR_RPMH_RAIL_VOTE_KIND, UINT32 Value);

/* Permanent provider baseline journal. Zero-initialize once and keep it with
 * the same graph for the entire provider lifetime. Identity is immutable from
 * the first attempt. The caller must prevent its owned reference (and graph
 * input references) from being released by ordinary consumer APIs. No release
 * operation exists: retained demand lasts until platform reset/OS reprogramming.
 */
typedef struct {
  CR_RPMH_RAIL *Rail;
  UINT32 VoltageMv;
  UINT8 Mode;
  BOOLEAN Owned;
} CR_RPMH_RAIL_RETAINED;
CR_STATUS CrRpmhRailRetain(CONST CR_RPMH_RAIL_IO *, CR_RPMH_RAIL *,
                          UINT32 VoltageMv, UINT8 Mode, CR_RPMH_RAIL_RETAINED *);
