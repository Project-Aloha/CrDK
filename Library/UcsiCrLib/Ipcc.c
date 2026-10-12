/**
  IPCC MPROC receive, adapted from Linux qcom-ipcc.c register semantics.
  Copyright (c) 2018-2020, The Linux Foundation.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include <ucsi/Ipcc.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>
#ifdef _KERNEL_MODE
#include "Ipcc.tmh"
#endif

#define IPCC_CONFIG 0x08U
#define IPCC_SEND_ID 0x0CU
#define IPCC_RECV_ID 0x10U
#define IPCC_SIGNAL_ENABLE 0x14U
#define IPCC_SIGNAL_DISABLE 0x18U
#define IPCC_SIGNAL_CLEAR 0x1CU
#define IPCC_CLEAR_ON_READ 1U
#define IPCC_EMPTY 0xFFFFFFFFU
#define IPCC_BUDGET 64U
#define PENDING_GLINK 1U
#define PENDING_MORE 2U
STATIC BOOLEAN MayRun(CR_IPCC *Ipcc){
  if(Ipcc->Admit!=NULL&&!Ipcc->Admit(Ipcc->AdmitContext)){
    CrAtomicOr32(&Ipcc->Stopping,1U);return FALSE;
  }
  return TRUE;
}
STATIC UINT32 Read(CR_IPCC *Ipcc,UINT32 Offset) {
  return Ipcc->Io.Read32 ? Ipcc->Io.Read32(Ipcc->Io.Context,Offset) : CrMmioRead32(Ipcc->Base+Offset);
}
STATIC VOID Write(CR_IPCC *Ipcc,UINT32 Offset,UINT32 Value) {
  if(Ipcc->Io.Write32)Ipcc->Io.Write32(Ipcc->Io.Context,Offset,Value);
  else CrMmioWrite32(Ipcc->Base+Offset,Value);
}

STATIC VOID Acknowledge(CR_IPCC *Ipcc)
{
  UINTN I;
  for (I = 0; I < IPCC_BUDGET; ++I) {
    if(!MayRun(Ipcc))return;
    UINT32 Id = Read(Ipcc, IPCC_RECV_ID);
    if(!MayRun(Ipcc))return;
    if (Id == IPCC_EMPTY) return;
    if (Id == CR_IPCC_LPASS_GLINK) {
      /* Peripheral masking is ISR-safe in every OS. Preserve the latched
         signal until deferred work re-enables it, then service the SMEM FIFO. */
      Write(Ipcc, IPCC_SIGNAL_DISABLE, Id);
      CrAtomicOr32(&Ipcc->Pending, PENDING_GLINK);
      ++Ipcc->Received;
    } else ++Ipcc->OtherSignals;
    Write(Ipcc, IPCC_SIGNAL_CLEAR, Id);
    CrMemoryFence();
    /* As Linux's demultiplexer does, clear unknown boot-enabled doorbells
       without changing their masks or consuming other clients' SMEM. */
  }
  CrAtomicOr32(&Ipcc->Pending, PENDING_MORE);
}

STATIC VOID Interrupt(VOID *Context)
{
  CR_IPCC *Ipcc = Context;
  CR_STATUS Status;
  if (Ipcc == NULL || !Ipcc->Registered || !MayRun(Ipcc)) return;
  ++Ipcc->Irqs;
  Acknowledge(Ipcc);
  if (!CrAtomicLoad32(&Ipcc->Stopping) && CrAtomicLoad32(&Ipcc->Pending) != 0) {
    Status = Ipcc->Wake(Ipcc->Context);
    if (CR_ERROR(Status)) Ipcc->Error = Status;
  }
  /* OSKAL performs the OS interrupt completion. GLink/PMIC never run here. */
}

VOID CrIpccSend(UINTN Base)
{
  CrMemoryFence();
  CrMmioWrite32(Base + IPCC_SEND_ID, CR_IPCC_LPASS_GLINK);
}

CR_STATUS CrIpccStop(CR_IPCC *Ipcc)
{
  CR_STATUS Status;
  if (Ipcc == NULL) return CR_INVALID_PARAMETER;
  CrAtomicOr32(&Ipcc->Stopping, 1U);
  if (Ipcc->SignalOwned) {
    Write(Ipcc, IPCC_SIGNAL_DISABLE, CR_IPCC_LPASS_GLINK);
    CrMemoryFence();
    Ipcc->SignalOwned = FALSE;
  }
  if (Ipcc->Registered) {
    Status = CrUnregisterInterrupt(&Ipcc->Interrupt);
    if (CR_ERROR(Status)) return Status;
    Ipcc->Registered = FALSE;
  }
  if (Ipcc->ConfigChanged) {
    UINT32 Control = Read(Ipcc, IPCC_CONFIG);
    Control = (Control & ~IPCC_CLEAR_ON_READ) |
              (Ipcc->SavedClearOnRead ? IPCC_CLEAR_ON_READ : 0);
    Write(Ipcc, IPCC_CONFIG, Control);
    CrMemoryFence();
    Ipcc->ConfigChanged = FALSE;
  }
  CrAtomicAnd32(&Ipcc->Pending, 0);
  return CR_SUCCESS;
}

STATIC CR_STATUS Start(CR_IPCC *Ipcc, UINTN Base,CONST CR_IPCC_IO *Io, CONST CR_INTERRUPT_CONFIG *Config,
                      CR_IPCC_WAKE Wake, VOID *Context,CR_IPCC_ADMIT Admit,VOID *AdmitContext)
{
  CR_STATUS Status, Cleanup;
  CR_INTERRUPT_CONFIG Saved;
  UINT32 Control;
  if (Ipcc == NULL || Config == NULL || Wake == NULL ||
      (Io==NULL ? Base==0||(Base&3U) : Io->Read32==NULL||Io->Write32==NULL))
    return CR_INVALID_PARAMETER;
  if (Ipcc->Registered) return CR_ALREADY_STARTED;
  if(Admit!=NULL&&!Admit(AdmitContext))return CR_NOT_READY;
  Saved = *Config;
  CR_IPCC_IO SavedIo={0};if(Io)SavedIo=*Io;
  cr_memset(Ipcc, 0, sizeof(*Ipcc));
  Ipcc->Base = Base; Ipcc->Wake = Wake; Ipcc->Context = Context;
  Ipcc->Io=SavedIo;
  Ipcc->Admit=Admit;Ipcc->AdmitContext=AdmitContext;
  Ipcc->Stopping = 1;
  Ipcc->Interrupt = Saved;
  Ipcc->Interrupt.Handler = Interrupt;
  Ipcc->Interrupt.Param = Ipcc;
  /* OSKAL publishes its dispatch entry before registration can invoke ISR.
     Query retained ownership after failure; never unregister a foreign IRQ. */
  Ipcc->Registered = TRUE;
  Status = CrRegisterInterrupt(&Ipcc->Interrupt);
  Ipcc->Registered = CrInterruptIsRegistered(&Ipcc->Interrupt);
  if (!Ipcc->Registered) return CR_ERROR(Status) ? Status : CR_DEVICE_ERROR;
  if (CR_ERROR(Status)) goto Rollback;
  if(!MayRun(Ipcc)){Status=CR_NOT_READY;goto Rollback;}
  Control = Read(Ipcc, IPCC_CONFIG);
  if(!MayRun(Ipcc)){Status=CR_NOT_READY;goto Rollback;}
  Ipcc->SavedClearOnRead = (Control & IPCC_CLEAR_ON_READ) != 0;
  if (Ipcc->SavedClearOnRead) {
    Ipcc->ConfigChanged = TRUE;
    Write(Ipcc, IPCC_CONFIG, Control & ~IPCC_CLEAR_ON_READ);
    CrMemoryFence();
    if (Read(Ipcc, IPCC_CONFIG) & IPCC_CLEAR_ON_READ) {
      Status = CR_DEVICE_ERROR;
      goto Rollback;
    }
  }
  if(!MayRun(Ipcc)){Status=CR_NOT_READY;goto Rollback;}
  Ipcc->SignalOwned = TRUE;
  CrAtomicAnd32(&Ipcc->Stopping, 0);
  Write(Ipcc, IPCC_SIGNAL_ENABLE, CR_IPCC_LPASS_GLINK);
  CrMemoryFence();
  return CR_SUCCESS;
Rollback:
  Cleanup = CrIpccStop(Ipcc);
  return CR_ERROR(Cleanup) ? Cleanup : Status;
}
CR_STATUS CrIpccStart(CR_IPCC *Ipcc,UINTN Base,CONST CR_INTERRUPT_CONFIG *Config,CR_IPCC_WAKE Wake,VOID *Context) {
  return Start(Ipcc,Base,NULL,Config,Wake,Context,NULL,NULL);
}
CR_STATUS CrIpccStartGuarded(CR_IPCC *Ipcc,UINTN Base,CONST CR_INTERRUPT_CONFIG *Config,
  CR_IPCC_WAKE Wake,VOID *Context,CR_IPCC_ADMIT Admit,VOID *AdmitContext){
  if(Admit==NULL)return CR_INVALID_PARAMETER;
  return Start(Ipcc,Base,NULL,Config,Wake,Context,Admit,AdmitContext);
}
CR_STATUS CrIpccStartWithIo(CR_IPCC *Ipcc,CONST CR_IPCC_IO *Io,CONST CR_INTERRUPT_CONFIG *Config,CR_IPCC_WAKE Wake,VOID *Context) {
  if(Io==NULL)return CR_INVALID_PARAMETER;
  return Start(Ipcc,0,Io,Config,Wake,Context,NULL,NULL);
}

CR_STATUS CrIpccService(CR_IPCC *Ipcc, BOOLEAN *GlinkReceived)
{
  UINT32 Pending;
  if (Ipcc == NULL || GlinkReceived == NULL) return CR_INVALID_PARAMETER;
  *GlinkReceived = FALSE;
  if (!Ipcc->Registered || CrAtomicLoad32(&Ipcc->Stopping)) return CR_NOT_READY;
  if(!MayRun(Ipcc))return CR_NOT_READY;
  if (CR_ERROR(Ipcc->Error)) return Ipcc->Error;
  Pending = CrAtomicAnd32(&Ipcc->Pending, 0);
  if (Pending & PENDING_MORE) {
    Acknowledge(Ipcc);
    Pending |= CrAtomicAnd32(&Ipcc->Pending, ~PENDING_GLINK);
  }
  if (Pending & PENDING_GLINK) {
    if(!MayRun(Ipcc))return CR_NOT_READY;
    *GlinkReceived = TRUE;
    Write(Ipcc, IPCC_SIGNAL_ENABLE, CR_IPCC_LPASS_GLINK);
    CrMemoryFence();
  }
  /* A new interrupt while re-enabling publishes Pending for another work
     invocation. The caller continues bursts on a bounded deadline timer. */
  return CrAtomicLoad32(&Ipcc->Pending) != 0 ? CR_NOT_READY : CR_SUCCESS;
}
