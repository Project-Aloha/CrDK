/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include "rpmh_internal.h"
#include <oskal/cr_mutex.h>
#ifdef _KERNEL_MODE
#include "trace.h"
#if !defined(CR_OSK_LOG_WITHOUT_WPP)
#include "rpmh.tmh"
#endif
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
    .reg_drv_cmd_resp_data = 0x40,

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
    .reg_drv_cmd_resp_data = 0x44,

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

STATIC UINT32 ChangeState(volatile UINT32 *State, UINT32 Old, UINT32 New) {
#ifdef _KERNEL_MODE
  return (UINT32)InterlockedCompareExchange((volatile LONG *)State,(LONG)New,(LONG)Old);
#else
  return InterlockedCompareExchange32(State,Old,New);
#endif
}
/* Called only by the exclusive retiring/consuming owner. Free is published
 * LAST: a new claim cannot reuse storage that an old completion still touches. */
STATIC VOID FreeTcs(RpmhDeviceContext *C,UINT32 T) {
  UINT32 Bit=(UINT32)BIT(T);
  CrAtomicAnd32(&C->TcsSynchronous,~Bit);
  CrAtomicAnd32(&C->TcsCompleted,~Bit);
  CrAtomicAnd32(&C->TcsReading,~Bit);
  CrAtomicAnd32(&C->TcsResponseValid,~Bit);
  CrAtomicAnd32(&C->TcsBusy,~Bit);
  CrAtomicAnd32(&C->TcsRetiring,~Bit);
  (VOID)ChangeState(&C->TcsState[T],CrAtomicLoad32(&C->TcsState[T]),RpmhTcsFree);
}
STATIC CR_STATUS RetireCompletedTcs(RpmhDeviceContext *C,UINT32 T) {
  UINT32 Bit=(UINT32)BIT(T),State=CrAtomicLoad32(&C->TcsState[T]);
  if(State==RpmhTcsFree) {
    /* A stray idle level must be acknowledged without racing a new claim. */
    if(ChangeState(&C->TcsState[T],RpmhTcsFree,RpmhTcsRetiring)!=RpmhTcsFree)return CR_BUSY;
    CrAtomicOr32(&C->TcsRetiring,Bit);
    CrMmioWrite32(C->drv_base_address+C->tcs_offset+C->drv_registers->reg_drv_irq_clear,Bit);
    CrAtomicAnd32(&C->TcsRetiring,~Bit);
    (VOID)ChangeState(&C->TcsState[T],RpmhTcsRetiring,RpmhTcsFree);
    return CR_SUCCESS;
  }
  if(State==RpmhTcsFailed) {
    if((CrAtomicLoad32(&C->TcsReading)|CrAtomicLoad32(&C->TcsSynchronous))&Bit) {
      CrMmioWrite32(C->drv_base_address+C->tcs_offset+C->drv_registers->reg_drv_irq_clear,Bit);
      return C->TcsResult[T];
    }
    /* Preserve legacy write late-completion recovery after its waiter has
     * consumed failure. A failed read never upgrades its earlier evidence. */
  }
  UINT32 Retiring=State==RpmhTcsWaiting?RpmhTcsRetiring:RpmhTcsAbandonedRetiring;
  if(State!=RpmhTcsWaiting && State!=RpmhTcsAbandonedWaiting && State!=RpmhTcsFailed)return CR_BUSY;
  if(ChangeState(&C->TcsState[T],State,Retiring)!=State)return CR_BUSY;
  CrAtomicOr32(&C->TcsRetiring,Bit);
  /* A service loop may have sampled an IRQ from the previous incarnation. */
  if(!(CrMmioRead32(C->drv_base_address+C->tcs_offset+C->drv_registers->reg_drv_irq_status)&Bit)) {
    CrAtomicAnd32(&C->TcsRetiring,~Bit);
    if(ChangeState(&C->TcsState[T],RpmhTcsRetiring,RpmhTcsWaiting)!=RpmhTcsRetiring)
      (VOID)ChangeState(&C->TcsState[T],RpmhTcsAbandonedRetiring,RpmhTcsAbandonedWaiting);
    return CR_NOT_READY;
  }
  CR_STATUS Status=CR_SUCCESS;
  UINT32 Enabled=ReadTcsReg(C,C->drv_registers->reg_drv_cmd_enable,T);
  BOOLEAN Reading=(CrAtomicLoad32(&C->TcsReading)&Bit)!=0;
  if(!Enabled || (Reading && Enabled!=1))Status=CR_DEVICE_ERROR;
  for(UINT32 I=0;Status==CR_SUCCESS && I<C->NumCmdsPerTcs;++I) {
    if(!(Enabled&BIT(I)))continue;
    UINT32 Value=ReadTcsCmdReg(C,C->drv_registers->reg_drv_cmd_status,T,I);
    if((Value&(RPMH_TCS_CMD_STATUS_ISSUED_BIT|RPMH_TCS_CMD_STATUS_COMPLETED_BIT))!=
       (RPMH_TCS_CMD_STATUS_ISSUED_BIT|RPMH_TCS_CMD_STATUS_COMPLETED_BIT))Status=CR_DEVICE_ERROR;
  }
  /* The response register belongs to this request until its TCS is cleared. */
  if(Status==CR_SUCCESS && Reading)
    C->TcsResponse[T]=ReadTcsCmdReg(C,C->drv_registers->reg_drv_cmd_resp_data,T,0);
  if(Status==CR_SUCCESS)Status=TcsSetTrigger(C,T,FALSE);
  if(Status==CR_SUCCESS)Status=WriteTcsRegSync(C,C->drv_registers->reg_drv_cmd_enable,T,0);
  if(Status==CR_SUCCESS)Status=WriteTcsRegSync(C,C->drv_registers->reg_drv_cmd_wait_for_compl,T,0);
  CrMmioWrite32(C->drv_base_address+C->tcs_offset+C->drv_registers->reg_drv_irq_clear,Bit);
  C->TcsResult[T]=Status;
  CrAtomicAnd32(&C->TcsRetiring,~Bit);
  if(Status!=CR_SUCCESS) {
    CrAtomicOr32(&C->TcsFailed,Bit);
    /* Failed retirement is quarantined, never evidence of a free slot. */
    do {State=CrAtomicLoad32(&C->TcsState[T]);}
    while(ChangeState(&C->TcsState[T],State,RpmhTcsFailed)!=State);
    return Status;
  }
  if(Reading)CrAtomicOr32(&C->TcsResponseValid,Bit);
  CrAtomicOr32(&C->TcsCompleted,Bit);
  /* Timeout can transfer a retiring request to the ISR, but cannot free it. */
  if(ChangeState(&C->TcsState[T],RpmhTcsRetiring,RpmhTcsCompleted)!=RpmhTcsRetiring)
    FreeTcs(C,T);
  return CR_SUCCESS;
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

/* Caller holds a stack LOCAL result only. The final consume and abandonment
 * handshake is serialized against claim; the ISR uses the atomic slot state. */
STATIC BOOLEAN ConsumeTcs(RpmhDeviceContext *C,UINT32 T,UINT64 Sequence,
                          BOOLEAN Reading,BOOLEAN Abandon,UINT32 *Raw,CR_STATUS *Out) {
  UINT32 Bit=(UINT32)BIT(T);BOOLEAN Done=FALSE;
  CrLockAcquire(&C->Lock);
  if(C->TcsSequence[T]!=Sequence){*Out=CR_COMPROMISED_DATA;Done=TRUE;goto Exit;}
  for(;;) {
    UINT32 State=CrAtomicLoad32(&C->TcsState[T]);
    if(State==RpmhTcsCompleted) {
      if(ChangeState(&C->TcsState[T],State,RpmhTcsConsuming)!=State)continue;
      if(Reading && !(CrAtomicLoad32(&C->TcsResponseValid)&Bit)) {
        C->TcsResult[T]=CR_DEVICE_ERROR;CrAtomicOr32(&C->TcsFailed,Bit);
        (VOID)ChangeState(&C->TcsState[T],RpmhTcsConsuming,RpmhTcsFailed);
        *Out=CR_DEVICE_ERROR;
      }else {
        if(Reading)*Raw=C->TcsResponse[T];
        FreeTcs(C,T);*Out=CR_SUCCESS;
      }
      Done=TRUE;break;
    }
    if(State==RpmhTcsFailed) {
      CrAtomicAnd32(&C->TcsSynchronous,~Bit);*Out=C->TcsResult[T];Done=TRUE;break;
    }
    if(!Abandon)break;
    if(State!=RpmhTcsWaiting && State!=RpmhTcsRetiring) {
      *Out=CR_COMPROMISED_DATA;Done=TRUE;break;
    }
    CrAtomicAnd32(&C->TcsSynchronous,~Bit);
    UINT32 Next=State==RpmhTcsWaiting?RpmhTcsAbandonedWaiting:RpmhTcsAbandonedRetiring;
    if(ChangeState(&C->TcsState[T],State,Next)!=State)continue;
    *Out=CR_TIMEOUT;Done=TRUE;break;
  }
Exit:
  CrLockRelease(&C->Lock);return Done;
}
STATIC CR_STATUS WaitForTcsCompletion(RpmhDeviceContext *C,UINT32 T,
                                      UINT64 Sequence,BOOLEAN Reading,UINT32 *Raw) {
  CR_STATUS Status;
  for(UINT32 Wait=0;Wait<RPMH_TX_MAX_WAIT_TIME;++Wait) {
    ServiceTcsCompletions(C,(UINT32)BIT(T));
    if(ConsumeTcs(C,T,Sequence,Reading,FALSE,Raw,&Status))return Status;
    cr_sleep(1);
  }
  /* Completion at the boundary still belongs to this synchronous owner. */
  ServiceTcsCompletions(C,(UINT32)BIT(T));
  if(ConsumeTcs(C,T,Sequence,Reading,TRUE,Raw,&Status))return Status;
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
      /* Failed retirement may have cleared CMD_ENABLE already. The read's
       * retained address remains exclusive until its actual owner releases. */
      if(CrAtomicLoad32(&RpmhContext->TcsReading)&BIT(tcs_inuse))
        for(UINT32 I=0;I<NumCmds;++I)
          if(CmdDBIsAddrEqual(Commands[I].addr,RpmhContext->TcsReadAddress[tcs_inuse]))return TRUE;
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
    if (!RpmhIsTcsBusy(RpmhContext, i) &&
        CrAtomicLoad32(&RpmhContext->TcsState[i])==RpmhTcsFree) {
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
  if(ChangeState(&RpmhContext->TcsState[*FreeTcsIndex],RpmhTcsFree,RpmhTcsProgramming)!=RpmhTcsFree)
    return CR_BUSY;
  CrAtomicOr32(&RpmhContext->TcsBusy, (UINT32)BIT(*FreeTcsIndex));
  log_info(CR_LOG_CHAR8_STR_FMT ": Claimed TCS %d", __FUNCTION__,
           *FreeTcsIndex);
exit:
  return Status;
}

STATIC
VOID TcsWriteBuffer(RpmhDeviceContext *RpmhContext, UINT32 TcsIndex,
                    UINT32 CmdId, RpmhTcsCmd *TcsCmd, UINT32 NumCmds, BOOLEAN Reading) {
  UINT32 CmdIdEnabled = 0;
  UINT32 CmdWaitForCompletion = 0;

  // Write each commands
  for (UINT32 cmd_idx = CmdId; cmd_idx < CmdId + NumCmds; cmd_idx++, TcsCmd++) {
    UINT32 CmdMsgId =
        RPMH_TCS_CMD_MSG_ID_LEN | (Reading?0:RPMH_TCS_CMD_MSG_ID_WRITE_FLAG_BIT);
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
    if(!Reading)WriteTcsCmdReg(RpmhContext, RpmhContext->drv_registers->reg_drv_cmd_data,
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

STATIC CR_STATUS Submit(RpmhDeviceContext *RpmhContext, RpmhTcsCmd *TcsCmd,
                        UINT32 NumCmds, BOOLEAN Reading, UINT32 *Raw) {
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
  if(RpmhContext->NextSequence==~((UINT64)0)){CrLockRelease(&RpmhContext->Lock);return CR_OUT_OF_RESOURCES;}
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
  UINT64 Sequence=++RpmhContext->NextSequence;
  RpmhContext->TcsSequence[TcsIndex]=Sequence;
  RpmhContext->TcsResult[TcsIndex] = CR_SUCCESS;
  CrAtomicAnd32(&RpmhContext->TcsCompleted, ~Bit);
  CrAtomicAnd32(&RpmhContext->TcsFailed, ~Bit);
  CrAtomicOr32(&RpmhContext->TcsSynchronous, Bit);
  CrAtomicAnd32(&RpmhContext->TcsResponseValid,~Bit);
  if(Reading){RpmhContext->TcsReadAddress[TcsIndex]=TcsCmd[0].addr;CrAtomicOr32(&RpmhContext->TcsReading,Bit);}
  else CrAtomicAnd32(&RpmhContext->TcsReading,~Bit);

  // Write buffer
  TcsWriteBuffer(RpmhContext, TcsIndex, 0, TcsCmd, NumCmds, Reading);
  (VOID)ChangeState(&RpmhContext->TcsState[TcsIndex],RpmhTcsProgramming,RpmhTcsWaiting);

  // Set trigger
  Status = TcsSetTrigger(RpmhContext, TcsIndex, TRUE);
  if (CR_ERROR(Status)) {
    RpmhContext->TcsResult[TcsIndex] = Status;
    CrAtomicOr32(&RpmhContext->TcsFailed, Bit);
    CrAtomicAnd32(&RpmhContext->TcsSynchronous, ~Bit);
    (VOID)ChangeState(&RpmhContext->TcsState[TcsIndex],RpmhTcsWaiting,RpmhTcsFailed);
    CrLockRelease(&RpmhContext->Lock);return Status;
  }
  CrLockRelease(&RpmhContext->Lock);
  return WaitForTcsCompletion(RpmhContext,TcsIndex,Sequence,Reading,Raw);
}

CR_STATUS RpmhWrite(RpmhDeviceContext *C,RpmhTcsCmd *Commands,UINT32 Count) {
  return Submit(C,Commands,Count,FALSE,NULL);
}
CR_STATUS RpmhRead(RpmhDeviceContext *C,UINT32 Address,UINT32 *Out) {
  if(CrMutexCheckLevel()!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  if(!C || !Out || !Address || (Address&3U) || ((UINTN)Out&3U) ||
     (UINTN)Out>~((UINTN)0)-sizeof(*Out) ||
     (UINTN)C>~((UINTN)0)-sizeof(*C) ||
     !((UINTN)Out+sizeof(*Out)<=(UINTN)C || (UINTN)Out>=(UINTN)C+sizeof(*C)))
    return CR_INVALID_PARAMETER;
  if(C->drv_size && (C->drv_base_address>~((UINTN)0)-C->drv_size ||
     !((UINTN)Out+sizeof(*Out)<=C->drv_base_address || (UINTN)Out>=C->drv_base_address+C->drv_size)))
    return CR_INVALID_PARAMETER;
  RpmhTcsCmd Command={Address,0,1,1};UINT32 Raw=0;
  CR_STATUS Status=Submit(C,&Command,1,TRUE,&Raw);
  if(Status==CR_SUCCESS)*Out=Raw;
  return Status;
}

CR_STATUS RpmhLibLoadConfig(RpmhDeviceContext *Context, CONST UINT8 *Data, UINTN Length) {
  if (Context == NULL || Data == NULL || Length != 9 || Context->Initialized)
    return CR_INVALID_PARAMETER;
  UINT32 Offset = (UINT32)Data[1] | (UINT32)Data[2] << 8 |
                  (UINT32)Data[3] << 16 | (UINT32)Data[4] << 24;
  UINT32 Count = (UINT32)Data[5] + Data[6] + Data[7] + Data[8];
  if (Data[0] > 4 || (Offset & 3U) != 0 || Data[5] == 0 || Count > 32)
    return CR_INVALID_PARAMETER;
  if (Context->drv_size != 0 &&
      (Offset > Context->drv_size || Context->drv_size - Offset < 4))
    return CR_INVALID_PARAMETER;
  Context->drv_id = Data[0]; Context->tcs_offset = Offset;
  Context->tcs_config.active_tcs.tcs_count = Data[5];
  Context->tcs_config.sleep_tcs.tcs_count = Data[6];
  Context->tcs_config.wake_tcs.tcs_count = Data[7];
  Context->tcs_config.control_tcs.tcs_count = Data[8];
  return CR_SUCCESS;
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
  if (RpmhContext->drv_base_address == 0 || RpmhContext->drv_id > 4 ||
      (RpmhContext->tcs_offset & 3U) != 0 ||
      (RpmhContext->drv_size != 0 &&
       (RpmhContext->drv_size < 16 || RpmhContext->drv_base_address >
        ~(UINTN)0 - RpmhContext->drv_size))) {
    return CR_INVALID_PARAMETER;
  }
#ifdef _KERNEL_MODE
  if (RpmhContext->drv_size == 0) return CR_INVALID_PARAMETER;
#endif

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
  // Every configured TCS and command register must fit before any writes or
  // IRQ publication. Use wide arithmetic for untrusted ACPI offsets/counts.
  UINT64 RegisterEnd = (UINT64)RpmhContext->tcs_offset +
      (UINT64)(TotalTcs - 1) * RpmhContext->drv_registers->reg_rsc_drv_tcs_offset +
      RpmhContext->drv_registers->reg_drv_cmd_resp_data +
      (UINT64)(RpmhContext->NumCmdsPerTcs - 1) *
          RpmhContext->drv_registers->reg_rsc_drv_cmd_offset + sizeof(UINT32);
  if (RegisterEnd > ~(UINTN)0 - RpmhContext->drv_base_address ||
      (RpmhContext->drv_size != 0 && RegisterEnd > RpmhContext->drv_size))
    return CR_INVALID_PARAMETER;

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
  CrAtomicAnd32(&RpmhContext->TcsReading,0);
  CrAtomicAnd32(&RpmhContext->TcsResponseValid,0);
  for(UINT32 I=0;I<32;++I)RpmhContext->TcsState[I]=RpmhTcsFree;
  RpmhContext->NextSequence=0;
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
  /* A late read still owns response storage, IRQ and mapping. */
  if(RpmhContext->ObservationReferences || CrAtomicLoad32(&RpmhContext->TcsReading))return CR_BUSY;
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
  CrAtomicAnd32(&RpmhContext->TcsReading,0);
  CrAtomicAnd32(&RpmhContext->TcsResponseValid,0);
  for(UINT32 I=0;I<32;++I)RpmhContext->TcsState[I]=RpmhTcsFree;
  RpmhContext->NextSequence=0;
  CrAtomicAnd32(&RpmhContext->TcsBusy, 0);
  CrAtomicAnd32(&RpmhContext->TcsSynchronous, 0);
  CrAtomicAnd32(&RpmhContext->TcsRetiring, 0);
  CrAtomicAnd32(&RpmhContext->TcsCompleted, 0);
  CrAtomicAnd32(&RpmhContext->TcsFailed, 0);
  return CR_SUCCESS;
}

// Retrieve Address from Cmd DB protocol and call this lib function
/* Each VRM has voltage, enable and mode at base+0/+4/+8. Validate the
 * complete tuple before arithmetic so a malformed base cannot wrap a write
 * into a different remote resource. The wire voltage field is 13 bits. */
STATIC BOOLEAN RpmhVrmAddressValid(UINT32 Address) {
  /* A resource occupies one 16-byte VRM slot. These APIs add their own
   * voltage/enable/mode offset; accepting an offset as a base targets a
   * different command or even the following resource. */
  return Address!=0 && (Address&15U)==0 && Address<=0xfffffff7U;
}

CR_STATUS
RpmhEnableVreg(RpmhDeviceContext *RpmhContext, CONST UINT32 Address,
               BOOLEAN Enable) {
  CR_STATUS Status = 0;
  if (!RpmhVrmAddressValid(Address) || (RpmhContext == NULL) || Enable > 1) {
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
  if (!RpmhVrmAddressValid(Address) || (RpmhContext == NULL) || VoltageMv > 0x1fffU) {
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
  if (!RpmhVrmAddressValid(Address) || (RpmhContext == NULL) || Mode > 7) {
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
