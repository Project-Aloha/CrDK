/** @file
 *  Linux-compatible GPI DMA target and I2C transfer protocol.
 *
 *  GpiCrDxe owns the explicitly supported Linux GPI-DMA endpoint.  The
 *  transfer entry point is intentionally separate from PI I2C because one
 *  GENI serial engine can only be in FIFO or GPI mode at a time.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/CrTargetQupLib.h>

#define EFI_GPI_CR_PROTOCOL_REVISION  0x0000000000010002ULL
#define EFI_GPI_CR_PROTOCOL_GUID \
  { 0x9f2f4a76, 0x5c8e, 0x4a0e, { 0x91, 0x43, 0x2b, 0x70, 0x8c, 0x6d, 0x4e, 0x12 } }

extern EFI_GUID gEfiGpiCrProtocolGuid;
typedef struct _EFI_GPI_CR_PROTOCOL EFI_GPI_CR_PROTOCOL;

typedef struct {
  UINT8   *Buffer;
  UINT32   Length;
  UINT32   Flags;
} EFI_GPI_I2C_DESCRIPTOR;

#define EFI_GPI_I2C_FLAG_START  0x00000001U
#define EFI_GPI_I2C_FLAG_STOP   0x00000002U
#define EFI_GPI_I2C_FLAG_WRITE  0x00000004U
#define EFI_GPI_I2C_FLAG_READ   0x00000008U

typedef EFI_STATUS (EFIAPI *EFI_GPI_CR_GET_CONTROLLER)(
  IN  EFI_GPI_CR_PROTOCOL               *This,
  IN  UINT8                              ControllerId,
  OUT CONST CR_TARGET_GPI_CONTROLLER **Controller
  );

typedef EFI_STATUS (EFIAPI *EFI_GPI_CR_GET_CONTEXT)(
  IN  EFI_GPI_CR_PROTOCOL       *This,
  OUT CONST CR_TARGET_QUP_CONTEXT **Context
  );

typedef EFI_STATUS (EFIAPI *EFI_GPI_CR_I2C_TRANSFER)(
  IN  EFI_GPI_CR_PROTOCOL       *This,
  IN  UINT32                    Instance,
  IN  UINT16                    SlaveAddress,
  IN  EFI_GPI_I2C_DESCRIPTOR   *Descriptors,
  IN  UINT16                    DescriptorCount,
  OUT UINT32                    *Transferred OPTIONAL
  );

typedef struct _EFI_GPI_CR_PROTOCOL {
  UINT64                         Revision;
  EFI_GPI_CR_GET_CONTROLLER     GetController;
  EFI_GPI_CR_GET_CONTEXT        GetContext;
  EFI_GPI_CR_I2C_TRANSFER       I2cTransfer;
} EFI_GPI_CR_PROTOCOL;
