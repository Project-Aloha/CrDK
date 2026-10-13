/* Parent bus's qualified PAS/SMEM/ARC/ICB binding. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#include <Library/PILCrLib.h>
#define WDF_PIL_PLATFORM_CR_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_PIL_PLATFORM_CR_INTERFACE,0x337b6b07,0xd9f3,0x4951,0x87,0x04,0x19,0xa2,0xe7,0x7c,0x4a,0x6c);
/* PASSIVE only. A successful Bind owns the returned nonpaged IO.Context until
   successful Unbind. Provider verifies board/PAS/carveout ownership, takes all
   underlying secure monitor/SMEM/CmdDB/RPMh/ICB references and SMEM pointer
   leases, and qualifies the three ARC/XO proxy keys BEFORE publication.
   ProxyWrite is an absolute vote for this Owner, with foreign votes preserved;
   Crypto path is private to this binding. No raw RPMh fallback is allowed.
   IO.Read32/Write32 may only access target QMP/IPCC mapped resources. SMC must
   return exact x0..x7, honor transport errors, and support interrupted standard
   calls. Unbind retries retained resource cleanup and refuses while PAS/ARC/
   ICB ownership remains. PilCr calls Unbind only after CrPilStop succeeds.
   Bind initializes output to zero. Failure either leaves no ownership/output,
   or returns a valid non-NULL IO.Context denoting retained ownership that must
   be recovered through Unbind before releasing the query reference.
   Bind and query failures are deployment failures, never readiness.
   Common/CrPilPlatform implements these consumer paths. It requires the trusted
   host's immutable verified handoff and translated MMIO grant; their absence
   remains a deployment failure, and must not enable ACPI PRDY. Bind independently
   checks actual Windows EL1 and nonmutating SCM/PAS service support; interrupted
   qualification retains its IO.Context and leases until Unbind retirement. */
typedef NTSTATUS (*PIL_PLATFORM_BIND)(PVOID,PVOID Owner,CONST CR_PIL_TARGET *,CR_PIL_IO *);
typedef NTSTATUS (*PIL_PLATFORM_UNBIND)(PVOID,PVOID Owner,CR_PIL_IO *);
typedef struct {
  INTERFACE Header;
  PIL_PLATFORM_BIND Bind;
  PIL_PLATFORM_UNBIND Unbind;
} WDF_PIL_PLATFORM_CR_INTERFACE;
