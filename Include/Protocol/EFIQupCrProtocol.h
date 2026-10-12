/** @file
 *  QUPv3 target resources and serial-engine ownership protocol.
 *
 *  Publishes Linux-derived resources and arbitrates native FIFO/GPI clients.
 *  The client holding a lease initializes and quiesces the controller.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/CrTargetQupLib.h>

#define EFI_QUP_CR_PROTOCOL_REVISION  0x0000000000010002ULL
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

/* All native clients must acquire the SE before mode/MMIO changes. A lease
   requires the entire SE aperture to have UC, writable CPU MMIO descriptors
   before the target's native core/config/memory interconnect votes. The
   platform creates those CPU mappings; this service never maps an unknown
   target address. Release
   only after GENI and DMA are idle.  Quiesced=FALSE permanently fences the SE
   for this boot; another client must not change mode behind outstanding DMA. */
typedef enum { QupCrSeFifo = 1, QupCrSeGpi = 2 } EFI_QUP_CR_SE_MODE;
typedef EFI_STATUS (EFIAPI *EFI_QUP_CR_ACQUIRE_SE)(
  IN EFI_QUP_CR_PROTOCOL *This, IN UINT8 WrapperId, IN UINT8 Engine,
  IN VOID *Owner, IN EFI_QUP_CR_SE_MODE Mode);
typedef EFI_STATUS (EFIAPI *EFI_QUP_CR_RELEASE_SE)(
  IN EFI_QUP_CR_PROTOCOL *This, IN UINT8 WrapperId, IN UINT8 Engine,
  IN VOID *Owner, IN BOOLEAN Quiesced);

typedef struct _EFI_QUP_CR_PROTOCOL {
  UINT64                    Revision;
  EFI_QUP_CR_GET_WRAPPER    GetWrapper;
  EFI_QUP_CR_GET_CONTEXT    GetContext;
  EFI_QUP_CR_ACQUIRE_SE      AcquireSe;
  EFI_QUP_CR_RELEASE_SE      ReleaseSe;
} EFI_QUP_CR_PROTOCOL;
