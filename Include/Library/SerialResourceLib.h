/* Fixed serial resource transaction, shared by OS frontends. MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_SERIAL_RESOURCE_GROUPS 3U
#define CR_SERIAL_RESOURCE_CLOCK 0U
#define CR_SERIAL_RESOURCE_ICB 1U
#define CR_SERIAL_RESOURCE_GPIO 2U
typedef struct {
  VOID *Context;
  /* No mutation/allocation in Validate or Reserve. Reserve may commit fixed
     metadata before reporting error; Release must handle that exact owner. */
  CR_STATUS (*Validate)(VOID *,UINT32 Group,UINT32 Index);
  CR_STATUS (*Reserve)(VOID *,VOID *Owner,UINT32 Group,UINT32 Index);
  CR_STATUS (*Begin)(VOID *,VOID *Owner,BOOLEAN *Owned);
  CR_STATUS (*Apply)(VOID *,VOID *Owner,UINT32 Group,UINT32 Index);
  CR_STATUS (*Release)(VOID *,VOID *Owner,UINT32 Group);
  CR_STATUS (*End)(VOID *,VOID *Owner);
  BOOLEAN (*Check)(VOID *,VOID *Owner);
} CR_SERIAL_RESOURCE_IO;
typedef struct {
  CR_SERIAL_RESOURCE_IO Io;
  VOID *Owner;
  UINT32 Counts[CR_SERIAL_RESOURCE_GROUPS];
  BOOLEAN Groups[CR_SERIAL_RESOURCE_GROUPS],Lease,Ready,Retiring,Quarantined;
} CR_SERIAL_RESOURCES;
/* Caller-owned zeroed nonpaged storage and stable owner. PASSIVE, externally
 * serialized. Commit every ownership frame before fallible callbacks; outputs
 * and prerequisites survive failed startup/retirement. Same Owner+Io cannot
 * be replaced until exact End succeeds. All metadata is reserved before the
 * first Apply. Hardware Check never implies protocol firmware/engine/IRQ proof.
 * Caller fences and drains its engine/IRQ/work before Retire. Quarantine is
 * software-only, irreversible and retains hardware/owner state for diagnosis. */
CR_STATUS CrSerialResourcesStart(CR_SERIAL_RESOURCES *,CONST CR_SERIAL_RESOURCE_IO *,VOID *Owner,CONST UINT32 Counts[CR_SERIAL_RESOURCE_GROUPS]);
CR_STATUS CrSerialResourcesRetire(CR_SERIAL_RESOURCES *);
VOID CrSerialResourcesQuarantine(CR_SERIAL_RESOURCES *);
BOOLEAN CrSerialResourcesReady(CONST CR_SERIAL_RESOURCES *);
