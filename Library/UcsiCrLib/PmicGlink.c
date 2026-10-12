/**
  UCSICr SM8450 PMIC GLink and PAN notifications, adapted from Linux
  ucsi_glink.c, ucsi.c and pmic_glink_altmode.c.
  Copyright (c) 2019-2020, The Linux Foundation.
  Copyright (c) 2022-2023, Linaro Ltd.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include <ucsi/PmicGlink.h>
#include <oskal/cr_string.h>
#include <oskal/cr_debug.h>
#ifdef _KERNEL_MODE
#include "PmicGlink.tmh"
#endif

#define OWNER_USBC 32779U
#define OWNER_PAN 32780U
#define UCSI_READ 0x11U
#define UCSI_WRITE 0x12U
#define UCSI_NOTIFY 0x13U
#define PAN_WRITE 0x15U
#define PAN_NOTIFY 0x16U
#define PAN_ENABLE 0x10U
#define PAN_ACK 0x11U
#define CCI_COMPLETE (1U << 31)
#define CCI_ACK (1U << 29)
#define CCI_RESET (1U << 27)
#define CCI_BUSY (1U << 28)
#define CCI_ERROR (1U << 30)
#define CCI_UNSUPPORTED (1U << 25)
#define REQUEST_TIMEOUT_US 5000000ULL
/* Linux ucsi.c UCSI_TIMEOUT_MS. PPM RESET can take more than five seconds
   even when each ucsi_glink read/write completes within its own interval. */
#define COMPLETION_TIMEOUT_US 10000000ULL
#define RESET_POLL_DELAY_US 20000ULL
#define COMPLETION_RETRY_US 1000000ULL
#define COMPLETION_RETRIES 2U

enum { StageProbe, StageReset, StageEnableCommand, StageCapability,
       StageEnableAll, StageStatus, StageRunning };
enum { OpIdle, OpWrite, OpWriteWait, OpRead, OpReadWait, OpAck, OpAckWait };

STATIC UINT32 Get32(CONST UINT8 *Buffer)
{
  UINT32 Value;
  cr_memcpy(&Value, Buffer, sizeof(Value));
  return Value;
}
STATIC VOID Put32(UINT8 *Buffer, UINT32 Value) { cr_memcpy(Buffer, &Value, sizeof(Value)); }
STATIC VOID Header(UINT8 *Buffer, UINT32 Owner, UINT32 Opcode)
{ Put32(Buffer, Owner); Put32(Buffer + 4, 1); Put32(Buffer + 8, Opcode); }

VOID CrPmicInitialize(CR_PMIC_GLINK *Pmic, CR_PMIC_SEND Send, VOID *Context)
{
  cr_memset(Pmic, 0, sizeof(*Pmic));
  Pmic->Send = Send;
  Pmic->Context = Context;
  Pmic->State.LastError = CR_NOT_READY;
}

STATIC VOID Changed(CR_PMIC_GLINK *Pmic, CR_USB_TYPEC_STATE *State)
{
  State->Sequence = Pmic->State.Sequence;
  if (cr_memcmp(&Pmic->State, State, sizeof(*State)) != 0) {
    ++State->Sequence;
    Pmic->State = *State;
  }
}

VOID CrPmicFail(CR_PMIC_GLINK *Pmic, CR_STATUS Status)
{
  CR_USB_TYPEC_STATE State = {0};
  Pmic->Faulted = TRUE;
  Pmic->RetryAtUs = 0;
  State.HpdIrqSequence = Pmic->State.HpdIrqSequence;
  State.LastError = Status;
  Changed(Pmic, &State);
}

STATIC VOID PanFail(CR_PMIC_GLINK *Pmic, CR_STATUS Status)
{
  CR_USB_TYPEC_STATE State = Pmic->State;
  /* Linux/Android use independent PAN and UCSI clients. Failure to enable
     Alt Mode must not shut down connector status or USB role monitoring. */
  Pmic->PanFaulted = TRUE; Pmic->PanError = Status;
  Pmic->PanPending = Pmic->PanDone = Pmic->PanEnabled = FALSE;
  Pmic->PanAcks = 0;
  cr_memset(&Pmic->AltState, 0, sizeof(Pmic->AltState));
  Pmic->AltValid = FALSE;
  State.DpActive = State.Hpd = State.Usb3 = FALSE;
  State.DpLanes = State.PinAssignment = 0;
  Changed(Pmic, &State);
  log_warn("UCSICr: PAN command=0x%x failed " CR_LOG_STATUS_FMT "; UCSI connector monitoring continues\n",
          Pmic->PanCommand, Status);
}

STATIC VOID MergeAltMode(CR_PMIC_GLINK *Pmic, CR_USB_TYPEC_STATE *State)
{
  if (!State->PdReady || !State->Connected || Pmic->PanFaulted || !Pmic->AltValid) return;
  State->Orientation = Pmic->AltState.Orientation;
  State->DpActive = Pmic->AltState.DpActive;
  State->DpLanes = Pmic->AltState.DpLanes;
  State->PinAssignment = Pmic->AltState.PinAssignment;
  State->Hpd = Pmic->AltState.Hpd;
  State->Usb3 = Pmic->AltState.Usb3;
  State->HpdIrqSequence = Pmic->AltState.HpdIrqSequence;
}

STATIC VOID AltMode(CR_PMIC_GLINK *Pmic, CONST UINT8 *Data, UINTN Length)
{
  CR_USB_TYPEC_STATE State = Pmic->AltState;
  UINT32 Svid, Pin;
  UINT8 Mux;
  if (Length != 32 || Data[12] != 0 || Data[13] > 2 || Data[14] > 3) return;
  Svid = Get32(Data + 8) >> 16;
  Mux = Data[14];
  Pin = Data[20] & 0x3FU;
  State.Orientation = Data[13] == 0 ? UsbTypeCNormal :
    Data[13] == 1 ? UsbTypeCReverse : UsbTypeCUnoriented;
  State.DpActive = FALSE;
  State.DpLanes = State.PinAssignment = 0;
  State.Hpd = FALSE;
  State.Usb3 = Mux == 1;
  if (Svid == 0xFF01 && (Mux == 2 || Mux == 3) &&
      State.Orientation != UsbTypeCUnoriented && Pin >= 3 && Pin <= 6 &&
      ((Mux == 2 && (Pin == 3 || Pin == 5)) ||
       (Mux == 3 && (Pin == 4 || Pin == 6)))) {
    State.DpActive = TRUE;
    State.PinAssignment = (UINT8)Pin;
    State.DpLanes = Mux == 2 ? 4 : 2;
    State.Usb3 = Mux == 3;
    State.Hpd = (Data[20] & (1U << 6)) != 0;
    if ((Data[20] & (1U << 7)) != 0) ++State.HpdIrqSequence;
  }
  if (Mux == 0) State.Orientation = UsbTypeCUnoriented;
  /* Refresh connector status for a cable/mux change. HPD-only events retain
     their independent DP path and do not issue extra UCSI commands. */
  if (!Pmic->AltValid || State.Orientation != Pmic->AltState.Orientation ||
      State.DpActive != Pmic->AltState.DpActive || State.Usb3 != Pmic->AltState.Usb3 ||
      State.PinAssignment != Pmic->AltState.PinAssignment) {
    Pmic->StatusDirty = TRUE;
    Pmic->NextPollUs = 0;
  }
  Pmic->AltState = State;
  Pmic->AltValid = TRUE;
  log_info("UCSICr: PAN port=0 orientation=%u mux=%u SVID=0x%x "
         "pin=%u HPD=%u IRQ=%u UCSI-fault=%u\n",
          State.Orientation, Mux, Svid, Pin, State.Hpd,
          (Data[20] & (1U << 7)) != 0, Pmic->Faulted);
  /* PAN is an independent Linux/Android client. Retain and ACK its events
     while UCSI is unavailable, but activate no USB/DP path without status. */
  if (!Pmic->Faulted) {
    State = Pmic->State;
    MergeAltMode(Pmic, &State);
    Changed(Pmic, &State);
  }
  if (Pmic->PanAcks < 16U) ++Pmic->PanAcks;
  else PanFail(Pmic, CR_OUT_OF_RESOURCES);
}

VOID CrPmicReceive(CR_PMIC_GLINK *Pmic, CONST UINT8 *Data, UINTN Length)
{
  UINT32 Owner, Type, Opcode;
  if (Pmic == NULL || Data == NULL || Length < 12) return;
  Owner = Get32(Data); Type = Get32(Data + 4); Opcode = Get32(Data + 8);
  ++Pmic->RxMessages;
  Pmic->LastOwner = Owner; Pmic->LastType = Type;
  Pmic->LastOpcode = Opcode; Pmic->LastLength = (UINT32)Length;
  if (Pmic->RxMessages <= 16U)
    log_info("UCSICr: PMIC RX owner=%u type=%u opcode=0x%x bytes=%u\n",
            Owner, Type, Opcode, (UINT32)Length);
  /* Kernel callbacks dispatch by owner/opcode, not request-vs-notify type.
     Accept either defined PMIC envelope while retaining payload checks. */
  if (Type != 1 && Type != 2) return;
  if (Owner == OWNER_PAN) {
    if (Pmic->PanFaulted) return;
    if ((Opcode & 0xFFU) == PAN_NOTIFY) AltMode(Pmic, Data, Length);
    else if ((Opcode & 0xFFU) == PAN_WRITE &&
             Length >= 12 && Pmic->PanPending) Pmic->PanDone = TRUE;
    return;
  }
  if (Owner != OWNER_USBC) return;
  if (Opcode == UCSI_NOTIFY && Length == 24 && Get32(Data + 16) == 0) {
    ++Pmic->Notifications;
    log_info("UCSICr: UCSI notification CCI=0x%x fault=%u\n",
            Get32(Data + 12), Pmic->Faulted);
    /* Command/ACK notifications wake in-flight work without causing periodic
       GET_CONNECTOR_STATUS requests. UCSI connector numbers start at one. */
    if (((Get32(Data + 12) >> 1) & 0x7FU) == 1U) {
      Pmic->StatusDirty = TRUE;
      Pmic->NextPollUs = 0;
    }
    return;
  }
  if (Pmic->Faulted) return;
  if (Opcode == UCSI_READ && Pmic->ReadPending) {
    UINTN Size = Length == 64 ? 48 : Length == 544 ? 528 : 0;
    if (Size == 0) return;
    ++Pmic->ReadReplies;
    Pmic->ReplyStatus = Get32(Data + 12 + Size) == 0 ? CR_SUCCESS : CR_DEVICE_ERROR;
    if (!CR_ERROR(Pmic->ReplyStatus)) {
      UINT16 Version;
      cr_memcpy(&Version, Data + 12, sizeof(Version));
      if ((Version >= 0x200 && Size != 528) ||
          (Version < 0x200 && Size != 48) || Version < 0x100 || Version > 0x300 ||
          (Pmic->Version != 0 && Pmic->Version != Version)) {
        Pmic->ReplyStatus = CR_UNSUPPORTED;
      } else {
        Pmic->Version = Version;
        Pmic->BufferSize = Size;
        cr_memcpy(Pmic->ReadBuffer, Data + 12, Size);
        UINT32 Cci = Get32(Pmic->ReadBuffer + 4);
        if (Pmic->ReadReplies <= 8U ||
            (Pmic->Stage < StageRunning && Pmic->Cci != Cci) ||
            (Cci & (CCI_ERROR | CCI_UNSUPPORTED | CCI_BUSY)) != 0)
          log_info("UCSICr: UCSI version=0x%x stage=%u op=%u command=0x" CR_LOG_HEX64_FMT " CCI=0x%x\n",
                  Version, (UINT32)Pmic->Stage, (UINT32)Pmic->Operation, Pmic->Command, Cci);
        Pmic->Cci = Cci;
        if (((Pmic->Cci >> 8) & 0xFFU) > (Size == 48 ? 16U : 256U))
          Pmic->ReplyStatus = CR_COMPROMISED_DATA;
      }
    }
    Pmic->ReadDone = TRUE;
  } else if (Opcode == UCSI_WRITE && Length == 16 && Pmic->WritePending) {
    ++Pmic->WriteReplies;
    Pmic->ReplyStatus = Get32(Data + 12) == 0 ? CR_SUCCESS : CR_DEVICE_ERROR;
    Pmic->WriteDone = TRUE;
  }
}

STATIC CR_STATUS SendRead(CR_PMIC_GLINK *Pmic)
{
  UINT8 Data[12];
  CR_STATUS Status;
  Header(Data, OWNER_USBC, UCSI_READ);
  Status = Pmic->Send(Pmic->Context, Data, sizeof(Data));
  if (!CR_ERROR(Status)) { Pmic->ReadPending = TRUE; Pmic->ReadDone = FALSE; }
  return Status;
}
STATIC CR_STATUS SendControl(CR_PMIC_GLINK *Pmic, UINT64 Command)
{
  UINT8 Data[544];
  CR_STATUS Status;
  cr_memset(Data, 0, sizeof(Data));
  Header(Data, OWNER_USBC, UCSI_WRITE);
  cr_memcpy(Data + 20, &Command, sizeof(Command));
  Status = Pmic->Send(Pmic->Context, Data, Pmic->BufferSize + 16U);
  if (!CR_ERROR(Status)) {
    Pmic->WritePending = TRUE; Pmic->WriteDone = FALSE;
    if (Command == 1) ++Pmic->ResetWrites;
    if (Pmic->Stage < StageRunning || (Command != 0x10012 && (Command & 0xffU) != 4))
      log_info("UCSICr: UCSI control=0x" CR_LOG_HEX64_FMT " bytes=%u\n",
              Command, (UINT32)(Pmic->BufferSize + 16U));
  }
  return Status;
}

STATIC CR_STATUS PanTick(CR_PMIC_GLINK *Pmic, UINT64 NowUs)
{
  UINT8 Data[24];
  CR_STATUS Status;
  if (Pmic->PanFaulted) return CR_SUCCESS;
  if (Pmic->PanPending) {
    if (Pmic->PanDone) {
      if (Pmic->PanCommand == PAN_ENABLE) {
        Pmic->PanEnabled = TRUE;
        log_info("UCSICr: PAN notifications enabled\n");
      }
      else if (Pmic->PanAcks != 0) --Pmic->PanAcks;
      Pmic->PanPending = Pmic->PanDone = FALSE;
    } else if (NowUs >= Pmic->PanDeadlineUs) return CR_TIMEOUT;
    else return CR_SUCCESS;
  }
  if (Pmic->PanEnabled && Pmic->PanAcks == 0) return CR_SUCCESS;
  cr_memset(Data, 0, sizeof(Data));
  Header(Data, OWNER_PAN, PAN_WRITE);
  Pmic->PanCommand = Pmic->PanEnabled ? PAN_ACK : PAN_ENABLE;
  Put32(Data + 12, Pmic->PanCommand);
  Status = Pmic->Send(Pmic->Context, Data, sizeof(Data));
  if (Status == CR_NOT_READY) return CR_SUCCESS;
  if (!CR_ERROR(Status)) {
    Pmic->PanPending = TRUE;
    Pmic->PanDeadlineUs = NowUs + REQUEST_TIMEOUT_US;
  }
  return Status;
}

STATIC CR_STATUS FinishCommand(CR_PMIC_GLINK *Pmic)
{
  UINTN Length = (Pmic->Cci >> 8) & 0xFFU;
  CR_USB_TYPEC_STATE State = Pmic->State;
  if ((Pmic->Command & 0xFFU) == 0x12) {
    UINT32 Word, Partner;
    if (Length < 9) return CR_COMPROMISED_DATA;
    Word = Get32(Pmic->ReadBuffer + 16);
    Partner = Word >> 29;
    State.PdReady = TRUE;
    State.LastError = CR_SUCCESS;
    State.Connected = (Word & (1U << 19)) != 0;
    State.PowerSource = State.Connected && (Word & (1U << 20)) != 0;
    State.DataRole = !State.Connected ? UsbTypeCNone : Partner == 1 ? UsbTypeCDevice :
      (Partner == 2 || Partner == 4) ? UsbTypeCHost : UsbTypeCNone;
    if (!State.Connected) {
      State.Orientation = UsbTypeCUnoriented;
      State.DpActive = State.Hpd = State.Usb3 = FALSE;
      State.PinAssignment = State.DpLanes = 0;
      if (Pmic->State.Connected) Pmic->AltValid = FALSE;
    } else MergeAltMode(Pmic, &State);
    Changed(Pmic, &State);
  } else if (Pmic->Stage == StageCapability) {
    if (Length < 16 || Pmic->ReadBuffer[20] == 0) return CR_COMPROMISED_DATA;
  }
  return CR_SUCCESS;
}

CR_STATUS CrPmicTick(CR_PMIC_GLINK *Pmic, UINT64 NowUs)
{
  CR_STATUS Status;
  if (Pmic == NULL || Pmic->Send == NULL) return CR_INVALID_PARAMETER;
  Status = PanTick(Pmic, NowUs);
  if (CR_ERROR(Status)) { PanFail(Pmic, Status); Status = CR_SUCCESS; }
  if (Pmic->Faulted) {
    if (Pmic->RetryAtUs == 0 || NowUs < Pmic->RetryAtUs)
      return Pmic->State.LastError;
    ++Pmic->Retries;
    log_warn("UCSICr: retrying acknowledged UCSI completion timeout "
           "(%u/%u); PAN monitoring retained\n", Pmic->Retries, COMPLETION_RETRIES);
    Pmic->Faulted = FALSE; Pmic->RetryAtUs = 0;
    Pmic->ReadPending = Pmic->ReadDone = Pmic->WritePending = Pmic->WriteDone = FALSE;
    Pmic->Stage = StageProbe; Pmic->Operation = OpIdle;
    Pmic->Command = Pmic->RequestedCommand = 0;
    Pmic->Version = 0; Pmic->BufferSize = 0; Pmic->Cci = 0;
    Pmic->DeadlineUs = Pmic->ReplyDeadlineUs = Pmic->NextPollUs = 0;
    Pmic->ResetClearing = Pmic->CompletionStarted = FALSE;
    CR_USB_TYPEC_STATE State = Pmic->State;
    State.LastError = CR_NOT_READY;
    Changed(Pmic, &State);
  }
  BOOLEAN Waiting = Pmic->Operation == OpReadWait ||
    Pmic->Operation == OpWriteWait || Pmic->Operation == OpAckWait;
  if (Waiting) {
    BOOLEAN Done = Pmic->Operation == OpReadWait ? Pmic->ReadDone : Pmic->WriteDone;
    /* Linux's blocking transport read/write has its own timeout. A read
       issued near the PPM completion deadline must still receive its reply
       before we stop polling or retry a command without transaction IDs.
       Process a delivered reply even if this timer invocation is delayed. */
    if (!Done) {
      if (NowUs < Pmic->ReplyDeadlineUs) return CR_SUCCESS;
      Status = CR_TIMEOUT;
      goto Fault;
    }
  } else if (Pmic->Operation != OpIdle && NowUs >= Pmic->DeadlineUs) {
    /* A completed transport read with stale CCI can exhaust the preliminary
       clear loop. Like ucsi_reset_ppm(), try the actual reset anyway. A
       missing transport acknowledgement still faults below. */
    if (Pmic->Stage == StageReset && Pmic->ResetClearing &&
        Pmic->Operation == OpRead) {
      Pmic->ReadPending = FALSE;
      Pmic->ResetClearing = FALSE; Pmic->Operation = OpIdle;
      Pmic->NextPollUs = 0;
      return CR_SUCCESS;
    }
    Status = CR_TIMEOUT;
    goto Fault;
  }
  switch (Pmic->Operation) {
    case OpIdle:
      if (Pmic->Stage == StageRunning && Pmic->RequestedCommand == 0 && !Pmic->StatusDirty)
        return CR_SUCCESS;
      if (NowUs < Pmic->NextPollUs) return CR_SUCCESS;
      Pmic->DeadlineUs = NowUs + REQUEST_TIMEOUT_US;
      Pmic->CompletionStarted = FALSE;
      if (Pmic->Stage == StageProbe) Pmic->Operation = OpRead;
      else {
        if (Pmic->Stage == StageReset) Pmic->Command = Pmic->ResetClearing ? 5 : 1;
        else if (Pmic->Stage == StageEnableCommand) Pmic->Command = 0x10005;
        else if (Pmic->Stage == StageCapability) Pmic->Command = 6;
        else if (Pmic->Stage == StageEnableAll) Pmic->Command = 0xDBE70005ULL;
        else {
          Pmic->Command = Pmic->RequestedCommand != 0 ? Pmic->RequestedCommand : 0x10012;
          Pmic->RequestedCommand = 0;
          /* Consume at request start. A change during this command/ACK is
             retained for a later query, rather than lost at completion. */
          if (Pmic->Command == 0x10012) Pmic->StatusDirty = FALSE;
          else Pmic->StatusDirty = TRUE; /* observe an accepted role request */
        }
        Pmic->Operation = OpWrite;
      }
      return CR_SUCCESS;
    case OpWrite:
      Status = SendControl(Pmic, Pmic->Command);
      if (!CR_ERROR(Status)) {
        Pmic->ReplyDeadlineUs = NowUs + REQUEST_TIMEOUT_US;
        Pmic->Operation = OpWriteWait;
      }
      break;
    case OpWriteWait:
    case OpAckWait:
      if (!Pmic->WriteDone) return CR_SUCCESS;
      Pmic->WritePending = FALSE;
      Status = Pmic->ReplyStatus;
      if (!CR_ERROR(Status)) {
        /* ucsi_reset_ppm starts its completion interval after the write
           transport ACK. RESET resends keep that original interval. */
        if (!Pmic->CompletionStarted) {
          Pmic->DeadlineUs = NowUs + COMPLETION_TIMEOUT_US;
          Pmic->CompletionStarted = TRUE;
          if (Pmic->Stage == StageReset && Pmic->Command == 1)
            log_info("UCSICr: PPM RESET accepted; completion timeout=10000 ms\n");
        }
        Pmic->Operation = OpRead;
        /* ucsi_reset_ppm() sleeps 20ms after every RESET write, giving the
           PPM time to consume it before examining a previous command's CCI. */
        if (Pmic->Stage == StageReset && Pmic->Command == 1)
          Pmic->NextPollUs = NowUs + RESET_POLL_DELAY_US;
      }
      /* OpAck is distinguished by the command's high internal flag. */
      break;
    case OpRead:
      if (NowUs < Pmic->NextPollUs) return CR_SUCCESS;
      Status = SendRead(Pmic);
      if (!CR_ERROR(Status)) {
        Pmic->ReplyDeadlineUs = NowUs + REQUEST_TIMEOUT_US;
        Pmic->Operation = OpReadWait;
      }
      break;
    case OpReadWait:
      if (!Pmic->ReadDone) return CR_SUCCESS;
      Pmic->ReadPending = FALSE;
      Status = Pmic->ReplyStatus;
      if (CR_ERROR(Status)) break;
      if (Pmic->Stage == StageProbe) {
        Pmic->ResetClearing = (Pmic->Cci & CCI_RESET) != 0;
        Pmic->Stage = StageReset;
        Pmic->Operation = OpIdle;
        break;
      }
      if (Pmic->Stage == StageReset) {
        if (Pmic->ResetClearing) {
          if ((Pmic->Cci & CCI_COMPLETE) || NowUs >= Pmic->DeadlineUs) {
            Pmic->ResetClearing = FALSE; Pmic->Operation = OpIdle;
            Pmic->NextPollUs = 0;
          } else Pmic->Operation = OpRead;
        } else if (Pmic->Cci == CCI_RESET) {
          ++Pmic->Stage; Pmic->Operation = OpIdle;
          Pmic->NextPollUs = 0;
        } else {
          /* If the PPM still reports another command, resend RESET rather
             than poll that stale completion until the timeout. */
          Pmic->Operation = Pmic->Cci != 0 ? OpWrite : OpRead;
          if (Pmic->Operation == OpRead)
            Pmic->NextPollUs = NowUs + RESET_POLL_DELAY_US;
        }
        break;
      }
      if (Pmic->Cci & (CCI_ERROR | CCI_UNSUPPORTED)) {
        Status = Pmic->Cci & CCI_UNSUPPORTED ? CR_UNSUPPORTED : CR_DEVICE_ERROR;
        break;
      }
      if ((Pmic->Command & (1ULL << 63)) != 0) {
        if (!(Pmic->Cci & CCI_ACK)) { Pmic->Operation = OpRead; break; }
        Pmic->Command &= ~(1ULL << 63);
        if (Pmic->Stage < StageRunning) ++Pmic->Stage;
        Pmic->Operation = OpIdle;
        Pmic->NextPollUs = 0;
      } else if ((Pmic->Cci & CCI_BUSY) || !(Pmic->Cci & CCI_COMPLETE)) {
        Pmic->Operation = OpRead;
      } else {
        Status = FinishCommand(Pmic);
        if (!CR_ERROR(Status)) {
          Pmic->Operation = OpAck;
          Pmic->DeadlineUs = NowUs + REQUEST_TIMEOUT_US;
          Pmic->CompletionStarted = FALSE;
        }
      }
      break;
    case OpAck: {
      UINT64 Command = 0x20004;
      if (((Pmic->Cci >> 1) & 0x7FU) != 0) Command |= (1U << 16);
      Status = SendControl(Pmic, Command);
      if (!CR_ERROR(Status)) {
        Pmic->Command |= (1ULL << 63);
        Pmic->Operation = OpAckWait;
        Pmic->ReplyDeadlineUs = NowUs + REQUEST_TIMEOUT_US;
      }
      break;
    }
    default:
      Status = CR_COMPROMISED_DATA;
      break;
  }
  if (Status == CR_NOT_READY) return CR_SUCCESS;
  if (!CR_ERROR(Status)) return Status;
Fault:
  if (Status == CR_TIMEOUT)
    log_err("UCSICr: timeout scope=" CR_LOG_CHAR8_STR_FMT " now=" CR_LOG_UINT64_FMT " command-deadline=" CR_LOG_UINT64_FMT " "
           "reply-deadline=" CR_LOG_UINT64_FMT " PAN-enabled=%u pending=%u fault=%u\n",
            Waiting ? "transport" : Pmic->CompletionStarted ? "completion" : "acquisition",
            NowUs, Pmic->DeadlineUs, Pmic->ReplyDeadlineUs,
            Pmic->PanEnabled, Pmic->PanPending, Pmic->PanFaulted);
  log_err("UCSICr: UCSI failure=" CR_LOG_STATUS_FMT " stage=%u op=%u command=0x" CR_LOG_HEX64_FMT " CCI=0x%x "
         "read=%u/%u write=%u/%u RX=%u notifications=%u resets=%u last=%u/%u/0x%x/%u\n",
          Status, (UINT32)Pmic->Stage, (UINT32)Pmic->Operation, Pmic->Command, Pmic->Cci,
          Pmic->ReadPending, Pmic->ReadReplies, Pmic->WritePending, Pmic->WriteReplies,
          Pmic->RxMessages, Pmic->Notifications, Pmic->ResetWrites,
          Pmic->LastOwner, Pmic->LastType, Pmic->LastOpcode, Pmic->LastLength);
  if (Pmic->BufferSize >= 24U) {
    UINT64 Control;
    cr_memcpy(&Control, Pmic->ReadBuffer + 8, sizeof(Control));
    log_err("UCSICr: PPM snapshot version=0x%x CONTROL=0x" CR_LOG_HEX64_FMT " "
           "MESSAGE_IN=0x%x/0x%x\n", Pmic->Version, Control,
            Get32(Pmic->ReadBuffer + 16), Get32(Pmic->ReadBuffer + 20));
  }
  CrPmicFail(Pmic, Status);
  /* PMIC replies have no transaction IDs. Retry only a completion poll
     after all transport replies were received; a missing read/write ACK
     cannot be safely correlated with a subsequent request. */
  if (Status == CR_TIMEOUT && Pmic->Operation == OpRead &&
      Pmic->CompletionStarted &&
      !Pmic->ReadPending && !Pmic->WritePending &&
      Pmic->Retries < COMPLETION_RETRIES)
    Pmic->RetryAtUs = NowUs + COMPLETION_RETRY_US;
  return Status;
}

CR_STATUS CrPmicRequestRole(CR_PMIC_GLINK *Pmic, BOOLEAN Power, UINT32 Role)
{
  if (Pmic == NULL || Role < 1 || Role > 2) return CR_INVALID_PARAMETER;
  if (!Pmic->State.PdReady || !Pmic->State.Connected || Pmic->Faulted) return CR_NOT_READY;
  if (Pmic->RequestedCommand != 0) return CR_ALREADY_STARTED;
  Pmic->RequestedCommand = (Power ? 0xBU : 9U) | (1U << 16) | ((UINT64)Role << 23) | (1U << 25);
  Pmic->NextPollUs = 0;
  return CR_SUCCESS;
}

BOOLEAN CrPmicNeedsService(CONST CR_PMIC_GLINK *Pmic)
{
  if (Pmic == NULL) return FALSE;
  if (!Pmic->PanFaulted && (!Pmic->PanEnabled || Pmic->PanPending || Pmic->PanAcks != 0))
    return TRUE;
  if (Pmic->Faulted) return Pmic->RetryAtUs != 0;
  return Pmic->Stage != StageRunning || Pmic->Operation != OpIdle ||
         Pmic->StatusDirty || Pmic->RequestedCommand != 0;
}
