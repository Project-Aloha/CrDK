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
    .reg_drv_cmd_wait_for_compl = 0x10,
    .reg_drv_control = 0x14,
    .reg_drv_cmd_enable = 0x1C,
    .reg_drv_cmd_msgid = 0x30,
    .reg_drv_cmd_addr = 0x34,
    .reg_drv_cmd_data = 0x38,
    .reg_drv_cmd_status = 0x3C,

    .reg_rsc_drv_cmd_offset = 0x14,
    .reg_rsc_drv_tcs_offset = 0x2A0,
};

STATIC RpmhDrvRegisters drv_registers_v3p0 = {
    .reg_drv_solver_config = 0x04,
    .reg_drv_print_child_config = 0x0C,

    .reg_drv_irq_enable = 0x00,
    .reg_drv_irq_status = 0x04,
    .reg_drv_irq_clear = 0x08,
    .reg_drv_cmd_wait_for_compl = 0x20,
    .reg_drv_control = 0x24,
    .reg_drv_cmd_enable = 0x2C,
    .reg_drv_cmd_msgid = 0x34,
    .reg_drv_cmd_addr = 0x38,
    .reg_drv_cmd_data = 0x3C,
    .reg_drv_cmd_status = 0x40,

    .reg_rsc_drv_cmd_offset = 0x18,
    .reg_rsc_drv_tcs_offset = 0x2A0,
};

STATIC CR_STATUS WriteTcsRegSync(RpmhDeviceContext *RpmhContext, UINT32 Reg,
                                 UINT32 TcsIndex, UINT32 Value) {
  WriteTcsReg(RpmhContext, Reg, TcsIndex, Value);
  CR_MEM_BARRIER_DATA_SYN_BARRIAR();

  // Wait for write to complete
  for (UINTN i = 0; i < RPMH_WRITE_MAX_WAIT_TIME; i++) {
    UINT32 RegVal = ReadTcsReg(RpmhContext, Reg, TcsIndex);
    if (RegVal == Value) {
      return CR_SUCCESS;
    }
    cr_sleep(1);
  }

  // log when timeout happen
  log_err("Rpmh " CR_LOG_CHAR8_STR_FMT
          " timeout: Reg=0x%X, TcsIndex=%d, Value=0x%X",
          __FUNCTION__, Reg, TcsIndex, Value);
  return CR_TIMEOUT;
}

STATIC
CR_STATUS TcsSetTrigger(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex,
                        BOOLEAN Trigger) {
  CR_STATUS Status;
  UINT32 RegVal;
  RegVal = ReadTcsReg(RpmhContext, RpmhContext->drv_registers->reg_drv_control,
                      TcsIndex);

  RegVal = CLR_BITS(RegVal, TCS_AMC_MODE_TRIGGER);
  // Write
  Status = WriteTcsRegSync(
      RpmhContext, RpmhContext->drv_registers->reg_drv_control, TcsIndex,
      RegVal);
  if (CR_ERROR(Status)) {
    return Status;
  }
  RegVal = CLR_BITS(RegVal, TCS_AMC_MODE_ENABLE);
  Status = WriteTcsRegSync(
      RpmhContext, RpmhContext->drv_registers->reg_drv_control, TcsIndex,
      RegVal);
  if (CR_ERROR(Status)) {
    return Status;
  }
  if (Trigger) {
    RegVal = TCS_AMC_MODE_ENABLE;
    Status = WriteTcsRegSync(
        RpmhContext, RpmhContext->drv_registers->reg_drv_control, TcsIndex,
        RegVal);
    if (CR_ERROR(Status)) {
      return Status;
    }
    RegVal |= TCS_AMC_MODE_TRIGGER;
    WriteTcsAsync(RpmhContext, RpmhContext->drv_registers->reg_drv_control,
                  TcsIndex, RegVal);
  }
  return CR_SUCCESS;
}

STATIC
CR_STATUS
RetireCompletedTcs(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex) {
  UINT32 Bit = (UINT32)BIT(TcsIndex);
  UINT32 Enabled;
  UINT32 Command;
  UINT32 CommandStatus;
  CR_STATUS Status = CR_SUCCESS;

  if ((CrAtomicLoad32(&RpmhContext->TcsBusy) & Bit) == 0) {
    CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                      RpmhContext->drv_registers->reg_drv_irq_clear,
                  Bit);
    return CR_SUCCESS;
  }

  /* The interrupt and a polling waiter can observe the same completion. */
  if ((CrAtomicOr32(&RpmhContext->TcsRetiring, Bit) & Bit) != 0) {
    return CR_BUSY;
  }

  Enabled = ReadTcsReg(
      RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_enable, TcsIndex);
  for (Command = 0; Command < RpmhContext->NumCmdsPerTcs; Command++) {
    if ((Enabled & BIT(Command)) == 0) {
      continue;
    }
    CommandStatus = ReadTcsCmdReg(
        RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_status,
        TcsIndex, Command);
    if ((CommandStatus & (RPMH_TCS_CMD_STATUS_ISSUED_BIT |
                          RPMH_TCS_CMD_STATUS_COMPLETED_BIT)) !=
        (RPMH_TCS_CMD_STATUS_ISSUED_BIT |
         RPMH_TCS_CMD_STATUS_COMPLETED_BIT)) {
      Status = CR_DEVICE_ERROR;
      break;
    }
  }

  if (!CR_ERROR(Status)) {
    Status = TcsSetTrigger(RpmhContext, TcsIndex, FALSE);
  }
  if (!CR_ERROR(Status)) {
    Status = WriteTcsRegSync(
        RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_enable,
        TcsIndex, 0);
  }
  if (!CR_ERROR(Status)) {
    Status = WriteTcsRegSync(
        RpmhContext,
        RpmhContext->drv_registers->reg_drv_cmd_wait_for_compl,
        TcsIndex, 0);
  }

  /* Always acknowledge the level interrupt. A failed TCS remains busy and
   * cannot be handed to another request. */
  CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                    RpmhContext->drv_registers->reg_drv_irq_clear,
                Bit);
  RpmhContext->TcsResult[TcsIndex] = Status;
  if (CR_ERROR(Status)) {
    CrAtomicOr32(&RpmhContext->TcsFailed, Bit);
  } else {
    CrAtomicOr32(&RpmhContext->TcsCompleted, Bit);
  }
  CrAtomicAnd32(&RpmhContext->TcsRetiring, ~Bit);

  /* A synchronous owner acknowledges completion before releasing the TCS,
   * preventing a new request from erasing its completion state. */
  if (!CR_ERROR(Status) &&
      (CrAtomicLoad32(&RpmhContext->TcsSynchronous) & Bit) == 0) {
    CrAtomicAnd32(&RpmhContext->TcsCompleted, ~Bit);
    CrAtomicAnd32(&RpmhContext->TcsBusy, ~Bit);
  }
  return Status;
}

STATIC
VOID
ServiceTcsCompletions(RpmhDeviceContext *RpmhContext, UINT32 Mask) {
  UINT32 IrqStatus;
  UINT32 TcsIndex;

  IrqStatus =
      CrMmioRead32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                   RpmhContext->drv_registers->reg_drv_irq_status);
  IrqStatus &= RpmhContext->tcs_config.active_tcs.mask & Mask;
  for (TcsIndex = RpmhContext->tcs_config.active_tcs.offset;
       TcsIndex < RpmhContext->tcs_config.active_tcs.offset +
                      RpmhContext->tcs_config.active_tcs.tcs_count;
       TcsIndex++) {
    if ((IrqStatus & BIT(TcsIndex)) != 0) {
      (VOID)RetireCompletedTcs(RpmhContext, TcsIndex);
    }
  }
}

STATIC
CR_STATUS
WaitForTcsCompletion(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex) {
  UINT32 Bit = (UINT32)BIT(TcsIndex);
  UINT32 Wait;
  CR_STATUS Status;

  for (Wait = 0; Wait < RPMH_TX_MAX_WAIT_TIME; Wait++) {
    ServiceTcsCompletions(RpmhContext, Bit);
    if ((CrAtomicLoad32(&RpmhContext->TcsFailed) & Bit) != 0) {
      CrAtomicAnd32(&RpmhContext->TcsSynchronous, ~Bit);
      return RpmhContext->TcsResult[TcsIndex];
    }
    if ((CrAtomicLoad32(&RpmhContext->TcsCompleted) & Bit) != 0) {
      CrAtomicAnd32(&RpmhContext->TcsSynchronous, ~Bit);
      CrAtomicAnd32(&RpmhContext->TcsCompleted, ~Bit);
      CrAtomicAnd32(&RpmhContext->TcsBusy, ~Bit);
      return CR_SUCCESS;
    }
    cr_sleep(1);
  }

  /* Keep a new writer from reusing this TCS while ownership is handed to a
   * possible late IRQ and its final state is inspected. */
  CrLockAcquire(&RpmhContext->Lock);
  CrAtomicAnd32(&RpmhContext->TcsSynchronous, ~Bit);
  ServiceTcsCompletions(RpmhContext, Bit);
  if ((CrAtomicLoad32(&RpmhContext->TcsFailed) & Bit) != 0) {
    Status = RpmhContext->TcsResult[TcsIndex];
  } else if ((CrAtomicLoad32(&RpmhContext->TcsCompleted) & Bit) != 0) {
    CrAtomicAnd32(&RpmhContext->TcsCompleted, ~Bit);
    CrAtomicAnd32(&RpmhContext->TcsBusy, ~Bit);
    Status = CR_SUCCESS;
  } else if ((CrAtomicLoad32(&RpmhContext->TcsBusy) & Bit) == 0) {
    /* An ISR may have retired the TCS and consumed the completion marker
     * after synchronous ownership was dropped. */
    Status = CR_SUCCESS;
  } else {
    Status = CR_TIMEOUT;
  }
  CrLockRelease(&RpmhContext->Lock);

  if (!CR_ERROR(Status)) {
    return Status;
  }
  if (Status != CR_TIMEOUT) {
    return Status;
  }
  log_err("RPMh TCS %u completion timed out", TcsIndex);
  return CR_TIMEOUT;
}

// drv tx done/ drv isr
VOID RpmhDrvTcsTxDoneIsr(VOID *Params) {
  RpmhDeviceContext *RpmhContext = (RpmhDeviceContext *)Params;

  if (RpmhContext == NULL || !RpmhContext->Initialized ||
      RpmhContext->drv_registers == NULL) {
    return;
  }
  ServiceTcsCompletions(RpmhContext,
                        RpmhContext->tcs_config.active_tcs.mask);
}

// Check if this TCS is busy
BOOLEAN RpmhIsTcsBusy(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex) {
  return (CrAtomicLoad32(&RpmhContext->TcsBusy) & BIT(TcsIndex)) != 0;
}

// Check if cmd is processing (inflight)
BOOLEAN
RpmhIsCmdInflight(RpmhDeviceContext *RpmhContext,
                  CONST RpmhTcsCmd *Commands, UINT32 NumCmds) {
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
      for (UINT32 cmd_idx = 0; cmd_idx < RpmhContext->NumCmdsPerTcs;
           cmd_idx++) {
        if (!(CurrentEnable & BIT(cmd_idx))) {
          continue;
        }
        UINT32 Address = ReadTcsCmdReg(
            RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_addr,
            tcs_inuse, cmd_idx);
        log_info(CR_LOG_CHAR8_STR_FMT ": TCS %u, CMD %u Address=0x%X",
                 __FUNCTION__, tcs_inuse, cmd_idx, Address);
        for (UINT32 RequestIndex = 0; RequestIndex < NumCmds;
             RequestIndex++) {
          if (CmdDBIsAddrEqual(Commands[RequestIndex].addr, Address)) {
            log_warn(
                CR_LOG_CHAR8_STR_FMT
                ": Cmd address 0x%X is inflight in TCS %u, CMD %u BUSY!",
                __FUNCTION__, Commands[RequestIndex].addr, tcs_inuse,
                cmd_idx);
            return TRUE;
          }
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
             IN CONST RpmhTcsCmd *Commands, IN UINT32 NumCmds,
             OUT UINT32 *FreeTcsIndex) {
  CR_STATUS Status = CR_SUCCESS;
  // If cmd is inflight, return busy
  if (RpmhIsCmdInflight(RpmhContext, Commands, NumCmds)) {
    log_warn(CR_LOG_CHAR8_STR_FMT ": Conflicting command is already inflight!",
             __FUNCTION__);
    Status = CR_BUSY;
    goto exit;
  }

  Status = GetNextFreeTcs(RpmhContext, TcsData, FreeTcsIndex);
  if (CR_ERROR(Status)) {
    log_warn(CR_LOG_CHAR8_STR_FMT ": No free TCS available!", __FUNCTION__);
    Status = CR_BUSY;
    goto exit;
  }

  // Set busy flag
  CrAtomicOr32(&RpmhContext->TcsBusy, (UINT32)BIT(*FreeTcsIndex));
  log_info(CR_LOG_CHAR8_STR_FMT ": Claimed TCS %d", __FUNCTION__,
           *FreeTcsIndex);
exit:
  return Status;
}

STATIC
VOID TcsWriteBuffer(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex,
                    UINT32 CmdId, RpmhTcsCmd *TcsCmd, UINT32 NumCmds) {
  UINT32 CmdIdEnabled = 0;
  UINT32 CmdWaitForCompletion = 0;

  // Write each commands
  for (UINT32 cmd_idx = CmdId; cmd_idx < CmdId + NumCmds; cmd_idx++, TcsCmd++) {
    UINT32 CmdMsgId =
        RPMH_TCS_CMD_MSG_ID_LEN | RPMH_TCS_CMD_MSG_ID_WRITE_FLAG_BIT;
    CmdIdEnabled |= BIT(cmd_idx);
    /* RpmhWrite is a synchronous API. Request a response for every command so
     * completion can be verified before a dependent resource is initialized. */
    CmdMsgId |= RPMH_TCS_CMD_MSG_ID_RESPONSE_REQUEST_BIT;
    if (TcsCmd->wait) {
      CmdWaitForCompletion |= BIT(cmd_idx);
    }
    // Write tcs cmd to hardware
    WriteTcsCmdReg(RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_msgid,
                   TcsIndex, cmd_idx, CmdMsgId);
    WriteTcsCmdReg(RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_addr,
                   TcsIndex, cmd_idx, TcsCmd->addr);
    WriteTcsCmdReg(RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_data,
                   TcsIndex, cmd_idx, TcsCmd->data);
  }

  WriteTcsReg(
      RpmhContext,
      RpmhContext->drv_registers->reg_drv_cmd_wait_for_compl, TcsIndex,
      CmdWaitForCompletion);
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
  UINT32 Command;
  UINT32 Bit;
  UINT32 TcsIndex;
  CR_STATUS Status;

  if (RpmhContext == NULL || TcsCmd == NULL || NumCmds == 0 ||
      !RpmhContext->Initialized || RpmhContext->NumCmdsPerTcs == 0 ||
      NumCmds > RpmhContext->NumCmdsPerTcs ||
      NumCmds > RPMH_MAX_CMDS_EACH_TCS ||
      RpmhContext->tcs_config.active_tcs.tcs_count == 0) {
    return CR_INVALID_PARAMETER;
  }
  for (Command = 0; Command < NumCmds; Command++) {
    if (TcsCmd[Command].wait > 1 ||
        TcsCmd[Command].response_required > 1) {
      return CR_INVALID_PARAMETER;
    }
  }

  // Serialize claim and programming so a second writer observes the newly
  // programmed in-flight addresses before it checks for conflicts.
  CrLockAcquire(&RpmhContext->Lock);
  Status = ClaimFreeTcs(RpmhContext, &RpmhContext->tcs_config.active_tcs,
                        TcsCmd, NumCmds, &TcsIndex);
  if (CR_ERROR(Status)) {
    CrLockRelease(&RpmhContext->Lock);
    log_err(CR_LOG_CHAR8_STR_FMT
            ": Failed to claim free TCS for %u commands, "
            "Status=0x%X",
            __FUNCTION__, NumCmds, Status);
    return Status;
  }
  Bit = (UINT32)BIT(TcsIndex);
  RpmhContext->TcsResult[TcsIndex] = CR_SUCCESS;
  CrAtomicAnd32(&RpmhContext->TcsCompleted, ~Bit);
  CrAtomicAnd32(&RpmhContext->TcsFailed, ~Bit);
  CrAtomicOr32(&RpmhContext->TcsSynchronous, Bit);

  // Write buffer
  TcsWriteBuffer(RpmhContext, TcsIndex, 0, TcsCmd, NumCmds);

  // Set trigger
  Status = TcsSetTrigger(RpmhContext, TcsIndex, TRUE);
  CrLockRelease(&RpmhContext->Lock);
  if (CR_ERROR(Status)) {
    RpmhContext->TcsResult[TcsIndex] = Status;
    CrAtomicOr32(&RpmhContext->TcsFailed, Bit);
    CrAtomicAnd32(&RpmhContext->TcsSynchronous, ~Bit);
    return Status;
  }
  return WaitForTcsCompletion(RpmhContext, TcsIndex);
}

CR_STATUS
RpmhLibInit(IN OUT RpmhDeviceContext **RpmhContextOut) {
  UINT32 DrvId = 0;
  UINT32 DrvMajorVer = 0;
  UINT32 DrvMinorVer = 0;
  UINT32 DrvConfig = 0;
  UINT32 MaxTcs = 0;
  UINT32 TotalTcs = 0;
  CR_STATUS Status = CR_SUCCESS;
  RpmhDeviceContext *RpmhContext = NULL;

  if (RpmhContextOut == NULL) {
    log_err("RpmhLibInit: NULL output parameter");
    return CR_INVALID_PARAMETER;
  }

  RpmhContext = *RpmhContextOut;
  if (RpmhContext == NULL) {
    log_err("RpmhLibInit requires a target context");
    return CR_NOT_FOUND;
  }

  if (RpmhContext->Initialized) {
    return CR_SUCCESS;
  }
  if (RpmhContext->drv_base_address == 0 || RpmhContext->drv_id > 4) {
    return CR_INVALID_PARAMETER;
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
  TotalTcs = RpmhContext->tcs_config.active_tcs.tcs_count +
             RpmhContext->tcs_config.sleep_tcs.tcs_count +
             RpmhContext->tcs_config.wake_tcs.tcs_count +
             RpmhContext->tcs_config.control_tcs.tcs_count;
  if (RpmhContext->NumCmdsPerTcs == 0 ||
      RpmhContext->NumCmdsPerTcs > RPMH_MAX_CMDS_EACH_TCS ||
      RpmhContext->tcs_config.active_tcs.tcs_count == 0 ||
      TotalTcs > MaxTcs || TotalTcs > 32) {
    log_err("Invalid RPMh TCS configuration");
    return CR_DEVICE_ERROR;
  }

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

  RpmhContext->tcs_config.control_tcs.offset =
      RpmhContext->tcs_config.wake_tcs.offset +
      RpmhContext->tcs_config.wake_tcs.tcs_count;
  RpmhContext->tcs_config.control_tcs.mask =
      (BIT(RpmhContext->tcs_config.control_tcs.tcs_count) - 1)
      << RpmhContext->tcs_config.control_tcs.offset;

  log_info("RPMH DRV TCS Config: Active TCS - offset: %d, mask: 0x%X",
           RpmhContext->tcs_config.active_tcs.offset,
           RpmhContext->tcs_config.active_tcs.mask);
  log_info("RPMH DRV TCS Config: Sleep  TCS - offset: %d, mask: 0x%X",
           RpmhContext->tcs_config.sleep_tcs.offset,
           RpmhContext->tcs_config.sleep_tcs.mask);
  log_info("RPMH DRV TCS Config: Wake   TCS - offset: %d, mask: 0x%X",
           RpmhContext->tcs_config.wake_tcs.offset,
           RpmhContext->tcs_config.wake_tcs.mask);

  CrLockInit(&RpmhContext->Lock);
  CrAtomicAnd32(&RpmhContext->TcsBusy, 0);
  CrAtomicAnd32(&RpmhContext->TcsSynchronous, 0);
  CrAtomicAnd32(&RpmhContext->TcsRetiring, 0);
  CrAtomicAnd32(&RpmhContext->TcsCompleted, 0);
  CrAtomicAnd32(&RpmhContext->TcsFailed, 0);
  CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                    RpmhContext->drv_registers->reg_drv_irq_enable,
                0);
  CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                    RpmhContext->drv_registers->reg_drv_irq_clear,
                RpmhContext->tcs_config.active_tcs.mask);

  RpmhContext->InterruptConfig.Handler = RpmhDrvTcsTxDoneIsr;
  RpmhContext->InterruptConfig.Param = RpmhContext;
  RpmhContext->Initialized = TRUE;
  Status = CrRegisterInterrupt(&RpmhContext->InterruptConfig);
  if (CR_ERROR(Status)) {
    RpmhContext->Initialized = FALSE;
    log_err("Failed to register RPMH DRV interrupt, Status=0x%X", Status);
    return Status;
  }

  // Enable completion interrupts for active TCSes.
  CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                    RpmhContext->drv_registers->reg_drv_irq_enable,
                RpmhContext->tcs_config.active_tcs.mask);

  return Status;
}

CR_STATUS
RpmhLibDeinit(IN OUT RpmhDeviceContext *RpmhContext) {
  CR_STATUS Status;

  if (RpmhContext == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (RpmhContext->Initialized && RpmhContext->drv_registers != NULL) {
    CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                      RpmhContext->drv_registers->reg_drv_irq_enable,
                  0);
    CrMmioWrite32(RpmhContext->drv_base_address + RpmhContext->tcs_offset +
                      RpmhContext->drv_registers->reg_drv_irq_clear,
                  RpmhContext->tcs_config.active_tcs.mask);
  }
  /* Registration can fail after installing the interrupt wrapper.  Attempt
   * cleanup even when initialization did not finish, and retain ownership
   * metadata until the hardware callback was actually removed. */
  Status = CrUnregisterInterrupt(&RpmhContext->InterruptConfig);
  if (CR_ERROR(Status) && Status != CR_NOT_FOUND) {
    return Status;
  }
  RpmhContext->Initialized = FALSE;
  RpmhContext->InterruptConfig.Handler = NULL;
  RpmhContext->InterruptConfig.Param = NULL;
  CrAtomicAnd32(&RpmhContext->TcsBusy, 0);
  CrAtomicAnd32(&RpmhContext->TcsSynchronous, 0);
  CrAtomicAnd32(&RpmhContext->TcsRetiring, 0);
  CrAtomicAnd32(&RpmhContext->TcsCompleted, 0);
  CrAtomicAnd32(&RpmhContext->TcsFailed, 0);
  return CR_SUCCESS;
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
