/* Actual ACPI table reader and DXE exit diagnostic against FV/Boot Services
 * contracts. This fixture does not emulate an ACPI operating system.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <PiDxe.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/FirmwareVolume2.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <IndustryStandard/Acpi.h>

STATIC EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
EFI_GUID gEfiFirmwareVolume2ProtocolGuid = { .Data1 = 1 };
EFI_GUID gEfiAcpiTableProtocolGuid = { .Data1 = 2 };
STATIC EFI_GUID Storage = { .Data1 = 3 };
#undef PcdGetPtr
#define PcdGetPtr(Token) (&Storage)
STATIC EFI_FIRMWARE_VOLUME2_PROTOCOL Volume;
STATIC EFI_ACPI_TABLE_PROTOCOL Acpi;
STATIC UINTN Count, Reads, Installed, Allocated, Freed, DebugCalls, DebugLevel;
STATIC INTN FailAt = -1;
STATIC EFI_STATUS ReadFailure, InstallFailure, LocateFailure;
STATIC CONST CHAR8 *LastFormat;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { return TRUE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {
  DebugCalls++; DebugLevel = Level; LastFormat = Format;
}
VOID EFIAPI DebugAssert(CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description) {
  fprintf(stderr, "native ASSERT %s:%zu: %s\n", File, (size_t)Line, Description);
  abort();
}
UINT8 EFIAPI CalculateCheckSum8(CONST UINT8 *Buffer, UINTN Size) {
  UINT8 Sum = 0; while (Size--) Sum += *Buffer++; return (UINT8)(0 - Sum);
}
STATIC EFI_STATUS EFIAPI FreeBuffer(VOID *Buffer) {
  assert(Buffer); Freed++; free(Buffer); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Registration, VOID **Out) {
  assert(Guid == &gEfiAcpiTableProtocolGuid);
  *Out = &Acpi; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI LocateHandles(EFI_LOCATE_SEARCH_TYPE Search, EFI_GUID *Guid,
                                      VOID *Key, UINTN *Number, EFI_HANDLE **Handles) {
  assert(Search == ByProtocol && Guid == &gEfiFirmwareVolume2ProtocolGuid);
  if (EFI_ERROR(LocateFailure)) return LocateFailure;
  *Handles = malloc(sizeof(**Handles)); assert(*Handles); Allocated++;
  (*Handles)[0] = &Volume; *Number = 1; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Protocol(EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Out) {
  assert(Handle == &Volume && Guid == &gEfiFirmwareVolume2ProtocolGuid);
  *Out = &Volume; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ReadFile(CONST EFI_FIRMWARE_VOLUME2_PROTOCOL *This,
                                 CONST EFI_GUID *Name, VOID **Buffer, UINTN *Size,
                                 EFI_FV_FILETYPE *Type, EFI_FV_FILE_ATTRIBUTES *Attributes,
                                 UINT32 *Authentication) {
  assert(This == &Volume && Name == &Storage && !Buffer && *Size == 0);
  *Size = Count * sizeof(EFI_ACPI_DESCRIPTION_HEADER);
  *Type = EFI_FV_FILETYPE_FREEFORM; *Attributes = 0; *Authentication = 0;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ReadSection(CONST EFI_FIRMWARE_VOLUME2_PROTOCOL *This,
                                    CONST EFI_GUID *Name, EFI_SECTION_TYPE Type,
                                    UINTN Instance, VOID **Out, UINTN *Size,
                                    UINT32 *Authentication) {
  assert(This == &Volume && Name == &Storage && Type == EFI_SECTION_RAW);
  assert(*Out == NULL);
#ifndef TEST_BEFORE_TERM
  assert(*Size == 0);
#endif
  assert(Instance == Reads++);
  if ((INTN)Instance == FailAt) return ReadFailure;
  if (Instance == Count) return EFI_NOT_FOUND;
  assert(Instance < Count);
  EFI_ACPI_DESCRIPTION_HEADER *Table = calloc(1, sizeof(*Table)); assert(Table);
  Allocated++; Table->Length = sizeof(*Table); Table->Revision = 1;
  Table->Signature = SIGNATURE_32('T', 'E', 'S', (CHAR8)('0' + Instance));
  Table->Checksum = 0x7f; *Out = Table; *Size = sizeof(*Table); *Authentication = 0;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Install(EFI_ACPI_TABLE_PROTOCOL *This, VOID *Table,
                                UINTN Size, UINTN *Key) {
  assert(This == &Acpi && Size == sizeof(EFI_ACPI_DESCRIPTION_HEADER));
  assert(((EFI_ACPI_DESCRIPTION_HEADER *)Table)->Length == Size);
  assert(CalculateCheckSum8(Table, Size) == 0); assert(*Key == 0);
  if (EFI_ERROR(InstallFailure)) return InstallFailure;
  *Key = ++Installed; return EFI_SUCCESS;
}

#include "AcpiPlatformTerminationProduction.c"

/* Only the actual native diagnostic block is extracted. Image allocation,
 * Exit/StartImage and CoreUnloadAndCloseImage remain outside this fixture. */
STATIC VOID Diagnose(EFI_STATUS Status) {
  struct { EFI_STATUS Status; struct { VOID *ImageBase; } Info; } Value = {
    .Status = Status, .Info.ImageBase = (VOID *)0x12340000
  };
  typeof(Value) *Image = &Value;
#include "DxeImageExitDiagnostic.c"
}

int main(int Argc, char **Argv) {
  assert(Argc == 2);
  Services.FreePool = FreeBuffer; Services.LocateProtocol = Locate;
  Services.LocateHandleBuffer = LocateHandles; Services.HandleProtocol = Protocol;
  Volume.ReadFile = ReadFile; Volume.ReadSection = ReadSection;
  Acpi.InstallAcpiTable = Install;
  CONST char *Mode = Argv[1]; EFI_STATUS Expected = EFI_SUCCESS;
  if (!strcmp(Mode, "normal")) { Count = 3; Expected = EFI_REQUEST_UNLOAD_IMAGE; }
  else if (!strcmp(Mode, "empty")) Expected = EFI_NOT_FOUND;
  else if (!strcmp(Mode, "first-read-error")) {
    Count = 3; FailAt = 0; ReadFailure = EFI_DEVICE_ERROR; Expected = ReadFailure;
  } else if (!strcmp(Mode, "later-read-error")) {
    Count = 3; FailAt = 1; ReadFailure = EFI_OUT_OF_RESOURCES; Expected = ReadFailure;
  } else if (!strcmp(Mode, "install-error")) {
    Count = 3; InstallFailure = EFI_ACCESS_DENIED; Expected = EFI_ABORTED;
  } else if (!strcmp(Mode, "locate-error")) {
    LocateFailure = EFI_NOT_FOUND; Expected = EFI_ABORTED;
  } else if (!strcmp(Mode, "unload-diagnostic")) {
    Diagnose(EFI_REQUEST_UNLOAD_IMAGE);
    assert(DebugCalls == 1 && !(DebugLevel & DEBUG_ERROR));
    assert(strstr(LastFormat, "requested unload")); puts("intentional unload diagnostic PASS"); return 0;
  } else if (!strcmp(Mode, "error-diagnostic")) {
    Diagnose(EFI_DEVICE_ERROR); assert(DebugCalls == 1 && DebugLevel == DEBUG_ERROR);
    assert(strstr(LastFormat, "start failed")); puts("real error diagnostic PASS"); return 0;
  } else if (!strcmp(Mode, "success-diagnostic")) {
    Diagnose(EFI_SUCCESS); assert(!DebugCalls); puts("successful exit diagnostic PASS"); return 0;
  } else assert(!"unknown mode");
  EFI_STATUS Status = AcpiPlatformEntryPoint((VOID *)1, (VOID *)2);
  assert(Status == Expected); assert(Allocated == Freed);
  if (!strcmp(Mode, "normal")) {
    assert(Installed == 3 && Reads == 4);
#ifndef TEST_BEFORE_TERM
    assert(DebugCalls == 1 && DebugLevel == DEBUG_INIT);
    assert(strstr(LastFormat, "installed %u RAW tables"));
#endif
  }
  if (!strcmp(Mode, "later-read-error")) assert(Installed == 1 && Reads == 2);
  if (!strcmp(Mode, "empty") || !strcmp(Mode, "first-read-error")) assert(!Installed && Reads == 1);
  printf("actual ACPI %s PASS installed=%zu reads=%zu\n", Mode, (size_t)Installed, (size_t)Reads);
  return 0;
}
