/* Referenced permanent PEP thermal service. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#include <Library/TsensCrLib.h>
#define WDF_TSENS_CR_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_TSENS_CR_INTERFACE,0x29c66f7a,0x4580,0x4db7,0x86,0xd8,0xf4,0x32,0x97,0x0c,0x27,0x1a);
DEFINE_GUID(GUID_DEVINTERFACE_TSENS_CR,0x4d569694,0x5c8d,0x4abc,0xaa,0x82,0x0d,0x2b,0x2f,0x2a,0xd8,0x0f);
typedef struct {PVOID Provider;UINT64 Generation;} CR_TSENS_LEASE;
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Begin)(PVOID,PVOID StableOwner,CR_TSENS_LEASE *);
  NTSTATUS (*End)(PVOID,PVOID StableOwner,CR_TSENS_LEASE *);
  NTSTATUS (*Read)(PVOID,PVOID StableOwner,CONST CR_TSENS_LEASE *,UINT32 Bank,UINT32 Sensor,CR_TSENS_SAMPLE *);
} WDF_TSENS_CR_INTERFACE;
/* Discover the permanent PEP TSENS alias then query this exact interface. Begin/
 * End PASSIVE; Read <=DISPATCH uses bounded MMIO only. Immutable owner/lease
 * storage identity pins D0/maps. Failed Read leaves output unchanged; milli-C
 * is a signed temperature, not a processor-performance feedback counter.
 * Quarantine closes reads with no register access, permits exact End/deref,
 * and permanently retains the instance until reboot. No threshold/calibration
 * programming or TSENS readiness is implied by ACPI descriptor publication. */
