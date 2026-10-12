/* Read-only DAL-owned ABL device tree. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/AndroidBootCrLib.h>

#define EFI_CR_FDT_PROTOCOL_REVISION 1U
#define EFI_CR_FDT_PROTOCOL_GUID \
  {0x3f056bd0,0xc1fd,0x4e9f,{0xa8,0x32,0x50,0x6b,0xa3,0x9c,0x2b,0x2c}}
extern EFI_GUID gEfiCrFdtProtocolGuid;
typedef struct _EFI_CR_FDT_PROTOCOL EFI_CR_FDT_PROTOCOL;

/* Returned blob/property storage is immutable and belongs to CrDALDxe until
 * ExitBootServices. Callers must not free or edit it. Node offsets are obtained
 * through this instance; missing/invalid data returns an EFI error. No write,
 * overlay or original bootloader-memory access is exposed by this protocol. */
struct _EFI_CR_FDT_PROTOCOL {
  UINT32 Revision;
  UINT32 Size;
  EFI_STATUS (EFIAPI *GetBlob)(IN EFI_CR_FDT_PROTOCOL *This,
                             OUT CONST VOID **Blob, OUT UINTN *Size);
  EFI_STATUS (EFIAPI *FindNode)(IN EFI_CR_FDT_PROTOCOL *This,
                              IN CONST CHAR8 *Path, OUT INT32 *Node);
  EFI_STATUS (EFIAPI *FindCompatible)(IN EFI_CR_FDT_PROTOCOL *This,
                                    IN INT32 AfterNode,
                                    IN CONST CHAR8 *Compatible, OUT INT32 *Node);
  EFI_STATUS (EFIAPI *GetProperty)(IN EFI_CR_FDT_PROTOCOL *This,
                                 IN INT32 Node, IN CONST CHAR8 *Name,
                                 OUT CONST VOID **Data, OUT UINTN *Size);
  /* Copies the DXE-parsed decision. Output is cleared on error; UNKNOWN is a
   * successful absence of a mode, not a substitute for a failed capture. */
  EFI_STATUS (EFIAPI *GetBootInfo)(IN EFI_CR_FDT_PROTOCOL *This,
                                 OUT CR_ANDROID_BOOT_INFO *Info);
};
