/** Native PIL status. Firmware remains resident through Shell. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/PILCrClients.h>
#define EFI_PIL_CR_PROTOCOL_REVISION 0x10001ULL
#define EFI_PIL_CR_PROTOCOL_GUID \
  {0xe41b50a7, 0xe8f2, 0x4a98, {0x84, 0x22, 0x7e, 0x36, 0x9a, 0x8d, 0xac, 0x9a}}
extern EFI_GUID gEfiPilCrProtocolGuid;
typedef struct _EFI_PIL_CR_PROTOCOL EFI_PIL_CR_PROTOCOL;
typedef struct {
  BOOLEAN Ready, Owned, Exiting;
  EFI_STATUS LastError;
  CONST CHAR8 *Stage;
} EFI_PIL_CR_STATE;
typedef EFI_STATUS (EFIAPI *EFI_PIL_CR_GET_STATE)(
  EFI_PIL_CR_PROTOCOL *This, EFI_PIL_CR_STATE *State);
/* Register before starting an ADSP writer/IRQ. Registration pins this PIL
   provider against Stop/unload. Unregister only after synchronous retirement.
   At ExitBootServices all clients are quiesced before any PAS shutdown. */
typedef EFI_STATUS (EFIAPI *EFI_PIL_CR_REGISTER_CLIENT)(
  EFI_PIL_CR_PROTOCOL *This, VOID *Owner, CR_PIL_CLIENT_QUIESCE Quiesce, VOID *Context);
typedef EFI_STATUS (EFIAPI *EFI_PIL_CR_UNREGISTER_CLIENT)(
  EFI_PIL_CR_PROTOCOL *This, VOID *Owner);
struct _EFI_PIL_CR_PROTOCOL {
  UINT64 Revision;
  EFI_PIL_CR_GET_STATE GetState;
  EFI_PIL_CR_REGISTER_CLIENT RegisterClient;
  EFI_PIL_CR_UNREGISTER_CLIENT UnregisterClient;
};
