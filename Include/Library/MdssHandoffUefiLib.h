/* UEFI storage/publication adapter; portable validation is in MdssHandoffCrLib. MIT */
#pragma once
#include <Protocol/EFIMdssHandoffProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/AcpiTable.h>

#define CR_MDSS_HANDOFF_RESERVATION_BYTES 8192U
typedef struct {
  EFI_PHYSICAL_ADDRESS Record;
  UINT64 Generation, TransferId;
  EFI_ACPI_TABLE_PROTOCOL *Acpi;
  EFI_MEMORY_DESCRIPTOR *MemoryMap;
  UINTN MapCapacity, TableKey;
  BOOLEAN RecordOwned, Installed, Published, Freezing, Frozen, SmmuComplete;
  EFI_STATUS FreezeStatus, PublishStatus;
  struct {
    UINT32 Owner;
    VOID *Context;
    EFI_MDSS_HANDOFF_FREEZE Freeze;
    EFI_MDSS_HANDOFF_SNAPSHOT Snapshot;
    BOOLEAN Frozen;
  } Producers[4];
  CR_MDSS_NATIVE_HANDOFF Local;
  CR_MDSS_HANDOFF_PARTICIPANT Participant;
  EFI_SMMU_CR_MAPPING_EVIDENCE Evidence;
  VOID *Mappings[CR_MDSS_NATIVE_ROUTES];
} CR_MDSS_UEFI_HANDOFF;

EFI_STATUS CrMdssUefiSetParticipant(CR_MDSS_UEFI_HANDOFF *,UINT32 Owner,VOID *,
  EFI_MDSS_HANDOFF_FREEZE,EFI_MDSS_HANDOFF_SNAPSHOT);
EFI_STATUS CrMdssUefiPrepare(CR_MDSS_UEFI_HANDOFF *);
EFI_STATUS CrMdssUefiRelease(CR_MDSS_UEFI_HANDOFF *);
EFI_STATUS CrMdssUefiFreeze(CR_MDSS_UEFI_HANDOFF *);
/* After the SMMU completion observer; no allocation/free or hardware write. */
EFI_STATUS CrMdssUefiPublish(CR_MDSS_UEFI_HANDOFF *,EFI_SMMU_CR_PROTOCOL *);
