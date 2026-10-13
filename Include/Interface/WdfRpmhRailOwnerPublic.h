/* Reserved absolute regulator owner ABI. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#include <Library/RpmhRailVoteLib.h>
#define WDF_RPMH_RAIL_OWNER_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_RPMH_RAIL_OWNER_INTERFACE, 0x6b20d52f, 0xa405, 0x47c0,
            0xb7, 0xd3, 0x86, 0x11, 0x09, 0x5f, 0xad, 0x31);

/* PASSIVE, kernel only; retain the referenced interface and stable nonpaged
   owner through failed cleanup. Names and values describe qualified cached
   VRM supplies: voltage is mV and mode is the hardware selector. Reserve is
   pure and claims capacity before publication; Get/Set require a reservation.
   Repeated Set is absolute. Pending gates hardware readiness even when this
   owner's logical value is absent. Outside provider D0 all operations decline.
   Failed ReleaseOwner never permits the consumer to retire its owner token.
   Query through GUID_DEVINTERFACE_RPMH_CR; legacy balanced APIs keep their ABI. */
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Reserve)(VOID *, VOID *Owner, CONST CHAR8 *Name, UINT32 VoltageMv, UINT8 Mode);
  NTSTATUS (*Validate)(VOID *, CONST CHAR8 *Name, UINT32 VoltageMv, UINT8 Mode);
  NTSTATUS (*Get)(VOID *, VOID *Owner, CONST CHAR8 *Name, CR_RPMH_RAIL_VALUE *, BOOLEAN *Pending);
  NTSTATUS (*Set)(VOID *, VOID *Owner, CONST CHAR8 *Name, CONST CR_RPMH_RAIL_VALUE *);
  NTSTATUS (*ReleaseOwner)(VOID *, VOID *Owner);
} WDF_RPMH_RAIL_OWNER_INTERFACE;
