/* Native ACPI locator/final evidence publication. Excluded from CRDK export. MIT */
#pragma once
#include <Library/PILCrUefiLib.h>
#include <Library/PILCrClients.h>
#include <Interface/CrPilHandoff.h>
#include <Protocol/AcpiTable.h>

typedef struct {
  EFI_PHYSICAL_ADDRESS Record;
  UINT64 Generation;
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN MapCapacity, TableKey;
  EFI_ACPI_TABLE_PROTOCOL *Acpi;
  BOOLEAN RecordOwned, Installed, Published;
} CR_PIL_UEFI_HANDOFF;
/* Zero initialize once. Prepare runs before EBS; failed preparation retains
   allocations for retry/release. Publish allocates/frees nothing and is called
   only by the terminal EBS path after synchronous client/PAS retirement. */
EFI_STATUS CrPilHandoffPrepare(CR_PIL_UEFI_HANDOFF *State);
EFI_STATUS CrPilHandoffRelease(CR_PIL_UEFI_HANDOFF *State);
EFI_STATUS CrPilHandoffPublish(CR_PIL_UEFI_HANDOFF *State,
                               CONST CR_PIL_CONTEXT *Pil,
                               CONST CR_PIL_CLIENTS *Clients,
                               EFI_RPMH_CR_PROTOCOL *Rpmh);
