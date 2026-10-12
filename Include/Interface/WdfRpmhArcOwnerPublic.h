/* Referenced ARC/XO absolute owner ABI. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#include <Library/RpmhArcVoteLib.h>
#define WDF_RPMH_ARC_OWNER_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_RPMH_ARC_OWNER_INTERFACE,0x23bb35e3,0x68c6,0x4b49,0x9a,0x23,0x02,0xd5,0x78,0xc1,0x06,0xe7);
/* PASSIVE. Query reference/D0 dependency precede use. Stable nonpaged Owner
   survives reservations/recovery. Reserve all owner/resource slots before
   publication. Describe/Resolve/Get are pure; zero/absent keeps its slot.
   Corner is a hardware table INDEX (XO0/3), never a voltage. Active state only.
   Fresh hardware is unknown; Get Pending remains true until explicit qualified
   Set. Failed writes preserve old logical demand and retained uncertain state.
   ReleaseOwner only after callbacks/PAS/journals drain; retry failure with the
   exact same interface/Owner. Provider shutdown must retain failed resources. */
typedef NTSTATUS (*RPMH_ARC_DESCRIBE)(PVOID,CONST CHAR8 *,CR_RPMH_ARC_RESOURCE *);
typedef NTSTATUS (*RPMH_ARC_RESOLVE)(PVOID,CONST CHAR8 *,UINT32,UINT32 *);
typedef NTSTATUS (*RPMH_ARC_RESERVE)(PVOID,PVOID,CONST CHAR8 *);
typedef NTSTATUS (*RPMH_ARC_GET)(PVOID,PVOID,CONST CHAR8 *,CR_RPMH_ARC_VALUE *,BOOLEAN *);
typedef NTSTATUS (*RPMH_ARC_SET)(PVOID,PVOID,CONST CHAR8 *,CONST CR_RPMH_ARC_VALUE *);
typedef NTSTATUS (*RPMH_ARC_RELEASE)(PVOID,PVOID);
typedef struct {
  INTERFACE Header;
  RPMH_ARC_DESCRIBE Describe;
  RPMH_ARC_RESOLVE ResolveLevel;
  RPMH_ARC_RESERVE Reserve;
  RPMH_ARC_GET Get;
  RPMH_ARC_SET Set;
  RPMH_ARC_RELEASE ReleaseOwner;
} WDF_RPMH_ARC_OWNER_INTERFACE;
