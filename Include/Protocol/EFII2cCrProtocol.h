/** @file
 *  Crane adapter for a MU Qualcomm I2C protocol instance.
 *
 *  I2CCrDxe forwards an existing PI master protocol when MU publishes one.
 *  On targets where MU only publishes gQcomI2CProtocolGuid, it translates PI
 *  requests to that private interface without taking ownership of GENI/QUP.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Protocol/EFIMuBusProtocol.h>
#include <Protocol/I2cMaster.h>

#define EFI_I2C_CR_PROTOCOL_REVISION  0x0000000000010001ULL
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

typedef EFI_STATUS (EFIAPI *EFI_I2C_CR_MU_TRANSFER)(
  IN EFI_I2C_CR_PROTOCOL    *This,
  IN MU_I2C_SLAVE_CONFIG    *Config,
  IN MU_I2C_DESCRIPTOR      *Descriptors,
  IN UINT16                  DescriptorCount,
  IN MU_I2C_CALLBACK         Callback OPTIONAL,
  IN VOID                   *Context OPTIONAL,
  IN UINT32                  Delay,
  OUT UINT32                *Transferred OPTIONAL
  );

typedef struct _EFI_I2C_CR_PROTOCOL {
  UINT64                         Revision;
  /* Keep the original standard-protocol fields at their established offsets. */
  EFI_HANDLE                     MasterHandle;
  EFI_I2C_MASTER_PROTOCOL       *Master;
  CONST EFI_I2C_CONTROLLER_CAPABILITIES *I2cControllerCapabilities;
  EFI_I2C_CR_SET_BUS_FREQUENCY   SetBusFrequency;
  EFI_I2C_CR_RESET                Reset;
  EFI_I2C_CR_START_REQUEST        StartRequest;
  /* Private-MU backing is appended for binary compatibility. */
  MU_I2C_PROTOCOL               *Qcom;
  MU_I2C_INSTANCE                 Instance;
  VOID                           *QcomHandle;
  UINT32                         BusFrequencyKHz;
  EFI_I2C_CR_MU_TRANSFER          MuTransfer;
} EFI_I2C_CR_PROTOCOL;
