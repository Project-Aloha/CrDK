/* Exclusive DSI0 configuration owner; kernel PASSIVE interface. MIT */
#pragma once
#include <wdm.h>
#include <Library/ClockExternalLib.h>
#define WDF_CLOCK_EXTERNAL_INTERFACE_REVISION 3U
DEFINE_GUID(GUID_CLOCK_EXTERNAL_INTERFACE,0x63d68b24,0x4473,0x450d,0x85,0x91,0x76,0x05,0xe1,0x6f,0xcc,0x58);
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Reserve)(VOID *,VOID *Owner);
  NTSTATUS (*Get)(VOID *,VOID *Owner,BOOLEAN *Enabled,BOOLEAN *Pending,UINT64 *Incarnation);
  NTSTATUS (*Set)(VOID *,VOID *Owner,UINT64 PixelHz,UINT64 ByteHz,BOOLEAN Enable);
  NTSTATUS (*Read)(VOID *,VOID *Owner,UINT32 Offset,UINT32 *Value);
  NTSTATUS (*ReleaseOwner)(VOID *,VOID *Owner);
  NTSTATUS (*NativeRetire)(VOID *,VOID *Owner,UINT64 TransferId,
      CONST CR_MDSS_NATIVE_HANDOFF *Record);
  NTSTATUS (*SetPlan)(VOID *,VOID *Owner,UINT32 Controller,
      CONST CR_CLOCK_DSI_PLAN *,BOOLEAN On,BOOLEAN *Owned);
  NTSTATUS (*GetPlan)(VOID *,VOID *Owner,UINT32 Controller,
      CR_CLOCK_DSI_PLAN *,BOOLEAN *On,BOOLEAN *Pending,UINT64 *Incarnation);
} WDF_CLOCK_EXTERNAL_INTERFACE;
/* Reserve before publishing any consumer. Owner is permanent nonpaged storage;
 * retain the referenced interface through all failed cleanup. All generic
 * legacy/owner writes to this group and its source ancestors are excluded.
 * A failed Set retains the reservation and hardware rollback state; retry
 * Set(false) before enabling again. Read is bounded/read-only and requires
 * actual access/domain references, never raw DISPCC access from consumers.
 * Get fails outside D0/removal; incarnation must match a held bus lease.
 */

/* NativeRetire is only for the root's already claimed immutable MDSH transfer,
 * after actual route/PHY/bridge fencing under its own power/access leases.
 * It grants no native claim or D0 by itself. Failed retirement retains those
 * leases, the exclusive reservation and exact record until the same owner
 * retries successfully. A completed transfer cannot be replayed against a
 * subsequently enabled Windows route. Keep the interface referenced through
 * final NativeRelease; ordinary Set(false) cannot retire firmware baselines.
 */
