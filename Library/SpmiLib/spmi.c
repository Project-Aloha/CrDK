/** @file
 *  OS-independent Qualcomm SPMI PMIC arbiter implementation.
 *
 *  The transaction and version behavior follows Linux spmi-pmic-arb. Target
 *  code supplies only resources and execution-environment configuration.
 *
 *  SPDX-License-Identifier: MIT
 */

#include "spmi_internal.h"
#include <Library/CrTargetSpmiLib.h>
#include <oskal/cr_string.h>
#ifdef _KERNEL_MODE
#include "spmi.tmh"
#endif

#define SPMI_ARB_VERSION_REGVAL_MIN_V2 0x20010000U
#define SPMI_ARB_VERSION_REGVAL_MIN_V3 0x30000000U
#define SPMI_ARB_VERSION_REGVAL_MIN_V5 0x50000000U
#define SPMI_ARB_VERSION_REGVAL_MIN_V7 0x70000000U
#define SPMI_ARB_VERSION_REGVAL_MIN_V8 0x80000000U
#define SPMI_ARB_VERSION_REGVAL_MIN_V8P5 0x80050000U

STATIC CONST SpmiPmicArbOps mSpmiArbOps[SPMI_ARB_VERSION_MAX] = {
    [SPMI_ARB_VERSION_1] =
        {
            PmicArbGetCoreResourceV1,
            PmicArbInitApidV1,
            PmicArbPpidToApidV1,
            PmicArbGetChannelOffsetV1,
            PmicArbFormatCmdV1,
            PmicArbCheckChannelStatusV1,
            PmicArbGetOwnerAccStatusV1,
            PmicArbGetAccEnableV1,
            PmicArbGetIrqStatusV1,
            PmicArbGetIrqClearV1,
            PmicArbGetApidOwnerV1,
            PmicArbGetApidMapOffsetV1,
        },
    [SPMI_ARB_VERSION_2] =
        {
            PmicArbGetCoreResourceV2,
            PmicArbInitApidV2,
            PmicArbPpidToApidV2,
            PmicArbGetChannelOffsetV2,
            PmicArbFormatCmdV2,
            PmicArbCheckChannelStatusV1,
            PmicArbGetOwnerAccStatusV2,
            PmicArbGetAccEnableV2,
            PmicArbGetIrqStatusV2,
            PmicArbGetIrqClearV2,
            PmicArbGetApidOwnerV2,
            PmicArbGetApidMapOffsetV2,
        },
    [SPMI_ARB_VERSION_3] =
        {
            PmicArbGetCoreResourceV2,
            PmicArbInitApidV2,
            PmicArbPpidToApidV2,
            PmicArbGetChannelOffsetV2,
            PmicArbFormatCmdV2,
            PmicArbCheckChannelStatusV1,
            PmicArbGetOwnerAccStatusV3,
            PmicArbGetAccEnableV2,
            PmicArbGetIrqStatusV2,
            PmicArbGetIrqClearV2,
            PmicArbGetApidOwnerV2,
            PmicArbGetApidMapOffsetV2,
        },
    [SPMI_ARB_VERSION_5] =
        {
            PmicArbGetCoreResourceV2,
            PmicArbInitApidV5,
            PmicArbPpidToApidV5,
            PmicArbGetChannelOffsetV5,
            PmicArbFormatCmdV2,
            PmicArbCheckChannelStatusV1,
            PmicArbGetOwnerAccStatusV5,
            PmicArbGetAccEnableV5,
            PmicArbGetIrqStatusV5,
            PmicArbGetIrqClearV5,
            PmicArbGetApidOwnerV2,
            PmicArbGetApidMapOffsetV5,
        },
    [SPMI_ARB_VERSION_7] =
        {
            PmicArbGetCoreResourceV7,
            PmicArbInitApidV7,
            PmicArbPpidToApidV5,
            PmicArbGetChannelOffsetV7,
            PmicArbFormatCmdV2,
            PmicArbCheckChannelStatusV1,
            PmicArbGetOwnerAccStatusV7,
            PmicArbGetAccEnableV7,
            PmicArbGetIrqStatusV7,
            PmicArbGetIrqClearV7,
            PmicArbGetApidOwnerV7,
            PmicArbGetApidMapOffsetV7,
        },
    [SPMI_ARB_VERSION_8] =
        {
            PmicArbGetCoreResourceV8,
            PmicArbInitApidV8,
            PmicArbPpidToApidV5,
            PmicArbGetChannelOffsetV8,
            PmicArbFormatCmdV2,
            PmicArbCheckChannelStatusV1,
            PmicArbGetOwnerAccStatusV7,
            PmicArbGetAccEnableV8,
            PmicArbGetIrqStatusV8,
            PmicArbGetIrqClearV8,
            PmicArbGetApidOwnerV8,
            PmicArbGetApidMapOffsetV8,
        },
    [SPMI_ARB_VERSION_8P5] =
        {
            PmicArbGetCoreResourceV8,
            PmicArbInitApidV8,
            PmicArbPpidToApidV5,
            PmicArbGetChannelOffsetV8,
            PmicArbFormatCmdV2,
            PmicArbCheckChannelStatusV8P5,
            PmicArbGetOwnerAccStatusV7,
            PmicArbGetAccEnableV8,
            PmicArbGetIrqStatusV8,
            PmicArbGetIrqClearV8,
            PmicArbGetApidOwnerV8,
            PmicArbGetApidMapOffsetV8,
        },
};

STATIC BOOLEAN SpmiRegionValid(IN SpmiDeviceContext *Ctx,
                               IN SPMI_MEMORY_REGION_TYPE RegionType) {
  return Ctx->Regions[RegionType].BaseAddress != 0 &&
         Ctx->Regions[RegionType].Size >= sizeof(UINT32);
}

STATIC SPMI_ARB_VERSION SpmiVersionFromRegister(IN UINT32 RegisterValue) {
  if (RegisterValue < SPMI_ARB_VERSION_REGVAL_MIN_V2) {
    return SPMI_ARB_VERSION_1;
  }
  if (RegisterValue < SPMI_ARB_VERSION_REGVAL_MIN_V3) {
    return SPMI_ARB_VERSION_2;
  }
  if (RegisterValue < SPMI_ARB_VERSION_REGVAL_MIN_V5) {
    return SPMI_ARB_VERSION_3;
  }
  if (RegisterValue < SPMI_ARB_VERSION_REGVAL_MIN_V7) {
    return SPMI_ARB_VERSION_5;
  }
  if (RegisterValue < SPMI_ARB_VERSION_REGVAL_MIN_V8) {
    return SPMI_ARB_VERSION_7;
  }
  if (RegisterValue < SPMI_ARB_VERSION_REGVAL_MIN_V8P5) {
    return SPMI_ARB_VERSION_8;
  }
  return SPMI_ARB_VERSION_8P5;
}

CR_STATUS
SpmiPmicArbBusInit(IN OUT SpmiDeviceContext *Ctx) {
  CR_STATUS Status;

  if (Ctx == NULL || Ctx->PmicArb.Ops == NULL ||
      Ctx->BusId >= Ctx->PmicArb.BusAvailable) {
    return CR_INVALID_PARAMETER;
  }

  Ctx->Bus.Id = Ctx->BusId;
  CrLockInit(&Ctx->Bus.Lock);
  CrLockInit(&Ctx->Bus.IrqLock);
  Ctx->Bus.MinApid = (UINT16)Ctx->PmicArb.MaxPeriphs;
  Ctx->Bus.MaxApid = 0;
  Status = Ctx->PmicArb.Ops->InitApid(Ctx, Ctx->BusId);
  if (CR_ERROR(Status)) {
    log_err("SPMI: APID initialization failed, Status=0x%X", Status);
  }
  return Status;
}

CR_STATUS
SpmiPmicArbProbe(IN OUT SpmiDeviceContext *Ctx) {
  CR_STATUS Status;
  UINT32 RegisterValue;

  if (Ctx == NULL || !SpmiRegionValid(Ctx, SPMI_MEMORY_REGION_TYPE_CORE)) {
    return CR_INVALID_PARAMETER;
  }
  if (Ctx->Channel > 5 || Ctx->ActiveEE > 5) {
    return CR_INVALID_PARAMETER;
  }

  RegisterValue = SpmiReadReg32(Ctx, SPMI_MEMORY_REGION_TYPE_CORE,
                                SPMI_PMIC_ARB_REG_VERSION);
  Ctx->Version = SpmiVersionFromRegister(RegisterValue);
  Ctx->PmicArb.Ops = &mSpmiArbOps[Ctx->Version];
  Status = Ctx->PmicArb.Ops->GetCoreResource(Ctx);
  if (CR_ERROR(Status)) {
    log_err("SPMI: failed to configure arbiter resources, version=0x%08X, "
            "Status=0x%X",
            RegisterValue, Status);
    return Status;
  }

  Status = SpmiPmicArbBusInit(Ctx);
  if (CR_ERROR(Status)) {
    return Status;
  }

  log_info("SPMI: PMIC arbiter register 0x%08X selected ABI version %u",
           RegisterValue, Ctx->Version);
  return CR_SUCCESS;
}

STATIC CR_STATUS SpmiValidateTransfer(IN SpmiDeviceContext *Ctx, IN UINT8 Sid,
                                      IN UINT16 Addr, IN CONST VOID *Buffer,
                                      IN UINTN Length) {
  UNREFERENCED_PARAMETER(Addr);

  if (Ctx == NULL || Buffer == NULL || !Ctx->Initialized || Sid > 0x0F ||
      Length == 0 || Length > SPMI_PMIC_ARB_MAX_TRANS_BYTES ||
      Ctx->PmicArb.Ops == NULL) {
    return CR_INVALID_PARAMETER;
  }
  return CR_SUCCESS;
}

STATIC UINT32 SpmiPackData(IN CONST UINT8 *Buffer, IN UINTN Length) {
  UINT32 Value = 0;
  UINTN Index;

  for (Index = 0; Index < Length; Index++) {
    Value |= (UINT32)Buffer[Index] << (Index * 8);
  }
  return Value;
}

STATIC VOID SpmiUnpackData(IN UINT32 Value, OUT UINT8 *Buffer,
                           IN UINTN Length) {
  UINTN Index;

  for (Index = 0; Index < Length; Index++) {
    Buffer[Index] = (UINT8)(Value >> (Index * 8));
  }
}

STATIC CR_STATUS SpmiFormatTransfer(IN SpmiDeviceContext *Ctx, IN BOOLEAN Write,
                                    IN UINT8 Sid, IN UINT16 Addr,
                                    IN UINTN Length, OUT UINT32 *Command,
                                    OUT UINT32 *Offset) {
  SPMI_ARB_CHANNEL_TYPE ChannelType;
  UINT8 Opcode;

  if (Command == NULL || Offset == NULL || Length == 0 ||
      Length > SPMI_PMIC_ARB_MAX_TRANS_BYTES) {
    return CR_INVALID_PARAMETER;
  }

  ChannelType = Write ? SPMI_ARB_CHANNEL_RW : SPMI_ARB_CHANNEL_OBS;
  Opcode = Write ? SPMI_ARB_OP_EXT_WRITEL : SPMI_ARB_OP_EXT_READL;
  *Offset = Ctx->PmicArb.Ops->GetChannelOffset(Ctx, Sid, Addr, ChannelType);
  if (*Offset == (UINT32)-1) {
    return CR_NOT_FOUND;
  }
  *Command =
      Ctx->PmicArb.Ops->FormatCmd(Opcode, Sid, Addr, (UINT8)(Length - 1));
  return CR_SUCCESS;
}

CR_STATUS
SpmiWaitForDone(IN SpmiDeviceContext *Ctx, IN UINTN BaseAddress,
                IN UINT32 Offset, IN UINT8 Sid, IN UINT16 Addr) {
  UINT32 Remaining;
  UINT32 ChannelStatus;
  CR_STATUS Status;

  for (Remaining = SPMI_PMIC_ARB_TIMEOUT_US; Remaining != 0; Remaining--) {
    ChannelStatus =
        SpmiIoRead32(Ctx, BaseAddress + Offset + SPMI_PMIC_ARB_STATUS);
    Status = Ctx->PmicArb.Ops->CheckChannelStatus(
        Ctx, ChannelStatus, Sid, Addr, Offset + SPMI_PMIC_ARB_STATUS);
    if (Status != CR_BUSY) {
      return Status;
    }
    SpmiIoStall(Ctx, 1);
  }

  log_err("SPMI: bus=%u sid=%u addr=0x%04X transaction timed out", Ctx->Bus.Id,
          Sid, Addr);
  return CR_TIMEOUT;
}

STATIC CR_STATUS SpmiReadUnlocked(IN SpmiDeviceContext *Ctx, IN UINT8 Sid,
                                  IN UINT16 Addr, OUT UINT8 *Buffer,
                                  IN UINTN Length) {
  CR_STATUS Status;
  UINT32 Command;
  UINT32 Offset;
  UINTN FirstLength;

  Status = SpmiFormatTransfer(Ctx, FALSE, Sid, Addr, Length, &Command, &Offset);
  if (CR_ERROR(Status)) {
    return Status;
  }

  SpmiIoWrite32(Ctx, Ctx->PmicArb.ReadAddr + Offset + SPMI_PMIC_ARB_CMD,
                Command);
  Status = SpmiWaitForDone(Ctx, Ctx->PmicArb.ReadAddr, Offset, Sid, Addr);
  if (CR_ERROR(Status)) {
    return Status;
  }

  FirstLength = Length > sizeof(UINT32) ? sizeof(UINT32) : Length;
  SpmiUnpackData(
      SpmiIoRead32(Ctx, Ctx->PmicArb.ReadAddr + Offset + SPMI_PMIC_ARB_RDATA0),
      Buffer, FirstLength);
  if (Length > sizeof(UINT32)) {
    SpmiUnpackData(SpmiIoRead32(Ctx, Ctx->PmicArb.ReadAddr + Offset +
                                         SPMI_PMIC_ARB_RDATA1),
                   Buffer + sizeof(UINT32), Length - sizeof(UINT32));
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS SpmiWriteUnlocked(IN SpmiDeviceContext *Ctx, IN UINT8 Sid,
                                   IN UINT16 Addr, IN CONST UINT8 *Buffer,
                                   IN UINTN Length) {
  CR_STATUS Status;
  UINT32 Command;
  UINT32 Offset;
  UINTN FirstLength;

  Status = SpmiFormatTransfer(Ctx, TRUE, Sid, Addr, Length, &Command, &Offset);
  if (CR_ERROR(Status)) {
    return Status;
  }

  FirstLength = Length > sizeof(UINT32) ? sizeof(UINT32) : Length;
  SpmiIoWrite32(Ctx, Ctx->PmicArb.WriteAddr + Offset + SPMI_PMIC_ARB_WDATA0,
                SpmiPackData(Buffer, FirstLength));
  if (Length > sizeof(UINT32)) {
    SpmiIoWrite32(
        Ctx, Ctx->PmicArb.WriteAddr + Offset + SPMI_PMIC_ARB_WDATA1,
        SpmiPackData(Buffer + sizeof(UINT32), Length - sizeof(UINT32)));
  }
  SpmiIoWrite32(Ctx, Ctx->PmicArb.WriteAddr + Offset + SPMI_PMIC_ARB_CMD,
                Command);
  return SpmiWaitForDone(Ctx, Ctx->PmicArb.WriteAddr, Offset, Sid, Addr);
}

CR_STATUS
SpmiRead(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT16 Addr,
         OUT UINT8 *Buf, IN UINTN Len) {
  CR_STATUS Status;

  Status = SpmiValidateTransfer(Ctx, Sid, Addr, Buf, Len);
  if (CR_ERROR(Status)) {
    return Status;
  }

  CrLockAcquire(&Ctx->Bus.Lock);
  Status = SpmiReadUnlocked(Ctx, Sid, Addr, Buf, Len);
  CrLockRelease(&Ctx->Bus.Lock);
  return Status;
}

CR_STATUS
SpmiWrite(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT16 Addr,
          IN CONST UINT8 *Buf, IN UINTN Len) {
  CR_STATUS Status;

  Status = SpmiValidateTransfer(Ctx, Sid, Addr, Buf, Len);
  if (CR_ERROR(Status)) {
    return Status;
  }

  CrLockAcquire(&Ctx->Bus.Lock);
  Status = SpmiWriteUnlocked(Ctx, Sid, Addr, Buf, Len);
  CrLockRelease(&Ctx->Bus.Lock);
  return Status;
}

CR_STATUS
SpmiMaskedWrite(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT16 Addr,
                IN CONST UINT8 *Buf, IN CONST UINT8 *Mask, IN UINTN Len) {
  CR_STATUS Status;
  UINT8 Temporary[SPMI_PMIC_ARB_MAX_TRANS_BYTES];
  UINTN Index;

  Status = SpmiValidateTransfer(Ctx, Sid, Addr, Buf, Len);
  if (CR_ERROR(Status) || Mask == NULL) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Ctx->Bus.Lock);
  Status = SpmiReadUnlocked(Ctx, Sid, Addr, Temporary, Len);
  if (!CR_ERROR(Status)) {
    for (Index = 0; Index < Len; Index++) {
      Temporary[Index] =
          (Temporary[Index] & (UINT8)~Mask[Index]) | (Buf[Index] & Mask[Index]);
    }
    Status = SpmiWriteUnlocked(Ctx, Sid, Addr, Temporary, Len);
  }
  CrLockRelease(&Ctx->Bus.Lock);
  return Status;
}

CR_STATUS
SpmiCommand(IN SpmiDeviceContext *Ctx, IN UINT8 Opc, IN UINT8 Sid) {
  CR_STATUS Status;
  UINT32 Offset;
  UINT32 Command;

  if (Ctx == NULL || !Ctx->Initialized || Sid > 0x0F || Opc < SPMI_CMD_RESET ||
      Opc > SPMI_CMD_WAKEUP) {
    return CR_INVALID_PARAMETER;
  }
  if (Ctx->Version != SPMI_ARB_VERSION_1) {
    return CR_UNSUPPORTED;
  }

  Offset = Ctx->PmicArb.Ops->GetChannelOffset(Ctx, Sid, 0, SPMI_ARB_CHANNEL_RW);
  if (Offset == (UINT32)-1) {
    return CR_NOT_FOUND;
  }
  Command = (UINT32)((Opc | 0x40U) << 27) | ((Sid & 0x0FU) << 20);

  CrLockAcquire(&Ctx->Bus.Lock);
  SpmiIoWrite32(Ctx, Ctx->PmicArb.WriteAddr + Offset + SPMI_PMIC_ARB_CMD,
                Command);
  Status = SpmiWaitForDone(Ctx, Ctx->PmicArb.WriteAddr, Offset, Sid, 0);
  CrLockRelease(&Ctx->Bus.Lock);
  return Status;
}

CR_STATUS
SpmiLibInit(IN OUT SpmiDeviceContext **Ctx) {
  CR_STATUS Status;

  if (Ctx == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (*Ctx == NULL) {
    *Ctx = CrTargetGetSpmiContext();
  }
  if (*Ctx == NULL) {
    return CR_NOT_FOUND;
  }
  if ((*Ctx)->Initialized) {
    return CR_SUCCESS;
  }

  Status = SpmiPmicArbProbe(*Ctx);
  if (CR_ERROR(Status)) {
    return Status;
  }

  (*Ctx)->Initialized = TRUE;
  if ((*Ctx)->HasInterrupt && !(*Ctx)->InterruptExternallyManaged) {
    Status = SpmiIrqInit(*Ctx);
    if (CR_ERROR(Status)) {
      (*Ctx)->Initialized = FALSE;
      return Status;
    }
  }
  return CR_SUCCESS;
}

CR_STATUS
SpmiLibDeinit(IN OUT SpmiDeviceContext *Ctx) {
  CR_STATUS FirstError;
  CR_STATUS Status;
  UINTN Index;
  BOOLEAN PendingIrq;

  if (Ctx == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (!Ctx->Initialized && !Ctx->InterruptRegistered) {
    return CR_SUCCESS;
  }

  FirstError = CR_SUCCESS;
  PendingIrq = FALSE;
  if (Ctx->Initialized) {
    for (Index = 0; Index < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES; Index++) {
      SpmiIrqEntry *Entry = &Ctx->IrqEntries[Index];

      if (Entry->Handler == NULL) {
        continue;
      }
      Status = SpmiUnregisterIrq(Ctx, Entry->Sid, Entry->Periph, Entry->Irq);
      if (CR_ERROR(Status)) {
        PendingIrq = TRUE;
        if (!CR_ERROR(FirstError)) {
          FirstError = Status;
        }
      } else {
        /* Keep a failed entry intact so a later deinit call can retry it. */
        cr_memset(Entry, 0, sizeof(*Entry));
      }
    }
  } else {
    for (Index = 0; Index < SPMI_PMIC_ARB_MAX_IRQ_ENTRIES; Index++) {
      if (Ctx->IrqEntries[Index].Handler != NULL) {
        PendingIrq = TRUE;
        break;
      }
    }
  }

  if (Ctx->InterruptRegistered) {
    Status = CrUnregisterInterrupt(&Ctx->InterruptConfig);
    if (CR_ERROR(Status)) {
      PendingIrq = TRUE;
      if (!CR_ERROR(FirstError)) {
        FirstError = Status;
      }
    } else {
      Ctx->InterruptRegistered = FALSE;
    }
  }

  if (!PendingIrq && !Ctx->InterruptRegistered) {
    cr_memset(Ctx->IrqEntries, 0, sizeof(Ctx->IrqEntries));
    cr_memset(Ctx->Bus.PpidToApid, 0, sizeof(Ctx->Bus.PpidToApid));
    cr_memset(Ctx->Bus.ApidData, 0, sizeof(Ctx->Bus.ApidData));
    Ctx->Bus.MinApid = (UINT16)Ctx->PmicArb.MaxPeriphs;
    Ctx->Bus.MaxApid = 0;
    Ctx->Bus.LastApid = 0;
    Ctx->Bus.ApidMapValid = FALSE;
    Ctx->Initialized = FALSE;
  }
  return FirstError;
}
