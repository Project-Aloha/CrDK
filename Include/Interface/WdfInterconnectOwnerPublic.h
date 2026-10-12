/* Reserved absolute bandwidth-owner ABI. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#include <Library/InterconnectVoteLib.h>
#define WDF_INTERCONNECT_OWNER_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_INTERCONNECT_OWNER_INTERFACE, 0x9c31f20b, 0xd486, 0x4f02,
            0xa7, 0x65, 0x6a, 0xc2, 0xa9, 0x4e, 0x51, 0x83);

/* PASSIVE, kernel only; retain the query interface reference. Stable opaque
   nonpaged Owner and immutable Path.Provider outlive reservations and failed
   cleanup. Reserve all paths before PEP publication. Validate/Get are pure,
   without capacity allocation or RPMh recovery. Get preserves logical demand
   through uncertain hardware writes; Pending prevents consumers from assuming
   D0 readiness. Set uses absolute Linux kB/s values and retained retry. Absent
   demand retains the reservation, enabling later pure qualification. Release
   only after journal Close and callback drain; failure retains the handle.
   Outside provider D0/removal all operations fail unavailable. A permanent
   broker must keep desired demands independently for a new incarnation. */
typedef NTSTATUS (*INTERCONNECT_OWNER_RESERVE)(VOID *, VOID *Owner, CONST CR_ICB_PATH *);
typedef NTSTATUS (*INTERCONNECT_OWNER_VALIDATE)(VOID *, VOID *Owner, CONST CR_ICB_PATH *);
typedef NTSTATUS (*INTERCONNECT_OWNER_GET)(VOID *, VOID *Owner, CONST CR_ICB_PATH *,
                                          CR_ICB_VOTE_VALUE *, BOOLEAN *Pending);
typedef NTSTATUS (*INTERCONNECT_OWNER_SET)(VOID *, VOID *Owner, CONST CR_ICB_PATH *,
                                          CONST CR_ICB_VOTE_VALUE *);
typedef NTSTATUS (*INTERCONNECT_OWNER_RELEASE)(VOID *, VOID *Owner);
typedef struct {
  INTERFACE Header;
  INTERCONNECT_OWNER_RESERVE Reserve;
  INTERCONNECT_OWNER_VALIDATE Validate;
  INTERCONNECT_OWNER_GET Get;
  INTERCONNECT_OWNER_SET Set;
  INTERCONNECT_OWNER_RELEASE ReleaseOwner;
} WDF_INTERCONNECT_OWNER_INTERFACE;
