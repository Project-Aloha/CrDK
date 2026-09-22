/** @file
 *  Read-only QUPv3 target resource protocol.
 *
 *  QUP hardware is initialized by the MU QUP/GENI stack.  This protocol
 *  publishes the target resource contract to Crane consumers without taking
 *  ownership of the controller registers.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/CrTargetQupLib.h>

#define EFI_QUP_CR_PROTOCOL_REVISION  0x0000000000010000ULL
#define EFI_QUP_CR_PROTOCOL_GUID \
  { 0x2a4d8f31, 0x6f7a, 0x4ec0, { 0x8d, 0x2a, 0xb1, 0x17, 0x6e, 0x93, 0x54, 0x01 } }

extern EFI_GUID gEfiQupCrProtocolGuid;
typedef struct _EFI_QUP_CR_PROTOCOL EFI_QUP_CR_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_QUP_CR_GET_WRAPPER)(
  IN  EFI_QUP_CR_PROTOCOL          *This,
  IN  UINT8                         WrapperId,
  OUT CONST CR_TARGET_QUP_WRAPPER **Wrapper
  );

typedef EFI_STATUS (EFIAPI *EFI_QUP_CR_GET_CONTEXT)(
  IN  EFI_QUP_CR_PROTOCOL      *This,
  OUT CONST CR_TARGET_QUP_CONTEXT **Context
  );

typedef struct _EFI_QUP_CR_PROTOCOL {
  UINT64                    Revision;
  EFI_QUP_CR_GET_WRAPPER    GetWrapper;
  EFI_QUP_CR_GET_CONTEXT    GetContext;
} EFI_QUP_CR_PROTOCOL;
