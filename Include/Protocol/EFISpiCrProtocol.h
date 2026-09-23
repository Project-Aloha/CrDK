/** @file
 *  Crane adapter for a MU Qualcomm SPI protocol instance.
 *
 *  SPICrDxe forwards an existing PI host-controller protocol when available.
 *  Otherwise it translates supported PI transactions to MU's private SPIDxe
 *  ABI without programming GENI registers itself.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Protocol/EFIMuBusProtocol.h>
#include <Protocol/SpiHc.h>

#define EFI_SPI_CR_PROTOCOL_REVISION  0x0000000000010001ULL
#define EFI_SPI_CR_PROTOCOL_GUID \
  { 0x3c7e91b5, 0x84d2, 0x4a69, { 0xb0, 0x2f, 0x76, 0x15, 0xc8, 0x43, 0x9a, 0x0d } }

extern EFI_GUID gEfiSpiCrProtocolGuid;
typedef struct _EFI_SPI_CR_PROTOCOL EFI_SPI_CR_PROTOCOL;

/** Vendor layout used when ChipSelectParameter is owned by SPICrDxe. */
typedef struct {
  UINT32 SlaveNumber;
} EFI_SPI_CR_CHIP_SELECT_PARAMETER;

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

typedef EFI_STATUS (EFIAPI *EFI_SPI_CR_MU_TRANSFER)(
  IN EFI_SPI_CR_PROTOCOL *This,
  IN MU_SPI_DEVICE_INFO  *DeviceInfo,
  IN CONST UINT8         *WriteBuffer,
  IN UINT32               WriteLength,
  OUT UINT8              *ReadBuffer,
  IN UINT32               ReadLength
  );

typedef struct _EFI_SPI_CR_PROTOCOL {
  UINT64                         Revision;
  /* Keep the original standard-protocol fields at their established offsets. */
  EFI_HANDLE                     HostHandle;
  EFI_SPI_HC_PROTOCOL           *Host;
  UINT32                         Attributes;
  UINT32                         FrameSizeSupportMask;
  UINT32                         MaximumTransferBytes;
  EFI_SPI_CR_CHIP_SELECT         ChipSelect;
  EFI_SPI_CR_CLOCK               Clock;
  EFI_SPI_CR_TRANSACTION         Transaction;
  /* Private-MU backing is appended for binary compatibility. */
  MU_SPI_PROTOCOL                *Qcom;
  MU_SPI_INSTANCE                 Instance;
  VOID                           *QcomHandle;
  MU_SPI_DEVICE_INFO              DeviceInfo;
  EFI_SPI_CR_MU_TRANSFER          MuTransfer;
} EFI_SPI_CR_PROTOCOL;
