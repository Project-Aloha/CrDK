/** @file
 *  Read-only GPI DMA target resource protocol.
 *
 *  The MU GpiDxe owns DMA programming.  Crane consumers use this protocol to
 *  discover the Linux-compatible controller/SID/IRQ contract.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Protocol/EFIMuBusProtocol.h>
#include <Library/CrTargetQupLib.h>

#define EFI_GPI_CR_PROTOCOL_REVISION  0x0000000000010001ULL
#define EFI_GPI_CR_PROTOCOL_GUID \
  { 0x9f2f4a76, 0x5c8e, 0x4a0e, { 0x91, 0x43, 0x2b, 0x70, 0x8c, 0x6d, 0x4e, 0x12 } }

extern EFI_GUID gEfiGpiCrProtocolGuid;
typedef struct _EFI_GPI_CR_PROTOCOL EFI_GPI_CR_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GPI_CR_GET_CONTROLLER)(
  IN  EFI_GPI_CR_PROTOCOL               *This,
  IN  UINT8                              ControllerId,
  OUT CONST CR_TARGET_GPI_CONTROLLER **Controller
  );

typedef EFI_STATUS (EFIAPI *EFI_GPI_CR_GET_CONTEXT)(
  IN  EFI_GPI_CR_PROTOCOL       *This,
  OUT CONST CR_TARGET_QUP_CONTEXT **Context
  );

typedef EFI_STATUS (EFIAPI *EFI_GPI_CR_GET_MU_PROTOCOL)(
  IN  EFI_GPI_CR_PROTOCOL *This,
  OUT MU_GPI_PROTOCOL     **Protocol
  );

typedef struct _EFI_GPI_CR_PROTOCOL {
  UINT64                         Revision;
  EFI_GPI_CR_GET_CONTROLLER     GetController;
  EFI_GPI_CR_GET_CONTEXT        GetContext;
  EFI_GPI_CR_GET_MU_PROTOCOL    GetMuProtocol;
  MU_GPI_PROTOCOL              *MuProtocol;
} EFI_GPI_CR_PROTOCOL;
