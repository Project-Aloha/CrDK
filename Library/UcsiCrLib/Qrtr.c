/**
  Bounded QRTR endpoint, SM8450 PD mapper and independent charger/audio PDR clients.
  Adapted from Linux net/qrtr/{af_qrtr,ns}.c and
  drivers/soc/qcom/{qcom_pd_mapper,pdr_interface,qcom_pdr_msg}.c.
  Copyright (c) 2013, 2020, The Linux Foundation.
  Copyright (c) 2015, Sony Mobile Communications Inc.
  Copyright (c) 2023, Linaro Ltd.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include <ucsi/Qrtr.h>
#include <oskal/cr_string.h>
#include <oskal/cr_debug.h>
#ifdef _KERNEL_MODE
#if !defined(CR_OSK_LOG_WITHOUT_WPP)
#include "Qrtr.tmh"
#endif
#endif

#define LOCAL_NODE 1U
#define CONTROL_PORT 0xFFFFFFFEU
#define MAPPER_PORT 0x4000U
#define PDR_PORT 0x4001U
#define AUDIO_PDR_PORT 0x4002U
#define PDR_INSTANCE ((74U << 8) | 1U)
#define PD_UP 0x1FFFFFFFU
#define PD_DOWN 0x0FFFFFFFU
#define PD_EARLY_DOWN 0x2FFFFFFFU
#define PD_UNINIT 0x7FFFFFFFU
STATIC CONST CHAR8 mChargerPath[] = "msm/adsp/charger_pd";
STATIC CONST CHAR8 mAudioPath[] = "msm/adsp/audio_pd";
STATIC VOID AudioForget(CR_QRTR *Router);
STATIC VOID AudioServer(CR_QRTR *Router, UINT32 Node, UINT32 Port);
STATIC VOID AudioFault(CR_QRTR *Router, CR_STATUS Reason);

STATIC UINT16 Get16(CONST UINT8 *Data)
{ UINT16 Value; cr_memcpy(&Value, Data, 2); return Value; }
STATIC UINT32 Get32(CONST UINT8 *Data)
{ UINT32 Value; cr_memcpy(&Value, Data, 4); return Value; }
STATIC VOID Put16(UINT8 *Data, UINT16 Value) { cr_memcpy(Data, &Value, 2); }
STATIC VOID Put32(UINT8 *Data, UINT32 Value) { cr_memcpy(Data, &Value, 4); }
STATIC UINTN TextLength(CONST CHAR8 *Text)
{ UINTN Length = 0; while (Text[Length] != 0) ++Length; return Length; }
STATIC BOOLEAN TextIs(CONST UINT8 *Data, UINTN Length, CONST CHAR8 *Text)
{ return Length == TextLength(Text) && cr_memcmp(Data, Text, Length) == 0; }

VOID CrQrtrInitialize(CR_QRTR *Router, CR_QRTR_SEND Send, VOID *Context)
{
  cr_memset(Router, 0, sizeof(*Router));
  Router->Send = Send; Router->Context = Context;
  Router->LastError = CR_NOT_READY;
}

VOID CrQrtrOnline(CR_QRTR *Router, BOOLEAN Online)
{
  CR_QRTR_SEND Send;
  VOID *Context;
  UINT32 Generation;
  UINT16 Transaction;
  BOOLEAN PdKnown;
  CR_QRTR_AUDIO_PD Audio;
  if (Router == NULL) return;
  if (Router->Online == Online) return;
  Send = Router->Send; Context = Router->Context;
  Generation = Router->PdGeneration + 1U;
  Transaction = Router->Transaction; PdKnown = Router->PdKnown;
  Audio = Router->Audio;
  CrQrtrInitialize(Router, Send, Context);
  Router->Audio = Audio;
  AudioForget(Router);
  Router->Online = Online; Router->PdGeneration = Generation;
  /* Once the notifier has been observed, loss of its transport is DOWN.
     Do not silently revert to the RPMSG-only startup fallback on reopen. */
  Router->PdKnown = PdKnown; Router->Transaction = Transaction;
  log_info("UCSICr: IPCRTR " CR_LOG_CHAR8_STR_FMT "; native QRTR/PD mapper/PDR " CR_LOG_CHAR8_STR_FMT "\n",
          Online ? "open" : "closed", Online ? "starting" : "reset");
}

STATIC CR_STATUS Enqueue(CR_QRTR *Router, UINT32 Type, UINT32 SourcePort,
                          UINT32 Node, UINT32 Port, CONST UINT8 *Data, UINTN Length)
{
  CR_QRTR_PACKET *Packet;
  UINTN Size = 32U + ((Length + 3U) & ~(UINTN)3U);
  if (Length == 0 || Size > CR_QRTR_MAX_PACKET) return CR_BAD_BUFFER_SIZE;
  if (Router->QueueCount == CR_QRTR_QUEUE_SIZE) return CR_NOT_READY;
  Packet = &Router->Queue[(Router->QueueHead + Router->QueueCount) % CR_QRTR_QUEUE_SIZE];
  cr_memset(Packet, 0, sizeof(*Packet)); Packet->Length = (UINT16)Size;
  Put32(Packet->Data, 1); Put32(Packet->Data + 4, Type);
  Put32(Packet->Data + 8, LOCAL_NODE); Put32(Packet->Data + 12, SourcePort);
  Put32(Packet->Data + 20, (UINT32)Length);
  Put32(Packet->Data + 24, Node); Put32(Packet->Data + 28, Port);
  cr_memcpy(Packet->Data + 32, Data, Length);
  ++Router->QueueCount;
  return CR_SUCCESS;
}

STATIC CR_STATUS Control(CR_QRTR *Router, UINT32 Type, UINT32 Node, UINT32 Port,
                          UINT32 SourcePort, UINT32 A, UINT32 B, UINT32 C, UINT32 D)
{
  UINT8 Data[20];
  Put32(Data, Type); Put32(Data + 4, A); Put32(Data + 8, B);
  Put32(Data + 12, C); Put32(Data + 16, D);
  return Enqueue(Router, Type, SourcePort, Node, Port, Data, sizeof(Data));
}

STATIC VOID PdState(CR_QRTR *Router, UINT32 State)
{
  BOOLEAN Ready = State == PD_UP;
  if (State != PD_UP && State != PD_DOWN && State != PD_EARLY_DOWN && State != PD_UNINIT)
    return;
  if (!Router->PdKnown || Router->PdReady != Ready) ++Router->PdGeneration;
  Router->PdKnown = TRUE; Router->PdReady = Ready;
  log_info("UCSICr: charger_pd state=0x%x ready=%u generation=%u\n",
          State, Ready, Router->PdGeneration);
}

STATIC VOID DiscardPdr(CR_QRTR *Router, BOOLEAN All)
{
  UINTN Read, Write = 0;
  /* A deleted endpoint must not receive a queued registration or state ACK
     when its QRTR port is later reused. Keep unrelated mapper/flow traffic. */
  for (Read = 0; Read < Router->QueueCount; ++Read) {
    CR_QRTR_PACKET *Packet = &Router->Queue[(Router->QueueHead + Read) % CR_QRTR_QUEUE_SIZE];
    if (Get32(Packet->Data + 4) == 1 && Get32(Packet->Data + 12) == PDR_PORT &&
        (All || (Get16(Packet->Data + 35) == 0x20 &&
                 Get16(Packet->Data + 33) == Router->PendingTransaction))) continue;
    if (Write != Read)
      cr_memcpy(&Router->Queue[(Router->QueueHead + Write) % CR_QRTR_QUEUE_SIZE], Packet, sizeof(*Packet));
    ++Write;
  }
  Router->QueueCount = Write;
}

STATIC VOID ForgetServer(CR_QRTR *Router)
{
  AudioForget(Router);
  DiscardPdr(Router, TRUE);
  PdState(Router, PD_DOWN);
  Router->ServerFound = Router->Registered = Router->Pending = FALSE;
  Router->ServerNode = Router->ServerPort = 0;
}

/* QMI strings in a top-level TLV have no length prefix. Nested domain
   entries use one byte before their name, as qmi_encdec.c specifies. */
typedef struct { CONST UINT8 *Data; UINT16 Length; } QRTR_TLV;
STATIC BOOLEAN Tlv(CONST UINT8 *Data, UINTN Length, UINT8 Type, QRTR_TLV *Value)
{
  UINTN Offset = 0;
  BOOLEAN Found = FALSE;
  cr_memset(Value, 0, sizeof(*Value));
  while (Offset < Length) {
    UINT16 Size;
    if (Length - Offset < 3U) return FALSE;
    Size = Get16(Data + Offset + 1U);
    if (Size > Length - Offset - 3U) return FALSE;
    if (Data[Offset] == Type) {
      if (Found) return FALSE;
      Value->Data = Data + Offset + 3U; Value->Length = Size; Found = TRUE;
    }
    Offset += 3U + Size;
  }
  return TRUE;
}
STATIC UINTN AddTlv(UINT8 *Data, UINTN Offset, UINT8 Type,
                    CONST VOID *Value, UINT16 Length)
{
  Data[Offset] = Type; Put16(Data + Offset + 1, Length);
  cr_memcpy(Data + Offset + 3, Value, Length);
  return Offset + 3U + Length;
}
STATIC VOID QmiHeader(UINT8 *Data, UINT8 Type, UINT16 Transaction,
                      UINT16 Message, UINTN Length)
{
  Data[0] = Type; Put16(Data + 1, Transaction); Put16(Data + 3, Message);
  Put16(Data + 5, (UINT16)(Length - 7U));
}
STATIC UINT16 NextTransaction(CR_QRTR *Router)
{ if (++Router->Transaction == 0) ++Router->Transaction; return Router->Transaction; }

STATIC CR_STATUS Register(CR_QRTR *Router, UINT64 NowUs)
{
  UINT8 Data[80], Enable = 1;
  UINTN Length = 7;
  UINT16 Transaction = NextTransaction(Router);
  CR_STATUS Status;
  Length = AddTlv(Data, Length, 1, &Enable, 1);
  Length = AddTlv(Data, Length, 2, mChargerPath, sizeof(mChargerPath) - 1U);
  QmiHeader(Data, 0, Transaction, 0x20, Length);
  Status = Enqueue(Router, 1, PDR_PORT, Router->ServerNode, Router->ServerPort, Data, Length);
  if (!CR_ERROR(Status)) {
    Router->Pending = TRUE; Router->PendingTransaction = Transaction;
    Router->DeadlineUs = NowUs + 5000000ULL;
    log_info("UCSICr: PDR listen " CR_LOG_CHAR8_STR_FMT " node=%u port=%u transaction=%u\n",
            mChargerPath, Router->ServerNode, Router->ServerPort, Transaction);
  }
  return Status;
}

STATIC CR_STATUS PdrMessage(CR_QRTR *Router, UINT32 Node, UINT32 Port,
                             CONST UINT8 *Data, UINTN Length)
{
  QRTR_TLV Result, State, Path, Id;
  UINT8 Reply[80];
  UINTN ReplyLength;
  UINT16 Message = Get16(Data + 3), Transaction = Get16(Data + 1);
  if (!Router->ServerFound || Node != Router->ServerNode || Port != Router->ServerPort)
    return CR_SUCCESS;
  if (Data[0] == 2 && Message == 0x20 && Router->Pending &&
      Transaction == Router->PendingTransaction) {
    if (!Tlv(Data + 7, Length - 7, 2, &Result) || Result.Length != 4 ||
        !Tlv(Data + 7, Length - 7, 0x10, &State) || (State.Data != NULL && State.Length != 4))
      return CR_COMPROMISED_DATA;
    Router->Pending = FALSE;
    if (Get16(Result.Data) != 0) {
      Router->LastError = CR_DEVICE_ERROR;
      Router->RetryUs = Router->DeadlineUs;
      log_warn("UCSICr: PDR registration rejected result=%u error=%u\n",
              Get16(Result.Data), Get16(Result.Data + 2));
    } else {
      Router->Registered = TRUE; Router->LastError = CR_SUCCESS;
      if (State.Data != NULL) PdState(Router, Get32(State.Data));
    }
    return CR_SUCCESS;
  }
  if (Data[0] != 4 || Message != 0x22) return CR_SUCCESS;
  if (!Tlv(Data + 7, Length - 7, 1, &State) || State.Length != 4 ||
      !Tlv(Data + 7, Length - 7, 2, &Path) ||
      !Tlv(Data + 7, Length - 7, 3, &Id) || Id.Length != 2)
    return CR_COMPROMISED_DATA;
  if (!TextIs(Path.Data, Path.Length, mChargerPath)) return CR_SUCCESS;
  /* Queue the QMI ACK independently of UCSI; a PD restart must still be
     acknowledged when connector negotiation has previously faulted. */
  ReplyLength = AddTlv(Reply, 7, 1, mChargerPath, sizeof(mChargerPath) - 1U);
  ReplyLength = AddTlv(Reply, ReplyLength, 2, Id.Data, 2);
  QmiHeader(Reply, 0, NextTransaction(Router), 0x23, ReplyLength);
  CR_STATUS Status = Enqueue(Router, 1, PDR_PORT, Node, Port, Reply, ReplyLength);
  if (CR_ERROR(Status)) return Status;
  PdState(Router, Get32(State.Data));
  return CR_SUCCESS;
}

STATIC VOID AudioDiscard(CR_QRTR *Router, BOOLEAN All)
{
  UINTN Count = 0;
  for (UINTN Read = 0; Read < Router->QueueCount; ++Read) {
    CR_QRTR_PACKET *Packet = &Router->Queue[(Router->QueueHead + Read) % CR_QRTR_QUEUE_SIZE];
    if (Get32(Packet->Data + 4) == 1 && Get32(Packet->Data + 12) == AUDIO_PDR_PORT &&
        (All || (Get16(Packet->Data + 35) == 0x20 &&
                 Get16(Packet->Data + 33) == Router->Audio.PendingTransaction))) continue;
    if (Read != Count)
      cr_memcpy(&Router->Queue[(Router->QueueHead + Count) % CR_QRTR_QUEUE_SIZE], Packet, sizeof(*Packet));
    ++Count;
  }
  Router->QueueCount = Count;
}

STATIC VOID AudioFault(CR_QRTR *Router, CR_STATUS Reason)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  if (!A->Enabled || A->Faulted) return;
  A->Faulted = TRUE; A->Ready = FALSE; A->Known = FALSE;
  A->Registered = A->Pending = FALSE; A->LastError = Reason;
  if (A->Generation != 0xFFFFFFFFU) ++A->Generation;
  AudioDiscard(Router, TRUE);
}

STATIC BOOLEAN AudioAdvance(CR_QRTR *Router, BOOLEAN Server)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  if (A->Faulted) return FALSE;
  if (A->Generation == 0xFFFFFFFFU || (Server && A->ServerGeneration == ~(UINT64)0)) {
    AudioFault(Router, CR_OUT_OF_RESOURCES); return FALSE;
  }
  ++A->Generation;
  if (Server) ++A->ServerGeneration;
  return TRUE;
}

STATIC VOID AudioForget(CR_QRTR *Router)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  if (!A->Enabled) return;
  (VOID)AudioAdvance(Router, TRUE);
  AudioDiscard(Router, TRUE);
  A->ServerFound = A->Registered = A->Pending = A->Indicated = FALSE;
  A->Known = A->Ready = FALSE; A->ServerNode = A->ServerPort = A->RawState = 0;
  A->RetryUs = 0;
  if (!A->Faulted) A->LastError = CR_NOT_READY;
}

STATIC VOID AudioServer(CR_QRTR *Router, UINT32 Node, UINT32 Port)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  if (!A->Enabled || A->Faulted ||
      (A->ServerFound && A->ServerNode == Node && A->ServerPort == Port)) return;
  if (A->ServerFound) AudioForget(Router);
  if (!AudioAdvance(Router, TRUE)) return;
  A->ServerFound = TRUE; A->ServerNode = Node; A->ServerPort = Port;
  A->Registered = A->Pending = A->Known = A->Ready = A->Indicated = FALSE;
  A->RawState = 0; A->RetryUs = 0; A->LastError = CR_NOT_READY;
}

STATIC VOID AudioState(CR_QRTR *Router, UINT32 State)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  if (A->Faulted) return;
  BOOLEAN Known = State == PD_UP || State == PD_DOWN || State == PD_EARLY_DOWN || State == PD_UNINIT;
  BOOLEAN Ready = Known && State == PD_UP && A->Registered && !A->Faulted;
  if ((!A->Known || !Known || A->RawState != State || A->Ready != Ready) &&
      !AudioAdvance(Router, FALSE)) return;
  A->Known = Known; A->Ready = Ready; A->RawState = State;
  A->LastError = Known ? (Ready ? CR_SUCCESS : CR_NOT_READY) : CR_COMPROMISED_DATA;
}

STATIC BOOLEAN AudioTransaction(CR_QRTR *Router, UINT16 *Transaction, BOOLEAN Ack)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  /* A readiness/consumer failure must not strand the remote PDR notifier.
   * Valid ACKs may progress while admission stays closed, provided the local
   * transaction namespace is still usable. Exhaustion never wraps. */
  if (A->Faulted && !Ack) return FALSE;
  if (A->NextTransaction == 0 || A->NextTransaction > 0xFFFFU) {
    AudioFault(Router, CR_OUT_OF_RESOURCES); return FALSE;
  }
  *Transaction = (UINT16)A->NextTransaction++;
  return TRUE;
}

STATIC VOID AudioTime(CR_QRTR *Router, UINT64 NowUs)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  if (!A->Enabled || A->Faulted) return;
  if (A->TimeKnown && NowUs < A->NowUs) { AudioFault(Router, CR_INVALID_PARAMETER); return; }
  A->TimeKnown = TRUE; A->NowUs = NowUs;
  if (A->Pending && NowUs >= A->DeadlineUs) {
    AudioDiscard(Router, FALSE); A->Pending = FALSE;
    A->LastError = CR_TIMEOUT;
    if (NowUs > ~(UINT64)0 - 5000000ULL) { AudioFault(Router, CR_OUT_OF_RESOURCES); return; }
    A->RetryUs = NowUs + 5000000ULL;
  }
}

STATIC VOID AudioRegister(CR_QRTR *Router, UINT64 NowUs)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  UINT8 Data[80], Enable = 1;
  UINT16 Transaction;
  if (!A->Enabled || A->Faulted || !A->ServerFound || A->Registered || A->Pending || NowUs < A->RetryUs) return;
  if (Router->QueueCount == CR_QRTR_QUEUE_SIZE) return;
  if (NowUs > ~(UINT64)0 - 5000000ULL) { AudioFault(Router, CR_OUT_OF_RESOURCES); return; }
  if (!AudioTransaction(Router, &Transaction, FALSE)) return;
  UINTN Length = AddTlv(Data, 7, 1, &Enable, 1);
  Length = AddTlv(Data, Length, 2, mAudioPath, sizeof(mAudioPath) - 1U);
  QmiHeader(Data, 0, Transaction, 0x20, Length);
  CR_STATUS Status = Enqueue(Router, 1, AUDIO_PDR_PORT, A->ServerNode, A->ServerPort, Data, Length);
  if (Status != CR_SUCCESS) { AudioFault(Router, Status); return; }
  A->Pending = TRUE; A->PendingTransaction = Transaction; A->DeadlineUs = NowUs + 5000000ULL;
  A->LastError = CR_NOT_READY;
}

STATIC CR_STATUS AudioMessage(CR_QRTR *Router, UINT32 Node, UINT32 Port,
                              CONST UINT8 *Data, UINTN Length)
{
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  QRTR_TLV Result, State, Path, Id;
  UINT16 Message = Get16(Data + 3), Transaction = Get16(Data + 1);
  if (!A->Enabled || !A->ServerFound || Node != A->ServerNode || Port != A->ServerPort)
    return CR_SUCCESS;
  if (!A->Faulted && Data[0] == 2 && Message == 0x20 && A->Pending && Transaction == A->PendingTransaction) {
    if (!Tlv(Data + 7, Length - 7, 2, &Result) || Result.Length != 4 ||
        !Tlv(Data + 7, Length - 7, 0x10, &State) || (State.Data && State.Length != 4)) return CR_COMPROMISED_DATA;
    A->Pending = FALSE;
    if (Get16(Result.Data) != 0) {
      A->LastError = CR_DEVICE_ERROR; A->RetryUs = A->DeadlineUs;
      return CR_SUCCESS;
    }
    A->Registered = TRUE;
    /* An indication may already have superseded the registration snapshot. */
    if (A->Indicated) AudioState(Router, A->RawState);
    else if (State.Data) AudioState(Router, Get32(State.Data));
    else A->LastError = CR_NOT_READY;
    return CR_SUCCESS;
  }
  if (Data[0] != 4 || Message != 0x22) return CR_SUCCESS;
  if (!Tlv(Data + 7, Length - 7, 1, &State) || State.Length != 4 ||
      !Tlv(Data + 7, Length - 7, 2, &Path) ||
      !Tlv(Data + 7, Length - 7, 3, &Id) || Id.Length != 2) return CR_COMPROMISED_DATA;
  if (!TextIs(Path.Data, Path.Length, mAudioPath)) return CR_SUCCESS;
  A->Indicated = TRUE;
  /* Invalidate before ACK creation/backpressure, never after returning to the
   * GLINK poller (which can transmit its next audio fragment immediately). */
  AudioState(Router, Get32(State.Data));
  UINT16 AckTransaction;
  if (!AudioTransaction(Router, &AckTransaction, TRUE)) return CR_SUCCESS;
  UINT8 Reply[80];
  UINTN ReplyLength = AddTlv(Reply, 7, 1, mAudioPath, sizeof(mAudioPath) - 1U);
  ReplyLength = AddTlv(Reply, ReplyLength, 2, Id.Data, 2);
  QmiHeader(Reply, 0, AckTransaction, 0x23, ReplyLength);
  return Enqueue(Router, 1, AUDIO_PDR_PORT, Node, Port, Reply, ReplyLength);
}

CR_STATUS CrQrtrEnableAudioPd(CR_QRTR *Router)
{
  if (!Router || !Router->Send) return CR_INVALID_PARAMETER;
  CR_QRTR_AUDIO_PD *A = &Router->Audio;
  if (A->Enabled) return A->Faulted ? A->LastError : CR_SUCCESS;
  A->Enabled = TRUE; A->Generation = 1; A->NextTransaction = 1; A->LastError = CR_NOT_READY;
  if (Router->ServerFound) AudioServer(Router, Router->ServerNode, Router->ServerPort);
  return CR_SUCCESS;
}

CR_STATUS CrQrtrGetAudioPd(CONST CR_QRTR *Router, CR_QRTR_AUDIO_PD_INFORMATION *Info)
{
  if (!Router || !Info) return CR_INVALID_PARAMETER;
  UINTN R = (UINTN)Router, I = (UINTN)Info;
  if ((I >= R && I - R < sizeof(*Router)) || (R > I && R - I < sizeof(*Info))) return CR_INVALID_PARAMETER;
  CONST CR_QRTR_AUDIO_PD *A = &Router->Audio;
  *Info = (CR_QRTR_AUDIO_PD_INFORMATION){A->Enabled, A->ServerFound, A->Registered,
    A->Known, A->Ready, A->Faulted, A->ServerNode, A->ServerPort, A->Generation,
    A->RawState, A->ServerGeneration, A->Enabled ? A->LastError : CR_NOT_READY};
  return CR_SUCCESS;
}

STATIC CR_STATUS MapperMessage(CR_QRTR *Router, UINT32 Node, UINT32 Port,
                                CONST UINT8 *Data, UINTN Length)
{
  /* Linux sm8450 selects sm8350_domains. No claim that these remote
     domains are running: this service describes their location only. */
  STATIC CONST struct { CONST CHAR8 *Name; UINT32 Instance; } Domains[] = {
    { "msm/adsp/audio_pd", 74 }, { "msm/adsp/root_pd", 74 },
    { "msm/adsp/charger_pd", 74 }, { "msm/cdsp/root_pd", 76 },
    { "msm/modem/root_pd", 180 }, { "msm/slpi/root_pd", 90 },
    { "msm/slpi/sensor_pd", 90 }
  };
  QRTR_TLV Service, Offset;
  UINT8 Reply[512], List[400], Result[4] = {0}, Number[2];
  UINTN ReplyLength = 7, ListLength = 1, Index, Total = 0;
  UINT32 Start = 0;
  UINT16 Message = Get16(Data + 3);
  if (Data[0] != 0) return CR_SUCCESS;
  if (Message == 0x21) {
    if (!Tlv(Data + 7, Length - 7, 1, &Service) || Service.Data == NULL ||
        Service.Length == 0 || Service.Length > 64 ||
        !Tlv(Data + 7, Length - 7, 0x10, &Offset) || (Offset.Data != NULL && Offset.Length != 4))
      return CR_COMPROMISED_DATA;
    if (Offset.Data != NULL) Start = Get32(Offset.Data);
    List[0] = 0;
    for (Index = 0; Index < sizeof(Domains) / sizeof(Domains[0]); ++Index) {
      BOOLEAN Match = TextIs(Service.Data, Service.Length, "tms/servreg") ||
        (Index == 0 && TextIs(Service.Data, Service.Length, "avs/audio")) ||
        (Index == 1 && TextIs(Service.Data, Service.Length, "tms/pdr_enabled")) ||
        (Index == 4 && TextIs(Service.Data, Service.Length, "gps/gps_service"));
      if (!Match) continue;
      if (Total++ < Start) continue;
      UINTN Size = TextLength(Domains[Index].Name);
      List[ListLength++] = (UINT8)Size;
      cr_memcpy(List + ListLength, Domains[Index].Name, Size); ListLength += Size;
      Put32(List + ListLength, Domains[Index].Instance); ListLength += 4;
      List[ListLength++] = 0; Put32(List + ListLength, 0); ListLength += 4;
      ++List[0];
    }
    ReplyLength = AddTlv(Reply, ReplyLength, 2, Result, 4);
    Put16(Number, (UINT16)Total); ReplyLength = AddTlv(Reply, ReplyLength, 0x10, Number, 2);
    Put16(Number, 1); ReplyLength = AddTlv(Reply, ReplyLength, 0x11, Number, 2);
    if (Total != 0) ReplyLength = AddTlv(Reply, ReplyLength, 0x12, List, (UINT16)ListLength);
    ++Router->DomainRequests;
    log_info("UCSICr: PD mapper query bytes=%u domains=%u offset=%u\n",
            Service.Length, (UINT32)Total, Start);
  } else if (Message == 0x24) {
    /* Acknowledge the locator's failure-reason report as Linux does. */
    if (!Tlv(Data + 7, Length - 7, 1, &Service) || Service.Data == NULL ||
        !Tlv(Data + 7, Length - 7, 2, &Offset) || Offset.Data == NULL)
      return CR_COMPROMISED_DATA;
    ReplyLength = AddTlv(Reply, ReplyLength, 2, Result, 4);
  } else {
    Put16(Result, 1); Put16(Result + 2, 0x47); /* QMI_ERR_NOT_SUPPORTED */
    ReplyLength = AddTlv(Reply, ReplyLength, 2, Result, 4);
  }
  QmiHeader(Reply, 2, Get16(Data + 1), Message, ReplyLength);
  return Enqueue(Router, 1, MAPPER_PORT, Node, Port, Reply, ReplyLength);
}

STATIC CR_STATUS Receive(CR_QRTR *Router, CONST UINT8 *Data, UINTN Length)
{
  UINT32 Type, Node, Port, Destination, Size, Confirm;
  UINTN Header;
  if (Length < 16) return CR_COMPROMISED_DATA;
  if (Data[0] == 1) {
    if (Length < 32 || Get32(Data) != 1) return CR_COMPROMISED_DATA;
    Header = 32; Type = Get32(Data + 4); Node = Get32(Data + 8);
    Port = Get32(Data + 12); Confirm = Get32(Data + 16);
    Size = Get32(Data + 20); Destination = Get32(Data + 28);
    if (Get32(Data + 24) != LOCAL_NODE && Get32(Data + 24) != 0xFFFFFFFFU)
      return CR_SUCCESS;
  } else if (Data[0] == 3) {
    Header = 16U + Data[3]; Type = Data[1]; Confirm = Data[2] & 1U;
    Size = Get32(Data + 4); Node = Get16(Data + 8); Port = Get16(Data + 10);
    Destination = Get16(Data + 14);
    if (Port == 0xFFFEU) Port = CONTROL_PORT;
    if (Destination == 0xFFFEU) Destination = CONTROL_PORT;
    if (Get16(Data + 12) != LOCAL_NODE && Get16(Data + 12) != 0xFFFFU)
      return CR_SUCCESS;
  } else return CR_COMPROMISED_DATA;
  if (Destination == 0xFFFFU) Destination = CONTROL_PORT;
  if (Size == 0 || Size > CR_QRTR_MAX_PACKET || Header > Length ||
      ((UINTN)Size + 3U) / 4U * 4U != Length - Header)
    return CR_COMPROMISED_DATA;
  Data += Header;
  if (Type == 1) {
    CR_STATUS Status;
    /* Reserve a slot for both the response and flow-control acknowledgement. */
    if (Router->QueueCount > CR_QRTR_QUEUE_SIZE - 2U) {
      if (Destination == AUDIO_PDR_PORT && Router->Audio.ServerFound &&
          Node == Router->Audio.ServerNode && Port == Router->Audio.ServerPort)
        AudioFault(Router, CR_OUT_OF_RESOURCES);
      return CR_NOT_READY;
    }
    if ((Destination == MAPPER_PORT || Destination == PDR_PORT || Destination == AUDIO_PDR_PORT) &&
        (Size < 7 || Get16(Data + 5) != Size - 7U)) Status = CR_COMPROMISED_DATA;
    else Status = Destination == MAPPER_PORT ? MapperMessage(Router, Node, Port, Data, Size) :
      Destination == PDR_PORT ? PdrMessage(Router, Node, Port, Data, Size) :
      Destination == AUDIO_PDR_PORT ? AudioMessage(Router, Node, Port, Data, Size) : CR_SUCCESS;
    if (CR_ERROR(Status) && Destination == AUDIO_PDR_PORT && Router->Audio.ServerFound &&
        Node == Router->Audio.ServerNode && Port == Router->Audio.ServerPort)
      AudioFault(Router, Status);
    if (Status == CR_NOT_READY) return Status;
    /* QRTR flow control acknowledges transport receipt even when the QMI
       decoder rejects the message, as Linux's qrtr_recvmsg() does. */
    if (Confirm != 0) {
      CR_STATUS Ack = Control(Router, 7, Node, Port, Destination, LOCAL_NODE, Destination, 0, 0);
      if (CR_ERROR(Ack)) return Ack;
    }
    return Status;
  }
  if (Type == 7 && Size >= 20 && Get32(Data) == 7) return CR_SUCCESS;
  if (Destination != CONTROL_PORT || Size < 20 || Get32(Data) != Type)
    return CR_COMPROMISED_DATA;
  if (Type == 2) {
    /* A new remote HELLO resynchronizes our service advertisement. */
    Router->Advertised = FALSE;
    AudioForget(Router);
  } else if (Type == 4 && Get32(Data + 4) == 0x42 && Get32(Data + 8) == PDR_INSTANCE) {
    UINT32 ServerNode = Get32(Data + 12), ServerPort = Get32(Data + 16);
    /* An IPCRTR endpoint may bridge a notifier hosted on another node. */
    if (ServerNode == 0 || ServerNode == LOCAL_NODE || ServerNode == 0xFFFFFFFFU ||
        ServerPort == 0 || ServerPort == CONTROL_PORT || ServerPort == 0xFFFFU)
      return CR_COMPROMISED_DATA;
    if (!Router->ServerFound || ServerNode != Router->ServerNode || ServerPort != Router->ServerPort) {
      if (Router->ServerFound) ForgetServer(Router);
      Router->ServerFound = TRUE; Router->ServerNode = ServerNode; Router->ServerPort = ServerPort;
      Router->Registered = Router->Pending = FALSE; Router->RetryUs = 0;
      Router->PdKnown = TRUE; Router->PdReady = FALSE; ++Router->PdGeneration;
    }
    AudioServer(Router, ServerNode, ServerPort);
  } else if (Router->ServerFound && ((Type == 5 && Get32(Data + 4) == 0x42 && Get32(Data + 8) == PDR_INSTANCE &&
              Get32(Data + 12) == Router->ServerNode && Get32(Data + 16) == Router->ServerPort) ||
             (Type == 3 && Node == Router->ServerNode))) ForgetServer(Router);
  return CR_SUCCESS;
}

CR_STATUS CrQrtrReceive(CR_QRTR *Router, CONST UINT8 *Data, UINTN Length)
{
  CR_STATUS Status;
  if (Router == NULL || Data == NULL) return CR_INVALID_PARAMETER;
  if (!Router->Online) return CR_NOT_READY;
  Status = Receive(Router, Data, Length);
  if (Status == CR_NOT_READY) return Status;
  ++Router->Received;
  if (CR_ERROR(Status)) {
    ++Router->Malformed; Router->LastError = Status;
    if (Router->Malformed <= 4U)
      log_warn("UCSICr: QRTR packet rejected bytes=%u: " CR_LOG_STATUS_FMT "\n", (UINT32)Length, Status);
  }
  return CR_SUCCESS; /* malformed QRTR traffic must not stop the PMIC channel */
}

CR_STATUS CrQrtrTick(CR_QRTR *Router, UINT64 NowUs)
{
  CR_STATUS Status;
  UINTN Budget;
  if (Router == NULL || Router->Send == NULL) return CR_INVALID_PARAMETER;
  if (!Router->Online) return CR_NOT_READY;
  AudioTime(Router, NowUs);
  if (Router->Pending && NowUs >= Router->DeadlineUs) {
    DiscardPdr(Router, FALSE);
    Router->Pending = FALSE; Router->RetryUs = NowUs + 5000000ULL;
    Router->LastError = CR_TIMEOUT;
    log_warn("UCSICr: PDR listen timed out; retrying after 5 seconds\n");
  }
  /* Drain before generating control packets: a remote HELLO can request
     readvertisement while the bounded queue is already full. */
  for (Budget = 0; Budget < 4 && Router->QueueCount != 0; ++Budget) {
    CR_QRTR_PACKET *Packet = &Router->Queue[Router->QueueHead];
    Status = Router->Send(Router->Context, Packet->Data, Packet->Length);
    if (CR_ERROR(Status)) return Status;
    Router->QueueHead = (Router->QueueHead + 1U) % CR_QRTR_QUEUE_SIZE;
    --Router->QueueCount;
  }
  if (!Router->HelloSent) {
    Status = Control(Router, 2, 0xFFFFFFFFU, CONTROL_PORT, CONTROL_PORT, 0, 0, 0, 0);
    if (CR_ERROR(Status)) return Status;
    Router->HelloSent = TRUE;
  }
  if (!Router->Advertised) {
    /* qmi_add_server(0x40, version=0x101, instance=0) packs to 0x101. */
    Status = Control(Router, 4, 0xFFFFFFFFU, CONTROL_PORT, CONTROL_PORT,
                     0x40, 0x101, LOCAL_NODE, MAPPER_PORT);
    if (CR_ERROR(Status)) return Status;
    Router->Advertised = TRUE;
  }
  if (Router->ServerFound && !Router->Registered && !Router->Pending && NowUs >= Router->RetryUs) {
    Status = Register(Router, NowUs);
    if (CR_ERROR(Status)) return Status;
  }
  AudioRegister(Router, NowUs);
  return CR_SUCCESS;
}
