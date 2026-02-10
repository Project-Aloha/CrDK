/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include "rpmh_internal.h"
#ifdef _KERNEL_MODE
#include "trace.h"
#include "rpmh.tmh"
#endif

/** Reference
  linux/drivers/soc/qcom/rpmh-rsc.c
*/
STATIC RpmhDrvRegisters drv_registers_v2p7 = {
    .reg_drv_solver_config = 0x04,
    .reg_drv_print_child_config = 0x0C,

    .reg_drv_irq_enable = 0x00,
    .reg_drv_irq_status = 0x04,
    .reg_drv_irq_clear = 0x08,
    .reg_drv_control = 0x14,
    .reg_drv_cmd_enable = 0x1C,
    .reg_drv_cmd_msgid = 0x30,
    .reg_drv_cmd_addr = 0x34,
    .reg_drv_cmd_data = 0x38,

    .reg_rsc_drv_cmd_offset = 0x14,
    .reg_rsc_drv_tcs_offset = 0x2A0,
};

STATIC RpmhDrvRegisters drv_registers_v3p0 = {
    .reg_drv_solver_config = 0x04,
    .reg_drv_print_child_config = 0x0C,

    .reg_drv_irq_enable = 0x00,
    .reg_drv_irq_status = 0x04,
    .reg_drv_irq_clear = 0x08,
    .reg_drv_control = 0x24,
    .reg_drv_cmd_enable = 0x2C,
    .reg_drv_cmd_msgid = 0x34,
    .reg_drv_cmd_addr = 0x38,
    .reg_drv_cmd_data = 0x3C,

    .reg_rsc_drv_cmd_offset = 0x18,
    .reg_rsc_drv_tcs_offset = 0x2A0,
};

STATIC VOID WriteTcsRegSync(RpmhDeviceContext *RpmhContext, UINT32 Reg,
                            UINT32 TcsIndex, UINT32 Value) {
  WriteTcsReg(RpmhContext, Reg, TcsIndex, Value);
  CR_MEM_BARRIER_DATA_SYN_BARRIAR();

  // Wait for write to complete
  for (UINTN i = 0; i < RPMH_WRITE_MAX_WAIT_TIME; i++) {
    UINT32 RegVal = ReadTcsReg(RpmhContext, Reg, TcsIndex);
    if (RegVal == Value)
      return;
    cr_sleep(1);
  }

  // log when timeout happen
  log_err("Rpmh " CR_LOG_CHAR8_STR_FMT
          " timeout: Reg=0x%X, TcsIndex=%d, Value=0x%X",
          __FUNCTION__, Reg, TcsIndex, Value);
}

STATIC VOID TcsEnableInterrupt(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex,
                               BOOLEAN Enable) {
  UINT32 RegVal;
  UINTN TcsIrqEnableAddress = RpmhContext->drv_base_address +
                              RpmhContext->tcs_offset +
                              RpmhContext->drv_registers->reg_drv_irq_enable;
  RegVal = CrMmioRead32(TcsIrqEnableAddress);
  if (Enable)
    RegVal = SET_BITS(RegVal, TcsIndex); // enable irq
  else
    RegVal = CLR_BITS(RegVal, TcsIndex); // disable irq

  CrMmioWrite32(TcsIrqEnableAddress, RegVal);
}

STATIC
VOID TcsSetTrigger(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex,
                   BOOLEAN Trigger) {
  UINT32 RegVal;
  RegVal = ReadTcsReg(RpmhContext, RpmhContext->drv_registers->reg_drv_control,
                      TcsIndex);

  RegVal = CLR_BITS(TCS_AMC_MODE_TRIGGER, RegVal);
  // Write
  WriteTcsRegSync(RpmhContext, RpmhContext->drv_registers->reg_drv_control,
                  TcsIndex, RegVal);
  RegVal = CLR_BITS(TCS_AMC_MODE_ENABLE, RegVal);
  WriteTcsRegSync(RpmhContext, RpmhContext->drv_registers->reg_drv_control,
                  TcsIndex, RegVal);
  if (Trigger) {
    RegVal = TCS_AMC_MODE_ENABLE;
    WriteTcsRegSync(RpmhContext, RpmhContext->drv_registers->reg_drv_control,
                    TcsIndex, RegVal);
    RegVal |= TCS_AMC_MODE_TRIGGER;
    WriteTcsReg(RpmhContext, RpmhContext->drv_registers->reg_drv_control,
                TcsIndex, RegVal);
  }
}

// drv tx done/ drv isr
VOID RpmhDrvTcsTxDoneIsr(VOID *Params) {
  UINT32 IrqStatus;
  RpmhDeviceContext *RpmhContext = (RpmhDeviceContext *)Params;

  log_info("RpmhDrvTcsTxDoneIsr called");
#ifndef _KERNEL_MODE
  if (!RpmhContext->IrqFromRpmhCr) {
    log_info("RpmhDrvTcsTxDoneIsr: Not from RpmhCr, ignore");
    log_info("Entering BSP Driver... Disable interrupt");
    // Unregister interrupt
    CrUnregisterInterrupt(&RpmhContext->InterruptConfig);
    return;
  }
  // Unset flag
  RpmhContext->IrqFromRpmhCr = FALSE;
#endif
  // Read irq status
  IrqStatus =
      CrMmioRead32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                   RpmhContext->drv_registers->reg_drv_irq_status);
  log_info("RpmhDrvTcsTxDoneIsr IrqStatus=0x%X", IrqStatus);

  // Check each bit and clear
  for (UINT32 i = 0; i < RpmhContext->NumCmdsPerTcs; i++) {
    if (IrqStatus & BIT(i)) {

      // Set trigger
      TcsSetTrigger(RpmhContext, i, FALSE);

      // Enable TCS again
      WriteTcsReg(RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_enable,
                  i, 0);
      // Clear irq
      CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                        RpmhContext->drv_registers->reg_drv_irq_clear,
                    BIT(i));
      if (!RpmhContext->tcs_config.active_tcs.tcs_count) {
        // Disable irq for wake tcs
        TcsEnableInterrupt(RpmhContext, i, FALSE);
      }
      // Clear busy flag
      RpmhContext->TcsBusy = CLR_BITS(BIT(i), RpmhContext->TcsBusy);
    }
  }
  log_info("RpmhDrvTcsTxDoneIsr exit");
}

// Check if this TCS is busy
BOOLEAN RpmhIsTcsBusy(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex) {
  return (RpmhContext->TcsBusy & BIT(TcsIndex)) != 0;
}

// Check if cmd is processing (inflight)
BOOLEAN
RpmhIsCmdInflight(RpmhDeviceContext *RpmhContext, UINT32 CmdRequestAddress) {
  // Only check busy tcs
  for (UINT32 tcs_inuse = RpmhContext->tcs_config.active_tcs.offset;
       tcs_inuse < RpmhContext->tcs_config.active_tcs.offset +
                       RpmhContext->tcs_config.active_tcs.tcs_count;
       tcs_inuse++) {
    if (RpmhIsTcsBusy(RpmhContext, tcs_inuse)) {
      // Check cmd addr running
      UINT32 CurrentEnable =
          ReadTcsReg(RpmhContext,
                     RpmhContext->drv_registers->reg_drv_cmd_enable, tcs_inuse);
      for (UINT32 cmd_idx = 0; cmd_idx < RPMH_MAX_CMDS_EACH_TCS; cmd_idx++) {
        if (!(CurrentEnable & BIT(cmd_idx))) {
          continue;
        }
        UINT32 Address = ReadTcsCmdReg(
            RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_addr,
            tcs_inuse, cmd_idx);
        log_info(CR_LOG_CHAR8_STR_FMT ": TCS %u, CMD %u Address=0x%X",
                 __FUNCTION__, tcs_inuse, cmd_idx, Address);
        if (CmdDBIsAddrEqual(CmdRequestAddress, Address)) {
          log_warn(CR_LOG_CHAR8_STR_FMT
                   ": Cmd address 0x%X is inflight in TCS %u, CMD %u BUSY!",
                   __FUNCTION__, CmdRequestAddress, tcs_inuse, cmd_idx);
          return TRUE;
        }
      }
    }
  }
  return FALSE;
}

// Find free tcs id
STATIC CR_STATUS GetNextFreeTcs(IN RpmhDeviceContext *RpmhContext,
                                IN RpmhDrvTcsData *TcsData,
                                OUT UINT32 *FreeTcsIndex) {
  for (UINT32 i = TcsData->offset; i < TcsData->offset + TcsData->tcs_count;
       i++) {
    if (!RpmhIsTcsBusy(RpmhContext, i)) {
      *FreeTcsIndex = i;
      return CR_SUCCESS;
    }
  }
  // No free tcs
  log_warn(CR_LOG_CHAR8_STR_FMT ": No free TCS found!", __FUNCTION__);
  return CR_NOT_FOUND;
}

STATIC
CR_STATUS
ClaimFreeTcs(IN RpmhDeviceContext *RpmhContext, IN RpmhDrvTcsData *TcsData,
             IN UINT32 CmdRequestAddress, OUT UINT32 *FreeTcsIndex) {
  CR_STATUS Status = CR_SUCCESS;
  // If cmd is inflight, return busy
  if (RpmhIsCmdInflight(RpmhContext, CmdRequestAddress)) {
    log_warn(CR_LOG_CHAR8_STR_FMT ": Cmd address 0x%X is already inflight!",
             __FUNCTION__, CmdRequestAddress);
    Status = CR_BUSY;
    goto exit;
  }

  Status = GetNextFreeTcs(RpmhContext, TcsData, FreeTcsIndex);
  if (FreeTcsIndex < 0) {
    log_warn(CR_LOG_CHAR8_STR_FMT ": No free TCS available!", __FUNCTION__);
    Status = CR_BUSY;
    goto exit;
  }

  // Set busy flag
  RpmhContext->TcsBusy = SET_BITS(RpmhContext->TcsBusy, BIT(*FreeTcsIndex));
  // Also set a mark to indicate a interrupt may happen later
  RpmhContext->IrqFromRpmhCr = TRUE;
  log_info(CR_LOG_CHAR8_STR_FMT ": Claimed TCS %d", __FUNCTION__,
           *FreeTcsIndex);
exit:
  return Status;
}

STATIC
VOID TcsWriteBuffer(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex,
                    UINT32 CmdId, RpmhTcsCmd *TcsCmd, UINT32 NumCmds) {
  UINT32 CmdIdEnabled = 0;
  UINT32 CmdMsgId =
      RPMH_TCS_CMD_MSG_ID_LEN | RPMH_TCS_CMD_MSG_ID_WRITE_FLAG_BIT;

  // Write each commands
  for (UINT32 cmd_idx = CmdId; cmd_idx < CmdId + NumCmds; cmd_idx++, TcsCmd++) {
    CmdIdEnabled |= BIT(cmd_idx);
    if (TcsCmd->response_required)
      CmdMsgId |= RPMH_TCS_CMD_MSG_ID_RESPONSE_REQUEST_BIT;
    // Write tcs cmd to hardware
    WriteTcsCmdReg(RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_msgid,
                   TcsIndex, cmd_idx, CmdMsgId);
    WriteTcsCmdReg(RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_addr,
                   TcsIndex, cmd_idx, TcsCmd->addr);
    WriteTcsCmdReg(RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_data,
                   TcsIndex, cmd_idx, TcsCmd->data);
  }

  // Write to enabled cmd id
  WriteTcsReg(
      RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_enable, TcsIndex,
      CmdIdEnabled | ReadTcsReg(RpmhContext,
                                RpmhContext->drv_registers->reg_drv_cmd_enable,
                                TcsIndex));
}

// Rpmh write
CR_STATUS
RpmhWrite(RpmhDeviceContext *RpmhContext, RpmhTcsCmd *TcsCmd, UINT32 NumCmds) {
  UINT32 TcsIndex;
  CR_STATUS Status;
  // Claim free TCS
  Status = ClaimFreeTcs(RpmhContext, &RpmhContext->tcs_config.active_tcs,
                        TcsCmd->addr, &TcsIndex);
  if (CR_ERROR(Status)) {
    log_err(CR_LOG_CHAR8_STR_FMT
            ": Failed to claim free TCS for cmd address 0x%X, "
            "Status=0x%X",
            __FUNCTION__, TcsCmd->addr, Status);
    return Status;
  }

  // Write buffer
  TcsWriteBuffer(RpmhContext, TcsIndex, 0, TcsCmd,
                 NumCmds); // Currently only support 1 cmd

  // Set trigger
  TcsSetTrigger(RpmhContext, TcsIndex, TRUE);
  return CR_SUCCESS;
}

CR_STATUS
RpmhLibInit(IN OUT RpmhDeviceContext **RpmhContextOut) {
  UINT32 DrvId = 0;
  UINT32 DrvMajorVer = 0;
  UINT32 DrvMinorVer = 0;
  UINT32 DrvConfig = 0;
  UINT32 MaxTcs = 0;
  CR_STATUS Status = CR_SUCCESS;
  RpmhDeviceContext *RpmhContext = NULL;

  if (RpmhContextOut == NULL) {
    log_err("RpmhLibInit: NULL output parameter");
    return CR_INVALID_PARAMETER;
  }

  // Check if RpmhContext is already initialized
  // Notice, only wdf passing initialized context
  if (*RpmhContextOut == NULL) {
    // Init driver structure if not initialized
    RpmhContext = CrTargetGetRpmhContext();
    if (RpmhContext == NULL) {
      log_err("RpmhLibInit: Failed to get Rpmh Context");
      return CR_NOT_FOUND;
    }
    *RpmhContextOut = RpmhContext;
  } else {
    // Use the provided context from WDF
    RpmhContext = *RpmhContextOut;
  }

  // Get Rpmh DRV(Direct Resource Voter) version ID
  DrvId = CrMmioRead32(RpmhContext->drv_base_address);
  DrvMajorVer = GET_FIELD(DrvId, RPMH_DRV_ID_MAJOR_MSK);
  DrvMinorVer = GET_FIELD(DrvId, RPMH_DRV_ID_MINOR_MSK);
  log_info("RPMH DRV Version: %d.%d", DrvMajorVer, DrvMinorVer);

  // Check if we are running on a supported version
  if (DrvMajorVer >= 3) {
    RpmhContext->drv_registers = &drv_registers_v3p0;
  } else {
    RpmhContext->drv_registers = &drv_registers_v2p7;
  }

  // probe tcs config
  DrvConfig =
      CrMmioRead32(RpmhContext->drv_base_address +
                   RpmhContext->drv_registers->reg_drv_print_child_config);

  MaxTcs = ((DrvConfig &
             ((RSC_DRV_TCS_NUM_MSK >> (__ffs(RSC_DRV_TCS_NUM_MSK) - 1))
              << (((__ffs(RSC_DRV_TCS_NUM_MSK) - 1)) * RpmhContext->drv_id))) >>
            ((__ffs(RSC_DRV_TCS_NUM_MSK) - 1) * RpmhContext->drv_id));
  log_info("RPMH DRV Max TCS: %d", MaxTcs);

  RpmhContext->NumCmdsPerTcs = GET_FIELD(DrvConfig, RSC_DRV_NCPT_MSK);
  log_info("RPMH DRV Num Cmds per TCS: %d", RpmhContext->NumCmdsPerTcs);
  CR_ASSERT((UINT32)(RpmhContext->tcs_config.active_tcs.tcs_count +
                     RpmhContext->tcs_config.sleep_tcs.tcs_count +
                     RpmhContext->tcs_config.wake_tcs.tcs_count +
                     RpmhContext->tcs_config.control_tcs.tcs_count) <= MaxTcs);

  // Calculate tcs offsets, masks, etc.
  RpmhContext->tcs_config.active_tcs.offset = 0;
  RpmhContext->tcs_config.active_tcs.mask =
      (BIT(RpmhContext->tcs_config.active_tcs.tcs_count) - 1) << 0;

  RpmhContext->tcs_config.sleep_tcs.offset =
      RpmhContext->tcs_config.active_tcs.tcs_count;
  RpmhContext->tcs_config.sleep_tcs.mask =
      (BIT(RpmhContext->tcs_config.sleep_tcs.tcs_count) - 1)
      << RpmhContext->tcs_config.sleep_tcs.offset;

  RpmhContext->tcs_config.wake_tcs.offset =
      RpmhContext->tcs_config.sleep_tcs.offset +
      RpmhContext->tcs_config.sleep_tcs.tcs_count;
  RpmhContext->tcs_config.wake_tcs.mask =
      (BIT(RpmhContext->tcs_config.wake_tcs.tcs_count) - 1)
      << RpmhContext->tcs_config.wake_tcs.offset;

  // Ignore control tcs
  log_info("RPMH DRV TCS Config: Active TCS - offset: %d, mask: 0x%X",
           RpmhContext->tcs_config.active_tcs.offset,
           RpmhContext->tcs_config.active_tcs.mask);
  log_info("RPMH DRV TCS Config: Sleep  TCS - offset: %d, mask: 0x%X",
           RpmhContext->tcs_config.sleep_tcs.offset,
           RpmhContext->tcs_config.sleep_tcs.mask);
  log_info("RPMH DRV TCS Config: Wake   TCS - offset: %d, mask: 0x%X",
           RpmhContext->tcs_config.wake_tcs.offset,
           RpmhContext->tcs_config.wake_tcs.mask);

  // Enable Interrupt
  // Status = CrRegisterInterrupt(
  //     RpmhContext->interrupt_no,        // Irq Number
  //     RpmhDrvTcsTxDoneIsr,              // Handler
  //     RpmhContext,                      // Param
  //     CR_INTERRUPT_TRIGGER_LEVEL_HIGH); // Level trigger
  // Set our ISR to handler
  RpmhContext->InterruptConfig.Handler = RpmhDrvTcsTxDoneIsr;
  RpmhContext->InterruptConfig.Param = RpmhContext;
  Status = CrRegisterInterrupt(&RpmhContext->InterruptConfig);
  if (CR_ERROR(Status)) {
    log_err("Failed to register RPMH DRV interrupt, Status=0x%X", Status);
    return Status;
  }

  // Ignore solver config

  // Calculate Active tcs mask and write it into drv_irq_enable
  CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                    RpmhContext->drv_registers->reg_drv_irq_clear,
                RpmhContext->tcs_config.active_tcs.mask);

  return Status;
}

// Retrieve Address from Cmd DB protocol and call this lib function
CR_STATUS
RpmhEnableVreg(RpmhDeviceContext *RpmhContext, CONST UINT32 Address,
               BOOLEAN Enable) {
  CR_STATUS Status = 0;
  if ((Address == 0) || (RpmhContext == NULL) || Enable > 1) {
    log_err("Invalid parameters");
    Status = CR_INVALID_PARAMETER;
    return Status;
  }

  RpmhTcsCmd VregEnableCmd = {
      .addr = (Address + RPMH_REGULATOR_ENABLE_REG),
      .data = Enable,
      .wait = 0,
      .response_required = 1,
  };

  // Write to RPMH
  Status = RpmhWrite(RpmhContext, &VregEnableCmd, 1);
  if (CR_ERROR(Status)) {
    log_err("Failed to write vreg data to RPMH, Status=0x%X", Status);
    return Status;
  }

  log_info("Vreg at 0x%X " CR_LOG_CHAR8_STR_FMT " via RPMH", Address,
           Enable ? "enabled" : "disabled");
  return CR_SUCCESS;
}

// Set vreg voltage
CR_STATUS
RpmhSetVregVoltage(RpmhDeviceContext *RpmhContext, CONST UINT32 Address,
                   CONST UINT32 VoltageMv) {
  CR_STATUS Status = 0;
  if ((Address == 0) || (RpmhContext == NULL)) {
    log_err("Invalid parameters");
    Status = CR_INVALID_PARAMETER;
    return Status;
  }

  // Data is in mV (0.001V)
  RpmhTcsCmd VregVoltageCmd = {
      .addr = (Address + RPMH_REGULATOR_VOLTAGE_REG),
      .data = VoltageMv,
      .wait = 0,
      .response_required = 1,
  };

  // Write to RPMH
  Status = RpmhWrite(RpmhContext, &VregVoltageCmd, 1);
  if (CR_ERROR(Status)) {
    log_err("Failed to write vreg voltage to RPMH, Status=0x%X", Status);
    return Status;
  }
  log_info("Vreg at 0x%X set to %u uV via RPMH", Address, VoltageMv * 1000);
  return CR_SUCCESS;
}

// Set Vreg mode
CR_STATUS
RpmhSetVregMode(RpmhDeviceContext *RpmhContext, CONST UINT32 Address,
                CONST UINT8 Mode) {
  CR_STATUS Status = 0;
  if ((Address == 0) || (RpmhContext == NULL) || Mode > 8) {
    log_err("Invalid parameters");
    Status = CR_INVALID_PARAMETER;
    return Status;
  }

  RpmhTcsCmd VregModeCmd = {
      .addr = (Address + RPMH_REGULATOR_MODE_REG),
      .data = Mode,
      .wait = 0,
      .response_required = 1,
  };

  // Write to RPMH
  Status = RpmhWrite(RpmhContext, &VregModeCmd, 1);
  if (CR_ERROR(Status)) {
    log_err("Failed to write vreg mode to RPMH, Status=0x%X", Status);
    return Status;
  }

  log_info("Vreg at 0x%X set to mode %u via RPMH", Address, Mode);
  return CR_SUCCESS;
}