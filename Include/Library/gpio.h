/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include "pdc.h"
#include <oskal/common.h>
#include <oskal/cr_interrupt.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define GPIO_PINS_MAX 300
#define GPIO_FUNCS_NUM_MAX 10
#define GPIO_PDC_PIN_NUMBER_INVALID 0xFFFF
#define GPIO_TLMM_PIN_STRIDE 0x1000

// Width info in gpio regs
#define GPIO_REG_CFG_CTL_REG_MUX_SEL_FIELD_WIDTH 4
#define GPIO_REG_CFG_CTL_REG_PULL_FIELD_WIDTH 2
#define GPIO_REG_CFG_CTL_REG_DRV_STR_FIELD_WIDTH 3
#define GPIO_REG_CFG_CTL_REG_EGPIO_EN_FIELD_WIDTH 1
#define GPIO_REG_CFG_CTL_REG_EGPIO_PRESENT_FIELD_WIDTH 1
#define GPIO_REG_CFG_CTL_REG_OE_FIELD_WIDTH 1
#define GPIO_REG_CFG_IO_REG_IN_FIELD_WIDTH 1
#define GPIO_REG_CFG_IO_REG_OUT_FIELD_WIDTH 1
#define GPIO_REG_CFG_INT_CFG_REG_INT_EN_FIELD_WIDTH 1
#define GPIO_REG_CFG_INT_STATUS_REG_INT_STATUS_FIELD_WIDTH 1
#define GPIO_REG_CFG_INT_TARGET_REG_INT_TARGET_FIELD_WIDTH 3
#define GPIO_REG_CFG_INT_CFG_REG_INT_POLARITY_FIELD_WIDTH 1
#define GPIO_REG_CFG_INT_CFG_REG_INT_DETECTION_FIELD_WIDTH 2
#define GPIO_REG_CFG_INT_CFG_REG_INT_RAW_STATUS_FIELD_WIDTH 1

enum GpioTlmmTile {
  GPIO_TLMM_TILE_DEFAULT,
  // Some platforms only have one tile
  GPIO_TLMM_TILE_EAST = GPIO_TLMM_TILE_DEFAULT,
  GPIO_TLMM_TILE_WEST,
  GPIO_TLMM_TILE_SOUTH,
  GPIO_TLMM_TILE_NORTH,
  GPIO_TLMM_TILE_MAX,
};

typedef enum {
  GPIO_CFG_REG_TYPE_CTL_REG,
  GPIO_CFG_REG_TYPE_IO_REG,
  GPIO_CFG_REG_TYPE_INT_CFG_REG,
  GPIO_CFG_REG_TYPE_INT_STATUS_REG,
  GPIO_CFG_REG_TYPE_INT_TARGET_REG,
  GPIO_CFG_REG_TYPE_WAKE_REG,
  GPIO_CFG_REG_TYPE_MAX
} GpioConfigRegType;

/* Gpio config params */
typedef enum {
  GPIO_PULL_NONE         = 0,
  GPIO_PULL_DOWN         = 1,
  GPIO_PULL_KEEPER       = 2,
  GPIO_PULL_UP_NO_KEEPER = 2,
  GPIO_PULL_UP           = 3,
  GPIO_PULL_MAX          = GPIO_PULL_UP,
  GPIO_PULL_UNCHANGE     = 0xFF,
} GpioPullType;

typedef enum {
  GPIO_FUNC_NORMAL = 0,
  GPIO_FUNC_1,
  GPIO_FUNC_2,
  GPIO_FUNC_3,
  GPIO_FUNC_4,
  GPIO_FUNC_5,
  GPIO_FUNC_6,
  GPIO_FUNC_7,
  GPIO_FUNC_8,
  GPIO_FUNC_9,
  GPIO_FUNC_10,
  GPIO_FUNC_11,
  GPIO_FUNC_12,
  GPIO_FUNC_13,
  GPIO_FUNC_14,
  GPIO_FUNC_15,
  GPIO_FUNC_MAX      = GPIO_FUNC_15,
  GPIO_FUNC_UNCHANGE = 0xFF,
} GpioFunctionType;

#define GPIO_DRIVE_STRENGTH_MAX_MA 16
// way to calc: ma / 2 - 1
typedef enum {
  GPIO_DRIVE_STRENGTH_2MA      = 0,
  GPIO_DRIVE_STRENGTH_4MA      = 1,
  GPIO_DRIVE_STRENGTH_6MA      = 2,
  GPIO_DRIVE_STRENGTH_8MA      = 3,
  GPIO_DRIVE_STRENGTH_10MA     = 4,
  GPIO_DRIVE_STRENGTH_12MA     = 5,
  GPIO_DRIVE_STRENGTH_14MA     = 6,
  GPIO_DRIVE_STRENGTH_16MA     = 7,
  GPIO_DRIVE_STRENGTH_MAX      = GPIO_DRIVE_STRENGTH_16MA,
  GPIO_DRIVE_STRENGTH_UNCHANGE = 0xFF,
} GpioDriveStrength;

typedef enum {
  GPIO_EGPIO_OWNER_REMOTE_OWNER = 0,
  GPIO_EGPIO_OWNER_APPS_OWNER   = 1,
  GPIO_EGPIO_OWNER_MAX          = GPIO_EGPIO_OWNER_APPS_OWNER,
  GPIO_EGPIO_OWNER_UNCHANGE     = 0xFF,
} GpioEGpioOwnerType;

// IO Reg
typedef enum {
  GPIO_VALUE_LOW      = 0,
  GPIO_VALUE_HIGH     = 1,
  GPIO_VALUE_MAX      = GPIO_VALUE_HIGH,
  GPIO_VALUE_UNCHANGE = 0xFF,
} GpioValueType;

typedef struct {
  UINT16            PinNumber;
  BOOLEAN           OutputEnable; // 0 = Input, 1 = Output
  GpioFunctionType  FunctionSel;
  GpioPullType      Pull;
  GpioDriveStrength DriveStrength; // in mA
  GpioValueType     OutputValue;   // Valid if OutputEnable is 1
} GpioConfigParams;

// Gpio Register configuration
typedef struct {
  UINT8 Index;
  // Regs
  UINT32 ControlRegOffset;
  UINT32 IoRegOffset;
  UINT32 IntCfgRegOffset;
  UINT32 IntStatusRegOffset;
  UINT32 IntTargetRegOffset;
  // Masks, generated after getting bits from dsdt.
  /* Bits Mask in Control reg */
  UINT32 PullMsk;
  UINT32 MuxSelMsk;
  UINT32 DriveStrengthMsk;
  UINT32 OutputEnableMsk;
  UINT32 EgpioPresentMsk;
  UINT32 EgpioEnabledMsk;
  /* Bits Mask in IO reg */
  UINT32 InputMsk;
  UINT32 OutputMsk;
  /* Bits Mask in Interrupt Configuration reg*/
  UINT32 InterruptEnMsk;
  UINT32 InterruptPolarityMsk;
  UINT32 InterruptDetectionMsk;
  UINT32 InterruptRawStatusMsk;
  /* Bits Mask in Interrupt Status reg */
  UINT32 InterruptStatusMsk;
  /* Bits Mask in Interrupt Target reg */
  UINT32 InterruptTargetMsk;
  /* Bits Mask in Wake reg */
  UINT32 WakeMsk;
  /* Kpss val for int target reg */
  UINT8 TargetKpssVal;
  UINT8 InterruptDetectionWidth;
} GpioConfigRegInfo;

// Tile Info
typedef struct {
  UINT32 TileOffset; // Offset from TLMM base address
} GpioTlmmTileInfo;

// Gpio Pin Info
typedef struct {
  UINTN              PinAddress;
  UINT8              WakeBit;
  BOOLEAN            Reserved;
  UINT16             PdcPinNumber;
  UINT32             WakeRegOffset;
  GpioConfigRegInfo *pRegCfg;
  GpioTlmmTileInfo  *pTileInfo;
  // Bit map for function available
  UINT32 FuncMuxMap;
} GpioPinInfo;

// Context
#define GPIO_MAX_REG_CFG 16
typedef struct {
  UINT64              TlmmBaseAddress;
  UINT16              PinCount;
  BOOLEAN             PullUpNoKeeper; // not used in kmd
  CR_INTERRUPT_CONFIG InterruptConfig;

  /* TLMM Tile info */
  GpioTlmmTileInfo Tiles[GPIO_TLMM_TILE_MAX];

  /* Gpio Register Configurations */
  GpioConfigRegInfo RegCfg[GPIO_MAX_REG_CFG];

  /* Gpio Pins */
  GpioPinInfo GpioPins[GPIO_PINS_MAX];
} GpioDeviceContext;

GpioValueType
GpioReadInputVal(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex);

GpioValueType
GpioReadOutputVal(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex);

VOID GpioSetConfig(
    IN GpioDeviceContext *GpioContext, IN GpioConfigParams *ConfigParams);

VOID GpioReadConfig(
    IN GpioDeviceContext *GpioContext, IN OUT GpioConfigParams *ConfigParams);

VOID GpioInitConfigParams(
    OUT GpioConfigParams *ConfigParams, IN UINT16 PinNumber);

CR_STATUS GpioEnableInterrupt(
    IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex, BOOLEAN Enable);

CR_STATUS GpioRegisterGpioIrq(
    IN GpioDeviceContext *Context, IN PdcDeviceContext *PdcContext,
    IN UINT16 GpioIndex, IN CR_INTERRUPT_HANDLER InterruptHandler,
    IN VOID *Param, IN CR_INTERRUPT_TRIGGER_TYPE TriggerType);

CR_STATUS GpioInitIrq(GpioDeviceContext *Context);

BOOLEAN
GpioReadIrqStatus(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex);
CR_STATUS GpioMaskInterrupt(
    IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex, BOOLEAN Mask);
CR_STATUS
GpioCheckInterruptEnabled(
    IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex, BOOLEAN *Enabled);

VOID GpioCleanIrqStatus(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex);

CR_STATUS GpioSetInterruptCfg(
    IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex,
    IN CR_INTERRUPT_TRIGGER_TYPE TriggerType);

CR_STATUS GpioLibInit(OUT GpioDeviceContext **GpioContext);