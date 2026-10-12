/** @file
 *  QPNPINT interrupt support for the Qualcomm SPMI PMIC arbiter.
 *
 *  SPDX-License-Identifier: MIT
 */

#include "spmi_internal.h"
#include <oskal/cr_string.h>
#ifdef _KERNEL_MODE
#include "spmi_irq.tmh"
#endif

/* Caller holds the same raw IRQ lock used by enable/register/dispatch. */
STATIC BOOLEAN ParentValid(SpmiParentIrqJournal *P) {
  return P && P->Identity==P && P->Context && P->Context->ParentIrq==P &&
    P->Count && P->Count<=SPMI_PARENT_IRQ_MAX_APIDS;
}
STATIC CR_STATUS ParentMaskLocked(SpmiParentIrqJournal *P,BOOLEAN Fault) {
  SpmiDeviceContext *C=P->Context;CR_STATUS First=CR_SUCCESS;
  if(Fault)P->Faulted=TRUE;
  /* Capture all desired bits before touching any hardware. Retries must not
     replace an earlier snapshot with the partially masked image. */
  if(!P->Blocked)for(UINTN I=0;I<P->Count;I++)
    P->Slots[I].Resume=(UINT8)(SpmiIoRead32(C,P->Slots[I].Address)&SPMI_PIC_ACC_ENABLE_BIT);
  P->Blocked=TRUE;
  for(UINTN I=0;I<P->Count;I++) {
    UINTN A=P->Slots[I].Address;UINT32 V=SpmiIoRead32(C,A);
    SpmiIoWrite32(C,A,V&~SPMI_PIC_ACC_ENABLE_BIT);
    if(SpmiIoRead32(C,A)&SPMI_PIC_ACC_ENABLE_BIT)First=CR_DEVICE_ERROR;
  }
  if(First!=CR_SUCCESS)P->Faulted=TRUE;
  return First;
}
CR_STATUS SpmiParentIrqAcquire(SpmiDeviceContext *C,SpmiParentIrqJournal *P) {
  if(!C||!P||!C->Initialized||!C->HasInterrupt||!C->InterruptExternallyManaged||
     C->InterruptRegistered||P->Context||P->Identity||C->ParentIrq)return CR_INVALID_PARAMETER;
  UINTN A=(UINTN)C,B=(UINTN)P;
  if(A>(UINTN)-1-sizeof(*C)||B>(UINTN)-1-sizeof(*P)||
     (A<B+sizeof(*P)&&B<A+sizeof(*C)))return CR_INVALID_PARAMETER;
  if(P->Generation==~(UINT64)0)return CR_OUT_OF_RESOURCES;
  if((C->Version!=SPMI_ARB_VERSION_5&&C->Version!=SPMI_ARB_VERSION_7)||
     !C->Bus.ApidCount||C->Bus.ApidCount>SPMI_PARENT_IRQ_MAX_APIDS||
     !SpmiApidValid(C,C->Bus.BaseApid)||
     !SpmiApidValid(C,(UINT16)(C->Bus.BaseApid+C->Bus.ApidCount-1)))return CR_UNSUPPORTED;
  CrLockAcquire(&C->Bus.IrqLock);CR_STATUS S=CR_SUCCESS;UINT16 Count=0;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++)if(C->IrqEntries[I].Handler){S=CR_BUSY;goto Done;}
  for(UINT32 I=C->Bus.BaseApid;I<(UINT32)C->Bus.BaseApid+C->Bus.ApidCount;I++) {
    if(C->Bus.ApidData[I].IrqEe!=C->ActiveEE)continue;
    if(!SpmiIrqApidFits(C,(UINT16)I)){S=CR_INVALID_PARAMETER;goto Done;}
    P->Slots[Count++].Address=C->PmicArb.Ops->GetAccEnable(C,(UINT16)I);
  }
  if(!Count){S=CR_UNSUPPORTED;goto Done;}
  for(UINTN I=0;I<Count;I++)P->Slots[I].Initial=
    (UINT8)(SpmiIoRead32(C,P->Slots[I].Address)&SPMI_PIC_ACC_ENABLE_BIT);
  P->Count=Count;P->Context=C;P->Identity=P;P->Blocked=P->Faulted=FALSE;
  ++P->Generation;C->ParentIrq=P;S=ParentMaskLocked(P,FALSE);
Done:
  CrLockRelease(&C->Bus.IrqLock);return S;
}
CR_STATUS SpmiParentIrqMask(SpmiParentIrqJournal *P,BOOLEAN Fault) {
  if(!ParentValid(P)||Fault>TRUE)return CR_INVALID_PARAMETER;
  SpmiDeviceContext *C=P->Context;CrLockAcquire(&C->Bus.IrqLock);
  CR_STATUS S=ParentMaskLocked(P,Fault);CrLockRelease(&C->Bus.IrqLock);return S;
}
CR_STATUS SpmiParentIrqResume(SpmiParentIrqJournal *P) {
  if(!ParentValid(P)||!P->Context->Initialized)return CR_INVALID_PARAMETER;
  SpmiDeviceContext *C=P->Context;CrLockAcquire(&C->Bus.IrqLock);CR_STATUS S=CR_SUCCESS;
  if(P->Faulted){S=CR_NOT_READY;goto Done;}
  if(!P->Blocked)goto Done;
  for(UINTN I=0;I<P->Count;I++) {
    UINTN A=P->Slots[I].Address;UINT32 V=SpmiIoRead32(C,A);
    SpmiIoWrite32(C,A,(V&~SPMI_PIC_ACC_ENABLE_BIT)|P->Slots[I].Resume);
    if((SpmiIoRead32(C,A)&SPMI_PIC_ACC_ENABLE_BIT)!=P->Slots[I].Resume){S=CR_DEVICE_ERROR;break;}
  }
  if(S==CR_SUCCESS)P->Blocked=FALSE;else (VOID)ParentMaskLocked(P,TRUE);
Done:
  CrLockRelease(&C->Bus.IrqLock);return S;
}
CR_STATUS SpmiParentIrqReleaseAfterDetach(SpmiParentIrqJournal *P) {
  if(!ParentValid(P))return CR_INVALID_PARAMETER;
  SpmiDeviceContext *C=P->Context;
  if(C->Initialized||C->InterruptRegistered)return CR_BUSY;
  CrLockAcquire(&C->Bus.IrqLock);CR_STATUS S=CR_SUCCESS;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++)if(C->IrqEntries[I].Handler){S=CR_BUSY;goto Done;}
  for(UINTN I=0;I<P->Count;I++) {
    UINTN A=P->Slots[I].Address;UINT32 V=SpmiIoRead32(C,A);
    SpmiIoWrite32(C,A,(V&~SPMI_PIC_ACC_ENABLE_BIT)|P->Slots[I].Initial);
    if((SpmiIoRead32(C,A)&SPMI_PIC_ACC_ENABLE_BIT)!=P->Slots[I].Initial){S=CR_DEVICE_ERROR;break;}
  }
  if(S==CR_SUCCESS){UINT64 Generation=P->Generation;C->ParentIrq=NULL;cr_memset(P,0,sizeof(*P));P->Generation=Generation;}
  else (VOID)ParentMaskLocked(P,TRUE);
Done:
  CrLockRelease(&C->Bus.IrqLock);return S;
}

STATIC SpmiIrqEntry *SpmiFindIrqEntry(IN SpmiDeviceContext *Ctx, IN UINT8 Sid,
                                      IN UINT8 Periph, IN UINT8 Irq) {
  UINTN Index;

  for (Index = 0; Index < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES; Index++) {
    SpmiIrqEntry *Entry = &Ctx->IrqEntries[Index];
    if (Entry->Handler != NULL && Entry->Sid == Sid &&
        Entry->Periph == Periph && Entry->Irq == Irq) {
      return Entry;
    }
  }
  return NULL;
}

STATIC SpmiIrqEntry *SpmiAllocateIrqEntry(IN SpmiDeviceContext *Ctx) {
  UINTN Index;

  for (Index = 0; Index < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES; Index++) {
    if (Ctx->IrqEntries[Index].Handler == NULL) {
      return &Ctx->IrqEntries[Index];
    }
  }
  return NULL;
}

CR_STATUS
SpmiGetApid(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT8 Periph,
            OUT UINT16 *Apid) {
  UINT16 Ppid;

  if (Ctx == NULL || Apid == NULL || !Ctx->Initialized || Sid > 0x0F ||
      Ctx->PmicArb.Ops == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Ppid = (UINT16)((Sid << 8) | Periph);
  if (Ppid >= SPMI_PMIC_ARB_MAX_PPID) {
    return CR_INVALID_PARAMETER;
  }
  UINT16 Result;
  CR_STATUS Status=Ctx->PmicArb.Ops->PpidToApid(Ctx,Ppid,&Result);
  if (CR_ERROR(Status)) return Status;
  if (!SpmiApidValid(Ctx,Result)) return CR_INVALID_PARAMETER;
  *Apid=Result;return CR_SUCCESS;
}

STATIC CR_STATUS SpmiSetIrqType(IN SpmiDeviceContext *Ctx, IN UINT8 Sid,
                                IN UINT8 Periph, IN UINT8 Irq,
                                IN CR_INTERRUPT_TRIGGER_TYPE TriggerType) {
  UINT8 Type[3] = {0, 0, 0};
  UINT8 Mask[3];
  UINT8 Bit;

  Bit = (UINT8)BIT(Irq);
  Mask[0] = Bit;
  Mask[1] = Bit;
  Mask[2] = Bit;

  switch (TriggerType) {
  case CR_INTERRUPT_TRIGGER_EDGE_RISING:
    Type[0] = Bit;
    Type[1] = Bit;
    break;
  case CR_INTERRUPT_TRIGGER_EDGE_FALLING:
    Type[0] = Bit;
    Type[2] = Bit;
    break;
  case CR_INTERRUPT_TRIGGER_EDGE_BOTH:
    Type[0] = Bit;
    Type[1] = Bit;
    Type[2] = Bit;
    break;
  case CR_INTERRUPT_TRIGGER_LEVEL_HIGH:
    Type[1] = Bit;
    break;
  case CR_INTERRUPT_TRIGGER_LEVEL_LOW:
    Type[2] = Bit;
    break;
  default:
    return CR_INVALID_PARAMETER;
  }

  return SpmiMaskedWrite(Ctx, Sid,
                         (UINT16)((Periph << 8) + SPMI_QPNPINT_REG_SET_TYPE),
                         Type, Mask, sizeof(Type));
}

STATIC VOID SpmiRecalculateApidBounds(IN OUT SpmiDeviceContext *Ctx) {
  UINTN Index;
  UINT16 Minimum = (UINT16)(Ctx->PmicArb.MaxPeriphs - 1);
  UINT16 Maximum = 0;
  BOOLEAN Found = FALSE;

  for (Index = 0; Index < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES; Index++) {
    SpmiIrqEntry *Entry = &Ctx->IrqEntries[Index];
    if (Entry->Handler == NULL) {
      continue;
    }
    Found = TRUE;
    if (Entry->Apid < Minimum) {
      Minimum = Entry->Apid;
    }
    if (Entry->Apid > Maximum) {
      Maximum = Entry->Apid;
    }
  }
  Ctx->Bus.MinApid = Found ? Minimum : (UINT16)Ctx->PmicArb.MaxPeriphs;
  Ctx->Bus.MaxApid = Found ? Maximum : 0;
}

CR_STATUS
SpmiRegisterIrq(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT8 Periph,
                IN UINT8 Irq, IN CR_INTERRUPT_TRIGGER_TYPE TriggerType,
                IN CR_INTERRUPT_HANDLER Handler, IN VOID *Param) {
  CR_STATUS Status;
  UINT16 Apid;
  UINT8 Bit;
  SpmiIrqEntry *Entry;

  if (Ctx == NULL || !Ctx->Initialized || Handler == NULL ||
      !Ctx->HasInterrupt || Sid>15 || Irq >= 8 ||
      (UINT32)TriggerType>CR_INTERRUPT_TRIGGER_EDGE_BOTH) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Ctx->Bus.IrqLock);
  if(Ctx->ParentIrq && Ctx->ParentIrq->Blocked){Status=CR_NOT_READY;goto Exit;}
  if (SpmiFindIrqEntry(Ctx, Sid, Periph, Irq) != NULL) {
    Status = CR_BUSY;
    goto Exit;
  }

  Status = SpmiGetApid(Ctx, Sid, Periph, &Apid);
  if (CR_ERROR(Status)) {
    goto Exit;
  }
  if (!SpmiIrqApidFits(Ctx,Apid)) { Status=CR_INVALID_PARAMETER;goto Exit; }
  UINT16 Address=(UINT16)((Periph<<8)+SPMI_QPNPINT_REG_SET_TYPE);
  UINT32 WriteOffset=Ctx->PmicArb.Ops->GetChannelOffset(Ctx,Sid,Address,SPMI_ARB_CHANNEL_RW);
  UINT32 ReadOffset=Ctx->PmicArb.Ops->GetChannelOffset(Ctx,Sid,Address,SPMI_ARB_CHANNEL_OBS);
  if (!SpmiChannelFits(Ctx,TRUE,WriteOffset,3) || !SpmiChannelFits(Ctx,FALSE,ReadOffset,3))
    { Status=CR_INVALID_PARAMETER;goto Exit; }
  if (Apid >= Ctx->PmicArb.MaxPeriphs ||
      Ctx->Bus.ApidData[Apid].IrqEe != Ctx->ActiveEE) {
    log_err("SPMI: sid=%u periph=0x%02X IRQ belongs to EE %u, active EE is %u",
            Sid, Periph, Ctx->Bus.ApidData[Apid].IrqEe, Ctx->ActiveEE);
    Status = CR_DEVICE_ERROR;
    goto Exit;
  }
  Entry = SpmiAllocateIrqEntry(Ctx);
  if (Entry == NULL) {
    Status = CR_OUT_OF_RESOURCES;
    goto Exit;
  }

  Bit = (UINT8)BIT(Irq);
  Status = SpmiWrite(
      Ctx, Sid, (UINT16)((Periph << 8) + SPMI_QPNPINT_REG_EN_CLR), &Bit, 1);
  if (!CR_ERROR(Status)) {
    Status = SpmiWrite(Ctx, Sid,
                       (UINT16)((Periph << 8) + SPMI_QPNPINT_REG_LATCHED_CLR),
                       &Bit, 1);
  }
  if (!CR_ERROR(Status)) {
    Status = SpmiSetIrqType(Ctx, Sid, Periph, Irq, TriggerType);
  }
  if (CR_ERROR(Status)) {
    goto Exit;
  }

  Entry->Sid = Sid;
  Entry->Periph = Periph;
  Entry->Irq = Irq;
  Entry->Apid = Apid;
  Entry->TriggerType = TriggerType;
  Entry->Handler = Handler;
  Entry->Param = Param;
  Entry->Enabled = FALSE;
  Entry->EnableUncertain = FALSE;
  SpmiRecalculateApidBounds(Ctx);

Exit:
  CrLockRelease(&Ctx->Bus.IrqLock);
  return Status;
}

STATIC CR_STATUS SpmiEnableIrqLocked(IN SpmiDeviceContext *Ctx, IN UINT8 Sid,
                                     IN UINT8 Periph, IN UINT8 Irq,
                                     IN BOOLEAN Enable) {
  CR_STATUS Status;
  SpmiIrqEntry *Entry;
  UINT8 Data[2];
  UINT8 Current;
  UINTN Index;
  BOOLEAN OtherEnabled = FALSE;

  if(Enable && Ctx->ParentIrq && Ctx->ParentIrq->Blocked)return CR_NOT_READY;
  Entry = SpmiFindIrqEntry(Ctx, Sid, Periph, Irq);
  if (Entry == NULL) {
    return CR_NOT_FOUND;
  }
  if (!SpmiIrqApidFits(Ctx,Entry->Apid)) return CR_INVALID_PARAMETER;
  UINT16 Address=(UINT16)((Periph<<8)+SPMI_QPNPINT_REG_EN_SET);
  UINT32 WriteOffset=Ctx->PmicArb.Ops->GetChannelOffset(Ctx,Sid,Address,SPMI_ARB_CHANNEL_RW);
  UINT32 ReadOffset=Ctx->PmicArb.Ops->GetChannelOffset(Ctx,Sid,Address,SPMI_ARB_CHANNEL_OBS);
  if (!SpmiChannelFits(Ctx,TRUE,WriteOffset,2) ||
      !SpmiChannelFits(Ctx,FALSE,ReadOffset,1)) return CR_INVALID_PARAMETER;

  Data[0] = (UINT8)BIT(Irq);
  Entry->EnableUncertain = TRUE;
  if (Enable) {
    SpmiIoWrite32(Ctx, Ctx->PmicArb.Ops->GetAccEnable(Ctx, Entry->Apid),
                  SPMI_PIC_ACC_ENABLE_BIT);
    if ((SpmiIoRead32(Ctx,Ctx->PmicArb.Ops->GetAccEnable(Ctx,Entry->Apid)) &
         SPMI_PIC_ACC_ENABLE_BIT) == 0) return CR_DEVICE_ERROR;
    Status =
        SpmiRead(Ctx, Sid, (UINT16)((Periph << 8) + SPMI_QPNPINT_REG_EN_SET),
                 &Current, 1);
    if (!CR_ERROR(Status) && (Current & Data[0]) == 0) {
      Data[1] = Data[0];
      Status = SpmiWrite(Ctx, Sid,
                         (UINT16)((Periph << 8) + SPMI_QPNPINT_REG_LATCHED_CLR),
                         Data, sizeof(Data));
    }
  } else {
    Status = SpmiWrite(
        Ctx, Sid, (UINT16)((Periph << 8) + SPMI_QPNPINT_REG_EN_CLR), Data, 1);
  }
  if (CR_ERROR(Status)) {
    return Status;
  }

  Status = SpmiRead(Ctx,Sid,(UINT16)((Periph << 8) + SPMI_QPNPINT_REG_EN_SET),
                    &Current,1);
  if (Status != CR_SUCCESS) return Status;
  if (((Current & Data[0]) != 0) != (Enable != FALSE)) return CR_DEVICE_ERROR;
  Entry->Enabled = Enable;
  Entry->EnableUncertain = FALSE;
  if (!Enable) {
    for (Index = 0; Index < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES; Index++) {
      if (Ctx->IrqEntries[Index].Handler != NULL &&
          Ctx->IrqEntries[Index].Apid == Entry->Apid &&
          (Ctx->IrqEntries[Index].Enabled || Ctx->IrqEntries[Index].EnableUncertain)) {
        OtherEnabled = TRUE;
        break;
      }
    }
    if (!OtherEnabled) {
      SpmiIoWrite32(Ctx, Ctx->PmicArb.Ops->GetAccEnable(Ctx, Entry->Apid), 0);
      if ((SpmiIoRead32(Ctx,Ctx->PmicArb.Ops->GetAccEnable(Ctx,Entry->Apid)) &
           SPMI_PIC_ACC_ENABLE_BIT) != 0) {
        Entry->EnableUncertain=TRUE;return CR_DEVICE_ERROR;
      }
    }
  }
  return CR_SUCCESS;
}

CR_STATUS
SpmiEnableIrq(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT8 Periph,
              IN UINT8 Irq, IN BOOLEAN Enable) {
  CR_STATUS Status;

  if (Ctx == NULL || !Ctx->Initialized || !Ctx->HasInterrupt || Enable > TRUE ||
      Irq >= 8) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Ctx->Bus.IrqLock);
  Status = SpmiEnableIrqLocked(Ctx, Sid, Periph, Irq, Enable);
  CrLockRelease(&Ctx->Bus.IrqLock);
  return Status;
}

CR_STATUS
SpmiUnregisterIrq(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT8 Periph,
                  IN UINT8 Irq) {
  CR_STATUS Status;
  SpmiIrqEntry *Entry;

  if (Ctx == NULL || !Ctx->Initialized || !Ctx->HasInterrupt || Irq >= 8) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Ctx->Bus.IrqLock);
  Entry = SpmiFindIrqEntry(Ctx, Sid, Periph, Irq);
  if (Entry == NULL) {
    Status = CR_NOT_FOUND;
    goto Exit;
  }
  Status = SpmiEnableIrqLocked(Ctx, Sid, Periph, Irq, FALSE);
  if (CR_ERROR(Status)) {
    goto Exit;
  }

  Entry->Handler = NULL;
  Entry->Param = NULL;
  Entry->Enabled = FALSE;
  Entry->EnableUncertain = FALSE;
  SpmiRecalculateApidBounds(Ctx);

Exit:
  CrLockRelease(&Ctx->Bus.IrqLock);
  return Status;
}

STATIC CR_STATUS SpmiHandlePeripheral(
    IN SpmiDeviceContext *Ctx, IN UINT16 Apid,
    OUT CR_INTERRUPT_HANDLER *Handlers, OUT VOID **Params,
    IN OUT UINTN *CallbackCount) {
  UINT16 Ppid=Ctx->Bus.ApidData[Apid].Ppid;
  UINT8 Sid=(UINT8)(Ppid>>8),Periph=(UINT8)Ppid;
  UINT32 Bits=SpmiIoRead32(Ctx,Ctx->PmicArb.Ops->GetIrqStatus(Ctx,Apid));
  CR_STATUS First=CR_SUCCESS;
  for (UINT8 Irq=0;Irq<8;++Irq) {
    if ((Bits&BIT(Irq))==0) continue;
    SpmiIrqEntry *Entry=SpmiFindIrqEntry(Ctx,Sid,Periph,Irq);
    UINT8 Clear=(UINT8)BIT(Irq);
    if (Entry==NULL || !Entry->Enabled || Entry->EnableUncertain || Entry->Handler==NULL) {
      /* Linux cleanup_irq clears the aggregator only for an unmapped bit.
         Never discard the PMIC latch of a foreign/uncertain source. */
      SpmiIoWrite32(Ctx,Ctx->PmicArb.Ops->GetIrqClear(Ctx,Apid),Clear);
      if (Ctx->IrqOrphans != ~(UINT32)0) ++Ctx->IrqOrphans;
      if (First==CR_SUCCESS) First=CR_NOT_READY;
      continue;
    }
    if (*CallbackCount>=SPMI_PMIC_ARB_MAX_IRQ_ENTRIES) {
      if (First==CR_SUCCESS) First=CR_OUT_OF_RESOURCES;
      continue;
    }
    SpmiIoWrite32(Ctx,Ctx->PmicArb.Ops->GetIrqClear(Ctx,Apid),Clear);
    CR_STATUS S=SpmiWrite(Ctx,Sid,(UINT16)((Periph<<8)+SPMI_QPNPINT_REG_LATCHED_CLR),&Clear,1);
    if (S!=CR_SUCCESS) {
      if (Ctx->IrqAckFailures != ~(UINT32)0) ++Ctx->IrqAckFailures;
      if (First==CR_SUCCESS) First=S;
      continue;
    }
    Handlers[*CallbackCount]=Entry->Handler;
    Params[*CallbackCount]=Entry->Param;
    ++*CallbackCount;
  }
  return First;
}

CR_STATUS
SpmiDispatchIrqs(IN SpmiDeviceContext *Ctx) {
  UINT16 First;
  UINT16 Last;
  UINT16 Group;
  UINT16 GroupOffset;
  UINT32 Status;
  UINT8 Bit;
  UINT16 Apid;
  BOOLEAN AccValid = FALSE;
  CR_INTERRUPT_HANDLER Handlers[SPMI_PMIC_ARB_MAX_IRQ_ENTRIES];
  VOID *Params[SPMI_PMIC_ARB_MAX_IRQ_ENTRIES];
  UINTN CallbackCount = 0;
  CR_STATUS FirstError=CR_SUCCESS,Handled;

  if (Ctx == NULL || !Ctx->Initialized || !Ctx->HasInterrupt) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Ctx->Bus.IrqLock);
  if(Ctx->ParentIrq && Ctx->ParentIrq->Blocked){CrLockRelease(&Ctx->Bus.IrqLock);return CR_NOT_READY;}
  if (!Ctx->SharedParentIrqScan && Ctx->Bus.MinApid > Ctx->Bus.MaxApid) {
    CrLockRelease(&Ctx->Bus.IrqLock);
    return CR_SUCCESS;
  }
  /* A shared parent owns the complete current-EE accumulator. A pending APID
     outside the registered clients must seal the parent instead of becoming
     an unbounded interrupt storm. Legacy dedicated users keep their window. */
  First = Ctx->SharedParentIrqScan ? Ctx->Bus.BaseApid : Ctx->Bus.MinApid;
  Last = Ctx->SharedParentIrqScan ?
    (UINT16)((UINT32)Ctx->Bus.BaseApid+Ctx->Bus.ApidCount-1) : Ctx->Bus.MaxApid;
  if (!SpmiApidValid(Ctx,First) || !SpmiApidValid(Ctx,Last) ||
      Ctx->PmicArb.Ops==NULL || Ctx->PmicArb.Ops->GetOwnerAccStatus==NULL) {
    CrLockRelease(&Ctx->Bus.IrqLock);return CR_INVALID_PARAMETER;
  }
  GroupOffset = (UINT16)(Ctx->Bus.BaseApid >> 5);

  /* Reject an incomplete interrupt window before acknowledging any APID. */
  for (Group=(UINT16)(First>>5);Group<=(UINT16)(Last>>5);++Group) {
    if (!SpmiAddressFits(Ctx,Ctx->PmicArb.Ops->GetOwnerAccStatus(
        Ctx,Ctx->ActiveEE,(UINT16)(Group-GroupOffset)),sizeof(UINT32))) {
      CrLockRelease(&Ctx->Bus.IrqLock);return CR_INVALID_PARAMETER;
    }
  }
  for (UINT32 Index=First;Index<=Last;++Index) {
    if (!SpmiIrqApidFits(Ctx,(UINT16)Index)) {
      CrLockRelease(&Ctx->Bus.IrqLock);return CR_INVALID_PARAMETER;
    }
  }

  for (Group = (UINT16)(First >> 5); Group <= (UINT16)(Last >> 5); Group++) {
    Status = SpmiIoRead32(
        Ctx, Ctx->PmicArb.Ops->GetOwnerAccStatus(
                 Ctx, Ctx->ActiveEE, (UINT16)(Group - GroupOffset)));
    if (Status != 0) {
      AccValid = TRUE;
    }
    for (Bit = 0; Bit < 32; Bit++) {
      if ((Status & BIT(Bit)) == 0) {
        continue;
      }
      Apid = (UINT16)(Group * 32 + Bit);
      if (Apid < First || Apid > Last ||
          Ctx->Bus.ApidData[Apid].IrqEe != Ctx->ActiveEE) {
        if (Ctx->SharedParentIrqScan && FirstError==CR_SUCCESS)
          FirstError=CR_NOT_READY;
        continue;
      }
      if ((SpmiIoRead32(Ctx, Ctx->PmicArb.Ops->GetAccEnable(Ctx, Apid)) &
           SPMI_PIC_ACC_ENABLE_BIT) != 0) {
        Handled=SpmiHandlePeripheral(Ctx,Apid,Handlers,Params,&CallbackCount);
        if (FirstError==CR_SUCCESS && Handled!=CR_SUCCESS) FirstError=Handled;
      }
    }
  }

  if (!AccValid) {
    for (Apid = First; Apid <= Last; Apid++) {
      if (Ctx->Bus.ApidData[Apid].IrqEe == Ctx->ActiveEE &&
          SpmiIoRead32(Ctx, Ctx->PmicArb.Ops->GetIrqStatus(Ctx, Apid)) != 0 &&
          (SpmiIoRead32(Ctx, Ctx->PmicArb.Ops->GetAccEnable(Ctx, Apid)) &
           SPMI_PIC_ACC_ENABLE_BIT) != 0) {
        Handled=SpmiHandlePeripheral(Ctx,Apid,Handlers,Params,&CallbackCount);
        if (FirstError==CR_SUCCESS && Handled!=CR_SUCCESS) FirstError=Handled;
      }
    }
  }

  if(FirstError!=CR_SUCCESS && Ctx->ParentIrq)(VOID)ParentMaskLocked(Ctx->ParentIrq,TRUE);
  CrLockRelease(&Ctx->Bus.IrqLock);
  for (Apid = 0; Apid < CallbackCount; Apid++) {
    Handlers[Apid](Params[Apid]);
  }
  return FirstError;
}

VOID SpmiIrqIsr(IN VOID *Context) {
  (VOID) SpmiDispatchIrqs((SpmiDeviceContext *)Context);
}

CR_STATUS
SpmiIrqInit(IN OUT SpmiDeviceContext *Ctx) {
  CR_STATUS Status;

  if (Ctx == NULL || !Ctx->HasInterrupt) {
    return CR_INVALID_PARAMETER;
  }
  if (Ctx->InterruptRegistered) {
    return CR_SUCCESS;
  }

  Ctx->InterruptConfig.Handler = SpmiIrqIsr;
  Ctx->InterruptConfig.Param = Ctx;
  Status = CrRegisterInterrupt(&Ctx->InterruptConfig);
  Ctx->InterruptRegistered = !CR_ERROR(Status) ||
                            CrInterruptIsRegistered(&Ctx->InterruptConfig);
  return Status;
}

CR_STATUS SpmiSetInterruptDelivery(IN OUT SpmiDeviceContext *Ctx,IN BOOLEAN Enable) {
  if (Ctx==NULL || Enable>1) return CR_INVALID_PARAMETER;
  if (Enable) {
    if (!Ctx->Initialized) return CR_NOT_READY;
    if (!Ctx->HasInterrupt || Ctx->InterruptExternallyManaged) return CR_UNSUPPORTED;
    return SpmiIrqInit(Ctx);
  }
  if (!Ctx->InterruptRegistered) return CR_SUCCESS;
  CR_STATUS Status=CrUnregisterInterrupt(&Ctx->InterruptConfig);
  if (!CR_ERROR(Status)) Ctx->InterruptRegistered=FALSE;
  return Status;
}
