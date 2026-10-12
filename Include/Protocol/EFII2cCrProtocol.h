/** @file
 *  Crane-owned I2C protocol for native GENI FIFO/GPI transports.
 *
 *  I2CCrDxe owns the SM8450 SE1 transport.  It never consumes a standard PI
 *  master, the packaged Qualcomm MU I2C driver, or either private protocol.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

/* Reuse only the descriptor/configuration layouts; no private MU protocol is
   located or linked by I2CCrDxe. */
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
  EFI_I2C_CR_SET_BUS_FREQUENCY   SetBusFrequency;
  EFI_I2C_CR_RESET                Reset;
  EFI_I2C_CR_START_REQUEST        StartRequest;
  /* Native target identity and bus rate. */
  MU_I2C_INSTANCE                 Instance;
  UINT32                         BusFrequencyKHz;
  EFI_I2C_CR_MU_TRANSFER          MuTransfer;
  /* Direct GENI FIFO backend used by the native target transport. */
  EFI_I2C_CR_MU_TRANSFER          FifoTransfer;
} EFI_I2C_CR_PROTOCOL;
