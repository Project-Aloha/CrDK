/** @file
 *  QPNPINT interrupt support for the Qualcomm SPMI PMIC arbiter.
 *
 *  SPDX-License-Identifier: MIT
 */

#include "spmi_internal.h"
#ifdef _KERNEL_MODE
#include "spmi_irq.tmh"
#endif

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
  return Ctx->PmicArb.Ops->PpidToApid(Ctx, Ppid, Apid);
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
      !Ctx->HasInterrupt || Irq >= 8) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Ctx->Bus.IrqLock);
  if (SpmiFindIrqEntry(Ctx, Sid, Periph, Irq) != NULL) {
    Status = CR_BUSY;
    goto Exit;
  }

  Status = SpmiGetApid(Ctx, Sid, Periph, &Apid);
  if (CR_ERROR(Status)) {
    goto Exit;
  }
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

  Entry = SpmiFindIrqEntry(Ctx, Sid, Periph, Irq);
  if (Entry == NULL) {
    return CR_NOT_FOUND;
  }

  Data[0] = (UINT8)BIT(Irq);
  if (Enable) {
    SpmiIoWrite32(Ctx, Ctx->PmicArb.Ops->GetAccEnable(Ctx, Entry->Apid),
                  SPMI_PIC_ACC_ENABLE_BIT);
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

  Entry->Enabled = Enable;
  if (!Enable) {
    for (Index = 0; Index < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES; Index++) {
      if (Ctx->IrqEntries[Index].Handler != NULL &&
          Ctx->IrqEntries[Index].Apid == Entry->Apid &&
          Ctx->IrqEntries[Index].Enabled) {
        OtherEnabled = TRUE;
        break;
      }
    }
    if (!OtherEnabled) {
      SpmiIoWrite32(Ctx, Ctx->PmicArb.Ops->GetAccEnable(Ctx, Entry->Apid), 0);
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
  SpmiRecalculateApidBounds(Ctx);

Exit:
  CrLockRelease(&Ctx->Bus.IrqLock);
  return Status;
}

STATIC VOID SpmiHandlePeripheral(
    IN SpmiDeviceContext *Ctx, IN UINT16 Apid,
    OUT CR_INTERRUPT_HANDLER *Handlers, OUT VOID **Params,
    IN OUT UINTN *CallbackCount) {
  UINT32 Status;
  UINT16 Ppid;
  UINT8 Sid;
  UINT8 Periph;
  UINT8 Irq;
  UINT8 Clear;

  Ppid = Ctx->Bus.ApidData[Apid].Ppid;
  Sid = (UINT8)(Ppid >> 8);
  Periph = (UINT8)Ppid;
  Status = SpmiIoRead32(Ctx, Ctx->PmicArb.Ops->GetIrqStatus(Ctx, Apid));
  for (Irq = 0; Irq < 8; Irq++) {
    SpmiIrqEntry *Entry;
    if ((Status & BIT(Irq)) == 0) {
      continue;
    }
    Entry = SpmiFindIrqEntry(Ctx, Sid, Periph, Irq);
    if (Entry != NULL && Entry->Enabled && Entry->Handler != NULL &&
        *CallbackCount < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES) {
      /* Defer client callbacks until the IRQ lock is released. */
      Handlers[*CallbackCount] = Entry->Handler;
      Params[*CallbackCount]   = Entry->Param;
      (*CallbackCount)++;
    }

    Clear = (UINT8)BIT(Irq);
    SpmiIoWrite32(Ctx, Ctx->PmicArb.Ops->GetIrqClear(Ctx, Apid), Clear);
    if (CR_ERROR(SpmiWrite(
            Ctx, Sid, (UINT16)((Periph << 8) + SPMI_QPNPINT_REG_LATCHED_CLR),
            &Clear, 1))) {
      log_err("SPMI: failed to acknowledge sid=%u periph=0x%02X irq=%u", Sid,
              Periph, Irq);
    }
  }
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

  if (Ctx == NULL || !Ctx->Initialized || !Ctx->HasInterrupt) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Ctx->Bus.IrqLock);
  if (Ctx->Bus.MinApid > Ctx->Bus.MaxApid) {
    CrLockRelease(&Ctx->Bus.IrqLock);
    return CR_SUCCESS;
  }
  First = Ctx->Bus.MinApid;
  Last = Ctx->Bus.MaxApid;
  GroupOffset = (UINT16)(Ctx->Bus.BaseApid >> 5);

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
      if (Apid >= First && Apid <= Last &&
          (SpmiIoRead32(Ctx, Ctx->PmicArb.Ops->GetAccEnable(Ctx, Apid)) &
           SPMI_PIC_ACC_ENABLE_BIT) != 0) {
        SpmiHandlePeripheral(
            Ctx, Apid, Handlers, Params, &CallbackCount);
      }
    }
  }

  if (!AccValid) {
    for (Apid = First; Apid <= Last; Apid++) {
      if (Ctx->Bus.ApidData[Apid].IrqEe == Ctx->ActiveEE &&
          SpmiIoRead32(Ctx, Ctx->PmicArb.Ops->GetIrqStatus(Ctx, Apid)) != 0 &&
          (SpmiIoRead32(Ctx, Ctx->PmicArb.Ops->GetAccEnable(Ctx, Apid)) &
           SPMI_PIC_ACC_ENABLE_BIT) != 0) {
        SpmiHandlePeripheral(
            Ctx, Apid, Handlers, Params, &CallbackCount);
      }
    }
  }

  CrLockRelease(&Ctx->Bus.IrqLock);
  for (Apid = 0; Apid < CallbackCount; Apid++) {
    Handlers[Apid](Params[Apid]);
  }
  return CR_SUCCESS;
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
  if (!CR_ERROR(Status)) {
    Ctx->InterruptRegistered = TRUE;
  }
  return Status;
}
