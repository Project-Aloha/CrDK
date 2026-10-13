/** @file
  Serialized, leased clients of one PMIC arbiter. SPDX-License-Identifier: MIT
**/
#pragma once
#include <Library/spmi.h>
#define CR_SPMI_MAX_CLIENTS 8
#define CR_SPMI_MAX_REGIONS 64
#define CR_SPMI_SHARED_REVISION 1
#define CR_SPMI_READ 1
#define CR_SPMI_WRITE 2
/* Rights are supplied by the board adapter, copied before publication. Clients
   cannot access IRQ control registers through Write/MaskedWrite. */
typedef struct { UINT8 Sid, Peripheral, Access, Irqs; } CR_SPMI_REGION;
typedef struct { VOID *Provider, *Owner; UINT64 Generation; } CR_SPMI_CLIENT;
typedef struct { VOID *Provider; UINT64 Generation; } CR_SPMI_IRQ;
typedef struct {
  VOID *Context; UINT64 Generation;
  /* Pass the exact original CR_SPMI_BUS address as the first argument. */
  CR_STATUS (*Read)(VOID *,UINT8,UINT16,UINT8 *,UINTN);
  CR_STATUS (*Write)(VOID *,UINT8,UINT16,CONST UINT8 *,UINTN);
  CR_STATUS (*MaskedWrite)(VOID *,UINT8,UINT16,CONST UINT8 *,CONST UINT8 *,UINTN);
  CR_STATUS (*Subscribe)(VOID *,UINT8,UINT8,UINT8,CR_INTERRUPT_TRIGGER_TYPE,
                         CR_INTERRUPT_HANDLER,VOID *,CR_SPMI_IRQ *);
  CR_STATUS (*Enable)(VOID *,CR_SPMI_IRQ *,BOOLEAN);
  CR_STATUS (*Unsubscribe)(VOID *,CR_SPMI_IRQ *);
  CR_STATUS (*Check)(VOID *);
  CR_STATUS (*BeginRetire)(VOID *);
} CR_SPMI_BUS;
typedef struct CR_SPMI_SHARED CR_SPMI_SHARED;
typedef struct {
  CR_SPMI_SHARED *Provider; CR_SPMI_CLIENT *Identity; CR_SPMI_BUS *BusIdentity; VOID *Owner;
  UINT64 Generation; CR_SPMI_REGION Regions[CR_SPMI_MAX_REGIONS]; UINT8 RegionCount; BOOLEAN Retiring;
} CR_SPMI_OWNER;
typedef struct {
  CR_SPMI_SHARED *Provider; CR_SPMI_OWNER *Owner; CR_SPMI_IRQ *Identity;
  UINT64 Generation; UINT8 Sid,Peripheral,Bit,Snapshot[4]; UINT16 Apid;
  CR_INTERRUPT_HANDLER Handler; VOID *Context;
  BOOLEAN Saved,Registered,Invoking;
} CR_SPMI_SUBSCRIPTION;
struct CR_SPMI_SHARED {
  SpmiDeviceContext *Spmi; UINT64 NextGeneration; BOOLEAN Initialized,Closed,Dispatching;
  CR_SPMI_OWNER Owners[CR_SPMI_MAX_CLIENTS];
  CR_SPMI_SUBSCRIPTION Irqs[SPMI_PMIC_ARB_MAX_IRQ_ENTRIES];
  struct { UINT16 Apid,References; UINT32 Initial; } Pic[SPMI_PMIC_ARB_MAX_IRQ_ENTRIES];
};
/* Every entry point including Dispatch must be serialized by the OS adapter.
   Native excludes IRQs with the OSKAL HIGH guard. NT uses its deferred delivery
   and worker rundown. Callback only wakes work; it cannot reenter this core.
   Tokens must remain at their original addresses until exact-success retirement.
   On partial Subscribe failure an issued token remains owned and must be retried
   through Unsubscribe. Failed cleanup retains all callback/provider storage. */
CR_STATUS CrSpmiSharedInit(CR_SPMI_SHARED *,SpmiDeviceContext *);
CR_STATUS CrSpmiSharedAcquire(CR_SPMI_SHARED *,VOID *,CONST CR_SPMI_REGION *,UINTN,
                              CR_SPMI_CLIENT *,CR_SPMI_BUS *);
CR_STATUS CrSpmiSharedRelease(CR_SPMI_SHARED *,CR_SPMI_CLIENT *);
CR_STATUS CrSpmiSharedDispatch(CR_SPMI_SHARED *);
CR_STATUS CrSpmiSharedClose(CR_SPMI_SHARED *);
CR_STATUS CrSpmiSharedUninit(CR_SPMI_SHARED *);
