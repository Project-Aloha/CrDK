/* Inactive final native UART publisher. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/UartHandoffLib.h>
#include <Protocol/AcpiTable.h>

#define CR_UART_HANDOFF_LOADED_HANDLES 1024U
typedef struct {
  EFI_SYSTEM_TABLE *System;
  EFI_HANDLE Image, SourceVolume;
  EFI_PHYSICAL_ADDRESS SourceFv, Record;
  UINTN SourceBytes, MapCapacity, TableKey;
  UINT64 Generation;
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_HANDLE *Handles;
  CR_UART_ADMISSION *Gate;
  EFI_ACPI_TABLE_PROTOCOL *Acpi;
  CR_UART_FIRMWARE_COVERAGE Coverage;
  CR_UART_FIRMWARE_IMAGE Loaded[CR_UART_HANDOFF_IMAGES];
  CR_UART_RETIRE_OBSERVATION Observation;
  UINT32 LoadedCount;
  EFI_STATUS Status;
  BOOLEAN CoverageOwned, RecordOwned, TableOwned, Installed, Preparing, Final;
} CR_UART_UEFI_HANDOFF;

/* Initialize state once to zero. The Image is this boot-only publisher's own
 * actual loaded image; its FV source qualifies every runtime image. Prepare
 * copies immutable generated coverage and preallocates ALL terminal storage.
 * Partial failures retain exact storage/source identity for retry/Release.
 * A nonzero key returned by this platform's ACPI implementation on a failed
 * Install still owns a possibly published table. Release must successfully
 * uninstall that exact key before freeing the referenced record; Prepare
 * refuses duplicate installation while that cleanup remains outstanding.
 * Caller owns live image+state, serializes at <=TPL_NOTIFY, and handles its
 * real ReadyToBoot/EBS events. No activation or always-ready protocol here. */
EFI_STATUS CrUartHandoffUefiPrepare(CR_UART_UEFI_HANDOFF *, EFI_HANDLE Image,
                                  EFI_SYSTEM_TABLE *, CONST CR_UART_FIRMWARE_COVERAGE *);
EFI_STATUS CrUartHandoffUefiRelease(CR_UART_UEFI_HANDOFF *);
/* Called ONLY from actual terminal EBS group, after successful MapKey. Closes
 * shared admission irreversibly; no allocation/map mutation/BS event/protocol
 * removal. Final diagnostic and exact pages/state are retained permanently.
 * Repeated call is purely idempotent. Failed stale-MapKey EBS must never call
 * this function. Keep each runtime writer's private EBS event alive as well. */
EFI_STATUS CrUartHandoffUefiFinalize(CR_UART_UEFI_HANDOFF *);
