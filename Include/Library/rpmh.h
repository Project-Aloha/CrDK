/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_time.h>
#include <oskal/cr_types.h>
#include <oskal/cr_interrupt.h>

// TCS(Trigger Command Set) commands type
typedef struct {
  UINT32 addr;
  UINT32 data;
  UINT32 wait;
  UINT32 response_required;
} RpmhTcsCmd;

// drv version specific registers
typedef struct {
  UINT32 reg_drv_solver_config;
  UINT32 reg_drv_print_child_config;
  UINT32 reg_drv_irq_clear;
  UINT32 reg_drv_irq_status;
  UINT32 reg_drv_irq_enable;
  UINT32 reg_drv_cmd_msgid;
  UINT32 reg_drv_cmd_addr;
  UINT32 reg_drv_cmd_data;
  UINT32 reg_drv_control;
  UINT32 reg_drv_cmd_enable;

  UINT32 reg_rsc_drv_tcs_offset;
  UINT32 reg_rsc_drv_cmd_offset;
} RpmhDrvRegisters;

// tcs data
typedef struct {
  UINT32 mask;
  UINT32 offset;
  UINT8  tcs_count;
} RpmhDrvTcsData;

// tcs count
typedef struct {
  RpmhDrvTcsData active_tcs;
  RpmhDrvTcsData sleep_tcs;
  RpmhDrvTcsData wake_tcs;
  RpmhDrvTcsData control_tcs; // Not used
} RpmhDrvTcsConfig;

/* Driver context */
typedef struct {
  UINTN             drv_base_address;
  UINT32            tcs_offset;
  UINT32            drv_id;
  // UINT32            interrupt_no;
  CR_INTERRUPT_CONFIG InterruptConfig;
  UINT32            NumCmdsPerTcs;
  UINT32            TcsBusy;
  RpmhDrvTcsConfig  tcs_config;
  RpmhDrvRegisters *drv_registers;
  BOOLEAN           IrqFromRpmhCr; // Workaround for compatible with bsp drivers
} RpmhDeviceContext;

enum TcsIndex {
  TCS_INDEX_ACTIVE  = 0,
  TCS_INDEX_SLEEP   = 1,
  TCS_INDEX_WAKE    = 2,
  TCS_INDEX_CONTROL = 3,
};

CR_STATUS
RpmhLibInit(IN OUT RpmhDeviceContext **RpmhContext);

CR_STATUS
RpmhWrite(
    IN RpmhDeviceContext *RpmhContext, IN RpmhTcsCmd *TcsCmd,
    IN UINT32 NumCmds);

CR_STATUS
RpmhEnableVreg(
    IN RpmhDeviceContext *RpmhContext, IN CONST UINT32 Address,
    IN BOOLEAN Enable);

CR_STATUS
RpmhSetVregVoltage(
    IN RpmhDeviceContext *RpmhContext, IN CONST UINT32 Address,
    IN CONST UINT32 VoltageMv);

CR_STATUS
RpmhSetVregMode(
    IN RpmhDeviceContext *RpmhContext, IN CONST UINT32 Address,
    IN CONST UINT8 Mode);
