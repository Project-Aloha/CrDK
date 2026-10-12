/* Pure USB native/role ownership boundaries. SPDX-License-Identifier: MIT */
#pragma once
#include <Interface/CrUsbHandoff.h>
#include <oskal/cr_status.h>

/* Seal/snapshot require caller-owned ordinary memory. No allocation, MMIO,
 * cache publication, OS RAM exclusion or ACPI installation is performed here.
 * Seal leaves its input unchanged on failure. Snapshot requires three disjoint
 * aligned buffers; failure leaves Out unchanged. Firmware publishes odd/even
 * with cache/barrier ordering separately. CRC detects damage, not authenticity.
 * A valid diagnostic UNCERTAIN snapshot remains unclaimable. */
UINT32 CrUsbHandoffCrc32(CONST CR_USB_NATIVE_HANDOFF *);
CR_STATUS CrUsbHandoffSeal(CR_USB_NATIVE_HANDOFF *,UINT64 EvenSequence);
CR_STATUS CrUsbHandoffValidate(CONST CR_USB_NATIVE_HANDOFF *);
CR_STATUS CrUsbHandoffSnapshot(CONST volatile VOID *,UINTN Bytes,
    CR_USB_NATIVE_HANDOFF *Scratch,CR_USB_NATIVE_HANDOFF *Out);
CR_STATUS CrUsbHandoffCanTakeover(CONST CR_USB_NATIVE_HANDOFF *,UINT64 BootGeneration);
CR_STATUS CrUsbControllerStopped(CONST CR_USB_CONTROLLER_STOP *);

typedef struct { UINT64 Incarnation,Assignment; UINT32 Role,Reserved; } CR_USB_ROLE_TOKEN;
typedef struct {
  VOID *ParentOwner,*ChildOwner;
  CR_USB_ROLE_TOKEN *Token;
  CR_USB_ROLE_TOKEN TokenImage;
  CR_USB_NATIVE_HANDOFF NativeRecord;
  UINT64 BootGeneration,Assignment,TransferId,NextIncarnation;
  UINT32 Role,PendingRole,Calls;
  BOOLEAN Initialized,Imported,Halted,Closing,Stopped,Quarantined;
} CR_USB_ROLE_BOUNDARY;

/* Zeroed permanent storage. Every call, including Enter/Leave, uses ONE
 * caller-owned serialization boundary (e.g. a short spinlock); these pure calls
 * never wait or invoke callbacks. Referenced owner/token storage must survive
 * failed closure and pending calls. This is not a Windows PnP driver.
 *
 * Native import requires actual OS reserved-RAM validation and producer source
 * identity outside this core. Assignment is the current immutable copied
 * raw/translated resource incarnation, not a descriptor-ready bit.
 *
 * BeginConfigure precedes the first provider/reset mutation. Failed configure
 * retains PendingRole; only the same request or actual cleanup may retire it.
 * Commit requires exact successful Configure plus an independently observed
 * stopped selected controller, and actual provider/PHY access checks outside.
 *
 * ReserveChild runs BEFORE completing lower START to the upper inbox driver;
 * it blocks all parent mutations even if upper START subsequently fails.
 * Stop represents rebalance only: exact child survives and can Restart.
 * Remove represents real top-down orderly REMOVE after upper resource release;
 * close/drain this filter before forwarding to the bus. BUSY is a private drain
 * result, never a failure returned for the mandatory-success PnP STOP/REMOVE.
 * D0 exit and EvtUrsSetRole do not call Remove. Surprise/assignment mismatch
 * must Quarantine, retaining storage/prerequisites without hardware access. */
CR_STATUS CrUsbRoleInit(CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner,
    UINT64 BootGeneration,UINT64 Assignment);
CR_STATUS CrUsbRoleImportNative(CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner,
    CONST CR_USB_NATIVE_HANDOFF *);
CR_STATUS CrUsbRoleCanHalt(CONST CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner);
CR_STATUS CrUsbRoleConfirmHalt(CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner,
    CONST CR_USB_CONTROLLER_STOP *);
CR_STATUS CrUsbRoleBeginConfigure(CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner,UINT32 Role);
CR_STATUS CrUsbRoleCommitConfigure(CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner,
    CR_STATUS ConfigureStatus,CONST CR_USB_CONTROLLER_STOP *);
CR_STATUS CrUsbRoleCleanupComplete(CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner,
    CR_STATUS ActualCleanupStatus);
CR_STATUS CrUsbRoleReserveChild(CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner,
    VOID *ChildOwner,UINT64 Assignment,UINT32 Role,CR_USB_ROLE_TOKEN *);
CR_STATUS CrUsbRoleEnter(CR_USB_ROLE_BOUNDARY *,VOID *ChildOwner,CONST CR_USB_ROLE_TOKEN *);
CR_STATUS CrUsbRoleLeave(CR_USB_ROLE_BOUNDARY *,VOID *ChildOwner,CONST CR_USB_ROLE_TOKEN *);
CR_STATUS CrUsbRoleStopChild(CR_USB_ROLE_BOUNDARY *,VOID *ChildOwner,CONST CR_USB_ROLE_TOKEN *);
CR_STATUS CrUsbRoleRestartChild(CR_USB_ROLE_BOUNDARY *,VOID *ChildOwner,
    CONST CR_USB_ROLE_TOKEN *,UINT64 Assignment);
CR_STATUS CrUsbRoleRemoveChild(CR_USB_ROLE_BOUNDARY *,VOID *ChildOwner,CR_USB_ROLE_TOKEN *);
CR_STATUS CrUsbRoleQuarantine(CR_USB_ROLE_BOUNDARY *,VOID *ParentOwner);
