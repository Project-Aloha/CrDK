/* Early ACPI installation gate; platform-selected library. GPL-2.0-only */
#pragma once
#include <Uefi.h>
#include <Protocol/AcpiTable.h>
/* Actual AcpiPlatform installer calls this helper for every IORT, before
 * InstallAcpiTable. The SM8450 implementation sets Handled FALSE only for a
 * bounded table without Adreno GPU routes, and TRUE for a GPU candidate even
 * on failure: callers must NEVER fall back to raw installation when TRUE.
 * Its DXE transaction installs immutable GPUH first and execution IORT last;
 * uncertainty retains allocations, clocks and the native owner for this boot.
 * The explicit Null implementation on other platforms always passes through
 * valid disjoint inputs (Handled FALSE, TableKey unchanged), without examining
 * routes or providing GPU admission/retirement authority.
 * Neither binding is an EBS callback or a function-driver protocol. */
EFI_STATUS CrAdrenoBootInstallIort(EFI_ACPI_TABLE_PROTOCOL *,CONST VOID *,UINTN,
 BOOLEAN *Handled,UINTN *TableKey);
