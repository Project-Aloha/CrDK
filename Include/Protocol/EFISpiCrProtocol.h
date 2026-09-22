/** @file
 *  Crane alias for a MU EFI_SPI_HC_PROTOCOL instance.
 *
 *  SPICrDxe never programs GENI registers.  It forwards the standard SPI host
 *  controller interface exposed by the MU SPIDxe binary.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Protocol/SpiHc.h>

#define EFI_SPI_CR_PROTOCOL_REVISION  0x0000000000010000ULL
#define EFI_SPI_CR_PROTOCOL_GUID \
  { 0x3c7e91b5, 0x84d2, 0x4a69, { 0xb0, 0x2f, 0x76, 0x15, 0xc8, 0x43, 0x9a, 0x0d } }

extern EFI_GUID gEfiSpiCrProtocolGuid;
typedef struct _EFI_SPI_CR_PROTOCOL EFI_SPI_CR_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SPI_CR_CHIP_SELECT)(
  IN EFI_SPI_CR_PROTOCOL       *This,
  IN CONST EFI_SPI_PERIPHERAL  *SpiPeripheral,
  IN BOOLEAN                    PinValue
  );

typedef EFI_STATUS (EFIAPI *EFI_SPI_CR_CLOCK)(
  IN EFI_SPI_CR_PROTOCOL       *This,
  IN CONST EFI_SPI_PERIPHERAL  *SpiPeripheral,
  IN UINT32                    *ClockHz
  );

typedef EFI_STATUS (EFIAPI *EFI_SPI_CR_TRANSACTION)(
  IN EFI_SPI_CR_PROTOCOL       *This,
  IN EFI_SPI_BUS_TRANSACTION   *BusTransaction
  );

typedef struct _EFI_SPI_CR_PROTOCOL {
  UINT64                         Revision;
  EFI_HANDLE                     HostHandle;
  EFI_SPI_HC_PROTOCOL           *Host;
  UINT32                         Attributes;
  UINT32                         FrameSizeSupportMask;
  UINT32                         MaximumTransferBytes;
  EFI_SPI_CR_CHIP_SELECT         ChipSelect;
  EFI_SPI_CR_CLOCK               Clock;
  EFI_SPI_CR_TRANSACTION         Transaction;
} EFI_SPI_CR_PROTOCOL;
