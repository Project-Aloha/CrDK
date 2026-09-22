/** @file
 *  Crane alias for a MU EFI_I2C_MASTER_PROTOCOL instance.
 *
 *  No private MU ABI is assumed.  I2CCrDxe only publishes an alias after a
 *  standard EFI_I2C_MASTER_PROTOCOL is present on a handle.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Protocol/I2cMaster.h>

#define EFI_I2C_CR_PROTOCOL_REVISION  0x0000000000010000ULL
#define EFI_I2C_CR_PROTOCOL_GUID \
  { 0x6f1a3d94, 0x2b5c, 0x4f06, { 0xa8, 0x7b, 0x39, 0xd2, 0x0e, 0x64, 0x5c, 0x28 } }

extern EFI_GUID gEfiI2cCrProtocolGuid;
typedef struct _EFI_I2C_CR_PROTOCOL EFI_I2C_CR_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_I2C_CR_SET_BUS_FREQUENCY)(
  IN EFI_I2C_CR_PROTOCOL *This,
  IN OUT UINTN           *BusClockHertz
  );

typedef EFI_STATUS (EFIAPI *EFI_I2C_CR_RESET)(
  IN EFI_I2C_CR_PROTOCOL *This
  );

typedef EFI_STATUS (EFIAPI *EFI_I2C_CR_START_REQUEST)(
  IN EFI_I2C_CR_PROTOCOL  *This,
  IN UINTN                 SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET *RequestPacket,
  IN EFI_EVENT             Event OPTIONAL,
  OUT EFI_STATUS          *I2cStatus OPTIONAL
  );

typedef struct _EFI_I2C_CR_PROTOCOL {
  UINT64                         Revision;
  EFI_HANDLE                     MasterHandle;
  EFI_I2C_MASTER_PROTOCOL       *Master;
  CONST EFI_I2C_CONTROLLER_CAPABILITIES *I2cControllerCapabilities;
  EFI_I2C_CR_SET_BUS_FREQUENCY   SetBusFrequency;
  EFI_I2C_CR_RESET                Reset;
  EFI_I2C_CR_START_REQUEST        StartRequest;
} EFI_I2C_CR_PROTOCOL;
