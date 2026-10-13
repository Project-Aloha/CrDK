/**
  UCSICr GLink v1 over SMEM, adapted from Linux qcom_glink_{native,smem}.c.
  Copyright (c) 2016, Linaro Ltd.
  SPDX-License-Identifier: GPL-2.0-only

  All operations are nonblocking. A bounded TX queue keeps protocol replies
  until the remote publishes FIFO space. PMIC and IPCRTR have independent
  intent pools and receive callbacks.
**/
#include <ucsi/Glink.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>
#include <oskal/cr_debug.h>
#ifdef _KERNEL_MODE
#if !defined(CR_OSK_LOG_WITHOUT_WPP)
#include "Glink.tmh"
#endif
#endif

#define GLINK_HOST_ADSP 2U
#define GLINK_DESCRIPTOR 478U
#define GLINK_TX_FIFO 479U
#define GLINK_RX_FIFO 480U
#define GLINK_FIFO_SIZE 16384U
#define GLINK_VERSION 0U
#define GLINK_VERSION_ACK 1U
#define GLINK_OPEN 2U
#define GLINK_CLOSE 3U
#define GLINK_OPEN_ACK 4U
#define GLINK_INTENT 5U
#define GLINK_RX_DONE 6U
#define GLINK_INTENT_REQ 7U
#define GLINK_INTENT_REQ_ACK 8U
#define GLINK_DATA 9U
#define GLINK_CLOSE_ACK 11U
#define GLINK_DATA_CONT 12U
#define GLINK_READ_NOTIF 13U
#define GLINK_RX_DONE_REUSE 14U
#define GLINK_SIGNALS 15U

typedef struct { UINT16 Command, Param1; UINT32 Param2; } GLINK_HEADER;

STATIC UINTN Align8(UINTN Value) { return (Value + 7U) & ~(UINTN)7U; }

STATIC CR_STATUS ServiceStage(CR_GLINK *Link);
STATIC VOID ServiceDown(CR_GLINK *Link, CR_GLINK_SERVICE *Service, CR_STATUS Reason);
STATIC VOID ServiceClose(CR_GLINK *Link, CR_GLINK_CHANNEL *Channel, CR_STATUS Reason);
STATIC VOID ServiceFinishWire(CR_GLINK_CHANNEL *Channel);
STATIC CR_STATUS Queue(CR_GLINK *Link, UINT16 Command, UINT16 Param1,
                        UINT32 Param2, CONST VOID *Payload, UINTN Length);

STATIC BOOLEAN ServiceSpan(CONST VOID *Pointer, UINTN Bytes)
{
  return Pointer != NULL && (UINTN)Pointer <= (UINTN)-1 - Bytes;
}

STATIC BOOLEAN ServiceDisjoint(CONST VOID *A, UINTN An, CONST VOID *B, UINTN Bn)
{
  return ServiceSpan(A, An) && ServiceSpan(B, Bn) &&
    !((UINTN)A < (UINTN)B + Bn && (UINTN)B < (UINTN)A + An);
}

STATIC UINTN ServiceNameLength(CONST CHAR8 Name[32])
{
  for (UINTN Index = 0; Index < 32; ++Index)
    if (Name[Index] == 0) return Index == 0 ? 0 : Index + 1;
  return 0;
}

STATIC CR_GLINK_CHANNEL *ServiceChannel(CR_GLINK *Link, CR_GLINK_SERVICE *Service)
{
  if (Link == NULL || Service == NULL) return NULL;
  for (UINTN Index = 0; Index < CR_GLINK_CHANNELS; ++Index)
    if (Link->Channels[Index].Service == Service) return &Link->Channels[Index];
  return NULL;
}

/* Also used before newly discovered SMEM ranges are installed in Link. */
STATIC BOOLEAN ServiceStorageOutside(CR_GLINK *Link, CONST VOID *Pointer, UINTN Bytes)
{
  if (!ServiceDisjoint(Link, sizeof(*Link), Pointer, Bytes)) return FALSE;
  for (UINTN Index = 0; Index < CR_GLINK_CHANNELS; ++Index) {
    CR_GLINK_SERVICE *S = Link->Channels[Index].Service;
    if (S && (!ServiceDisjoint(S, sizeof(*S), Pointer, Bytes) ||
              !ServiceDisjoint(S->Config.RxStorage, S->Config.RxBytes, Pointer, Bytes) ||
              !ServiceDisjoint(S->Config.TxStorage, S->Config.TxBytes, Pointer, Bytes)))
      return FALSE;
  }
  return TRUE;
}

STATIC BOOLEAN ServiceOutside(CR_GLINK *Link, CONST VOID *Pointer, UINTN Bytes)
{
  return ServiceStorageOutside(Link, Pointer, Bytes) &&
    (!Link->Descriptor || ServiceDisjoint((CONST VOID *)Link->Descriptor, 32, Pointer, Bytes)) &&
    (!Link->Tx || ServiceDisjoint(Link->Tx, Link->TxSize, Pointer, Bytes)) &&
    (!Link->Rx || ServiceDisjoint(Link->Rx, Link->RxSize, Pointer, Bytes));
}

STATIC BOOLEAN ServiceAdvance(CR_GLINK *Link, CR_GLINK_SERVICE *Service)
{
  if (Link->ServiceGeneration == ~(UINT64)0) {
    Service->Faulted = TRUE;
    Service->LastError = CR_OUT_OF_RESOURCES;
    return FALSE;
  }
  Service->Generation = ++Link->ServiceGeneration;
  return TRUE;
}

STATIC VOID ServiceNotify(CR_GLINK *Link, CR_GLINK_SERVICE *Service)
{
  if (Service->Config.State == NULL) return;
  Link->ServiceCallback = TRUE;
  Service->Config.State(Service->Config.Context, Service->Online,
                       Service->Generation, Service->LastError);
  Link->ServiceCallback = FALSE;
}

STATIC VOID ServiceDown(CR_GLINK *Link, CR_GLINK_SERVICE *Service, CR_STATUS Reason)
{
  Service->Online = FALSE;
  Service->LastError = Reason;
  (VOID)ServiceAdvance(Link, Service);
  Service->TxLength = Service->TxOffset = 0;
  cr_memset(Service->Offset, 0, sizeof(Service->Offset));
  cr_memset(Service->Remaining, 0, sizeof(Service->Remaining));
  if (!Service->Detaching) ServiceNotify(Link, Service);
}

VOID CrGlinkReset(CR_GLINK *Link)
{
  if (Link == NULL || Link->ServiceCallback) return;
  CR_GLINK_SERVICE *Services[CR_GLINK_CHANNELS];
  for (UINTN Index = 0; Index < CR_GLINK_CHANNELS; ++Index) {
    Services[Index] = Link->Channels[Index].Service;
    if (Services[Index]) {
      Services[Index]->Faulted = Services[Index]->PeerClosed = FALSE;
      ServiceDown(Link, Services[Index], CR_NOT_READY);
    }
  }
  UINT64 ServiceGeneration = Link->ServiceGeneration;
  CR_GLINK_RECEIVE Receive = Link->Receive;
  CR_GLINK_QRTR_RECEIVE QrtrReceive = Link->QrtrReceive;
  CR_GLINK_STATE QrtrState = Link->QrtrState;
  CR_GLINK_QRTR_RECEIVE LogsReceive = Link->LogsReceive;
  CR_GLINK_STATE LogsState = Link->LogsState;
  CR_GLINK_KICK Kick = Link->Kick;
  CR_GLINK_COMMIT Commit = Link->Commit;
  VOID *CommitContext = Link->CommitContext;
  VOID *Context = Link->Context;
  if (QrtrState != NULL) QrtrState(Context, FALSE);
  if (LogsState != NULL) LogsState(Context, FALSE);
  cr_memset(Link, 0, sizeof(*Link));
  Link->Receive = Receive;
  Link->QrtrReceive = QrtrReceive;
  Link->QrtrState = QrtrState;
  Link->LogsReceive = LogsReceive;
  Link->LogsState = LogsState;
  Link->Kick = Kick;
  Link->Commit = Commit;
  Link->CommitContext = CommitContext;
  Link->Context = Context;
  Link->ServiceGeneration = ServiceGeneration;
  for (UINTN Index = 0; Index < CR_GLINK_CHANNELS; ++Index) {
    Link->Channels[Index].Service = Services[Index];
    if (Services[Index])
      cr_memcpy(Link->Channels[Index].Name, Services[Index]->Config.Name, 32);
  }
}

STATIC CR_STATUS Queue(CR_GLINK *Link, UINT16 Command, UINT16 Param1,
                        UINT32 Param2, CONST VOID *Payload, UINTN Length)
{
  GLINK_HEADER Header = { Command, Param1, Param2 };
  CR_GLINK_QUEUED *Message;
  UINTN Size = Align8(sizeof(Header) + Length);
  if (Length > sizeof(Link->Queue[0].Data) - sizeof(Header))
    return CR_BAD_BUFFER_SIZE;
  if (Link->QueueCount == CR_GLINK_QUEUE_SIZE) return CR_NOT_READY;
  Message = &Link->Queue[(Link->QueueHead + Link->QueueCount) % CR_GLINK_QUEUE_SIZE];
  cr_memset(Message, 0, sizeof(*Message));
  Message->Size = (UINT16)Size;
  cr_memcpy(Message->Data, &Header, sizeof(Header));
  if (Length != 0) cr_memcpy(Message->Data + sizeof(Header), Payload, Length);
  ++Link->QueueCount;
  return CR_SUCCESS;
}

STATIC VOID RingCopy(UINT8 *Fifo, UINT32 Size, UINT32 Offset,
                     VOID *Buffer, UINTN Length, BOOLEAN Write)
{
  UINTN First = Length < Size - Offset ? Length : Size - Offset;
  if (Write) {
    cr_memcpy(Fifo + Offset, Buffer, First);
    if (Length > First) cr_memcpy(Fifo, (UINT8 *)Buffer + First, Length - First);
  } else {
    cr_memcpy(Buffer, Fifo + Offset, First);
    if (Length > First) cr_memcpy((UINT8 *)Buffer + First, Fifo, Length - First);
  }
}

STATIC VOID DiscardChannelTx(CR_GLINK *Link, UINT16 Local)
{
  UINTN Read, Count = 0;
  for (Read = 0; Read < Link->QueueCount; ++Read) {
    UINTN Slot = (Link->QueueHead + Read) % CR_GLINK_QUEUE_SIZE;
    GLINK_HEADER Header;
    cr_memcpy(&Header, Link->Queue[Slot].Data, sizeof(Header));
    if (Header.Param1 == Local &&
        (Header.Command == GLINK_DATA || Header.Command == GLINK_DATA_CONT ||
         Header.Command == GLINK_INTENT || Header.Command == GLINK_INTENT_REQ ||
         Header.Command == GLINK_INTENT_REQ_ACK || Header.Command == GLINK_RX_DONE ||
         Header.Command == GLINK_RX_DONE_REUSE || Header.Command == GLINK_OPEN)) continue;
    if (Read != Count)
      Link->Queue[(Link->QueueHead + Count) % CR_GLINK_QUEUE_SIZE] = Link->Queue[Slot];
    ++Count;
  }
  Link->QueueCount = Count;
}

/* Keep OPEN/control handshakes ordered even when closing before the queued
   OPEN reaches the peer. Remove only service data/intent traffic. */
STATIC VOID ServiceDiscardTx(CR_GLINK *Link, UINT16 Local)
{
  UINTN Count = 0;
  for (UINTN Read = 0; Read < Link->QueueCount; ++Read) {
    UINTN Slot = (Link->QueueHead + Read) % CR_GLINK_QUEUE_SIZE;
    GLINK_HEADER Header;
    cr_memcpy(&Header, Link->Queue[Slot].Data, sizeof(Header));
    if (Header.Param1 == Local &&
        (Header.Command == GLINK_DATA || Header.Command == GLINK_DATA_CONT ||
         Header.Command == GLINK_INTENT || Header.Command == GLINK_INTENT_REQ ||
         Header.Command == GLINK_INTENT_REQ_ACK || Header.Command == GLINK_RX_DONE ||
         Header.Command == GLINK_RX_DONE_REUSE)) continue;
    if (Read != Count)
      Link->Queue[(Link->QueueHead + Count) % CR_GLINK_QUEUE_SIZE] = Link->Queue[Slot];
    ++Count;
  }
  Link->QueueCount = Count;
}

STATIC VOID ServiceClose(CR_GLINK *Link, CR_GLINK_CHANNEL *Channel, CR_STATUS Reason)
{
  CR_GLINK_SERVICE *Service = Channel->Service;
  if (!Channel->Closing) ServiceDown(Link, Service, Reason);
  Channel->Closing = TRUE;
  Service->PeerClosed = TRUE;
  if (Reason != CR_NOT_READY && Reason != CR_SUCCESS) Service->Faulted = TRUE;
  ServiceDiscardTx(Link, Service->Local);
}

STATIC VOID ServiceFinishWire(CR_GLINK_CHANNEL *Channel)
{
  /* Once OPEN was sent, its peer OPEN may still be in flight even after
     CLOSE_ACK. Keep the slot until the other half closes, or edge reset. */
  if (!Channel->Closing ||
      (Channel->LocalOpen && (!Channel->CloseAck || !Channel->RemoteClosed)) ||
      (Channel->RemoteId && !Channel->RemoteClosed)) return;
  CR_GLINK_SERVICE *Service = Channel->Service;
  cr_memset(Channel, 0, sizeof(*Channel));
  Channel->Service = Service;
  cr_memcpy(Channel->Name, Service->Config.Name, sizeof(Channel->Name));
}

/* One stage pass has a fixed channel/fragment budget. Control traffic retains
   five queue slots; a later pass follows Flush so existing adapter TX-progress
   scheduling also observes pending large service messages. */
STATIC CR_STATUS ServiceStage(CR_GLINK *Link)
{
  if (!Link->Bound || !Link->VersionAck) return CR_SUCCESS;
  for (UINTN Index = 0; Index < CR_GLINK_CHANNELS; ++Index) {
    CR_GLINK_CHANNEL *Channel = &Link->Channels[Index];
    CR_GLINK_SERVICE *Service = Channel->Service;
    if (Service == NULL) continue;
    CR_STATUS Status;
    if (Channel->Closing) {
      if (Channel->LocalOpen && !Channel->CloseSent) {
        Status = Queue(Link, GLINK_CLOSE, Service->Local, 0, NULL, 0);
        if (CR_ERROR(Status)) return Status;
        Channel->CloseSent = TRUE;
      }
      ServiceFinishWire(Channel);
      continue;
    }
    if (Service->Detaching || Service->Faulted || Service->PeerClosed) continue;
    if (!Channel->LocalOpen) {
      UINTN Length = ServiceNameLength(Service->Config.Name);
      Status = Queue(Link, GLINK_OPEN, Service->Local, (UINT32)Length,
                     Service->Config.Name, Length);
      if (CR_ERROR(Status)) return Status;
      Channel->LocalOpen = TRUE;
    }
    if (!Channel->RemoteId || !Channel->OpenAck) continue;
    if (!Channel->Advertised) {
      UINT32 Pairs[CR_GLINK_SERVICE_INTENTS * 2];
      for (UINT32 Intent = 0; Intent < Service->Config.IntentCount; ++Intent) {
        Pairs[2 * Intent] = Service->Config.IntentBytes;
        Pairs[2 * Intent + 1] = Intent + 1;
      }
      Status = Queue(Link, GLINK_INTENT, Service->Local, Service->Config.IntentCount,
                     Pairs, Service->Config.IntentCount * 8U);
      if (CR_ERROR(Status)) return Status;
      Channel->Advertised = Service->Config.IntentCount;
    }
    if (!Service->Online) {
      Service->Online = TRUE;
      Service->LastError = CR_SUCCESS;
      ServiceNotify(Link, Service);
    }
    while (Service->TxLength && Link->QueueCount < CR_GLINK_QUEUE_SIZE - 5U) {
      UINT64 Payload[(CR_GLINK_MESSAGE_SIZE + 8) / 8];
      UINT32 *Prefix = (UINT32 *)Payload;
      UINT32 Chunk = Service->TxLength - Service->TxOffset;
      if (Chunk > CR_GLINK_MESSAGE_SIZE) Chunk = CR_GLINK_MESSAGE_SIZE;
      Prefix[0] = Chunk;
      Prefix[1] = Service->TxLength - Service->TxOffset - Chunk;
      cr_memcpy((UINT8 *)Payload + 8, Service->Config.TxStorage + Service->TxOffset, Chunk);
      Status = Queue(Link, Service->TxOffset ? GLINK_DATA_CONT : GLINK_DATA,
                     Service->Local, Service->TxIntent, Payload, Chunk + 8U);
      if (CR_ERROR(Status)) return Status;
      Service->TxOffset += Chunk;
      if (Service->TxOffset == Service->TxLength)
        Service->TxLength = Service->TxOffset = 0;
    }
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS Commit(CR_GLINK *Link, CR_GLINK_COMMIT_OPERATION Operation,
                        VOID *Context, BOOLEAN *Written)
{
  *Written = FALSE;
  return Link->Commit ? Link->Commit(Link->CommitContext, Operation, Context, Written) :
    Operation(Context, Written);
}

STATIC CR_STATUS FlushCommit(VOID *Context, BOOLEAN *Written)
{
  CR_GLINK *Link = Context;
  UINT32 Head = Link->Descriptor[1], Tail = Link->Descriptor[0];
  UINT32 Available;
  BOOLEAN Sent = FALSE;
  if (Head >= Link->TxSize || Tail >= Link->TxSize || ((Head | Tail) & 7U) != 0) {
    return CR_COMPROMISED_DATA;
  }
  CrMemoryFence();
  Available = Tail <= Head ? Link->TxSize - Head + Tail : Tail - Head;
  /* Reserve both the empty/full distinction and READ_NOTIF command. */
  Available = Available >= 16U ? Available - 16U : 0U;
  while (Link->QueueCount != 0) {
    CR_GLINK_QUEUED *Message = &Link->Queue[Link->QueueHead];
    if (Message->Size > Available) break;
    *Written = TRUE;
    RingCopy(Link->Tx, Link->TxSize, Head, Message->Data, Message->Size, TRUE);
    Head = (Head + Message->Size) % Link->TxSize;
    Available -= Message->Size;
    Link->QueueHead = (Link->QueueHead + 1U) % CR_GLINK_QUEUE_SIZE;
    --Link->QueueCount;
    Sent = TRUE;
    Link->ReadNotify = FALSE;
  }
  if (Link->QueueCount != 0 && !Link->ReadNotify &&
      (Tail <= Head ? Link->TxSize - Head + Tail : Tail - Head) >= 16U) {
    GLINK_HEADER Notify = {GLINK_READ_NOTIF, 0, 0};
    *Written = TRUE;
    RingCopy(Link->Tx, Link->TxSize, Head, &Notify, sizeof(Notify), TRUE);
    Head = (Head + sizeof(Notify)) % Link->TxSize;
    Link->ReadNotify = TRUE;
    Sent = TRUE;
  }
  if (Sent) {
    CrMemoryFence();
    Link->Descriptor[1] = Head;
    CrMemoryFence();
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS Flush(CR_GLINK *Link)
{
  BOOLEAN Written;
  CR_STATUS Status = Commit(Link, FlushCommit, Link, &Written);
  /* Never notify through another provider while the admission lock is held. */
  if (Status == CR_SUCCESS && Written && Link->Kick != NULL) Link->Kick(Link->Context);
  return Status;
}

typedef struct { CR_GLINK *Link; UINT32 Tail; BOOLEAN Initialize; } GLINK_INDEX_COMMIT;
STATIC CR_STATUS IndexCommit(VOID *Context, BOOLEAN *Written)
{
  GLINK_INDEX_COMMIT *C = Context;
  *Written = TRUE;
  CrMemoryFence();
  if (C->Initialize) C->Link->Descriptor[1] = 0;
  C->Link->Descriptor[2] = C->Tail;
  CrMemoryFence();
  return CR_SUCCESS;
}

CR_STATUS CrGlinkServiceTx(CR_GLINK *Link)
{
  if (Link == NULL) return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  if (!Link->Bound) return CR_NOT_READY;
  CR_STATUS Status = ServiceStage(Link);
  if (CR_ERROR(Status) && Status != CR_NOT_READY) return Status;
  Status = Flush(Link);
  if (CR_ERROR(Status)) return Status;
  Status = ServiceStage(Link);
  return Status == CR_NOT_READY ? CR_SUCCESS : Status;
}

CR_STATUS CrGlinkBind(CR_GLINK *Link, CONST CR_GLINK_SMEM *Smem)
{
  CR_STATUS Status;
  VOID *Address;
  VOID *Descriptor;
  UINT32 Size;
  if (Link == NULL || Smem == NULL || Smem->Allocate == NULL || Smem->Get == NULL)
    return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  if (Link->Bound) return CR_ALREADY_STARTED;
  Status = Smem->Allocate(Smem->Context, GLINK_HOST_ADSP, GLINK_DESCRIPTOR, 32, &Address);
  if (CR_ERROR(Status) && Status != CR_ALREADY_STARTED) return Status;
  Status = Smem->Get(Smem->Context, GLINK_HOST_ADSP, GLINK_DESCRIPTOR, &Size, &Address);
  if (CR_ERROR(Status)) return Status;
  if (Address == NULL || Size != 32 || ((UINTN)Address & 3U) != 0 ||
      !ServiceStorageOutside(Link, Address, Size))
    return CR_COMPROMISED_DATA;
  Descriptor = Address;
  Status = Smem->Allocate(Smem->Context, GLINK_HOST_ADSP, GLINK_TX_FIFO, GLINK_FIFO_SIZE, &Address);
  if (CR_ERROR(Status) && Status != CR_ALREADY_STARTED) return Status;
  Status = Smem->Get(Smem->Context, GLINK_HOST_ADSP, GLINK_TX_FIFO, &Size, &Address);
  if (CR_ERROR(Status)) return Status;
  if (Address == NULL || Size < CR_GLINK_MESSAGE_SIZE + 32U || Size > 131072 ||
      (Size & 7U) != 0 || !ServiceStorageOutside(Link, Address, Size) ||
      !ServiceDisjoint(Descriptor, 32, Address, Size)) return CR_COMPROMISED_DATA;
  Link->Descriptor = Descriptor;
  Link->Tx = Address;
  Link->TxSize = Size;
  GLINK_INDEX_COMMIT Initial = { Link, 0, TRUE };
  BOOLEAN Written;
  Status = Commit(Link, IndexCommit, &Initial, &Written);
  if (Status != CR_SUCCESS) return Status;
  Link->Features = 1U; /* GLINK_FEATURE_INTENT_REUSE */
  Link->Smem = *Smem;
  Link->Bound = TRUE;
  return Queue(Link, GLINK_VERSION, 1, Link->Features, NULL, 0);
}

STATIC CR_GLINK_CHANNEL *RemoteChannel(CR_GLINK *Link, UINT16 Id, UINT16 *Local)
{
  UINTN Index;
  for (Index = 0; Index < CR_GLINK_CHANNELS; ++Index) {
    if (Id != 0 && Link->Channels[Index].RemoteId == Id) {
      *Local = (UINT16)(Index + 1U);
      return &Link->Channels[Index];
    }
  }
  return NULL;
}

STATIC CR_STATUS Advertise(CR_GLINK *Link, UINT16 Local, UINT32 Id)
{
  UINT32 Pair[2] = {
    Link->Channels[Local - 1U].Service ?
      Link->Channels[Local - 1U].Service->Config.IntentBytes :
      Link->Channels[Local - 1U].Logs ? CR_GLINK_LOG_SIZE : CR_GLINK_MESSAGE_SIZE, Id
  };
  return Queue(Link, GLINK_INTENT, Local, 1, Pair, sizeof(Pair));
}

/* Advertisements have a protocol length independent of our fixed TX pool.
 * Linux qcom_glink_handle_intent consumes the full frame even for a channel
 * without a registered endpoint. Walk the ring directly so a valid larger
 * advertisement needs neither a larger stack buffer nor truncated framing. */
STATIC CR_STATUS ReceiveIntents(CR_GLINK *Link, CONST GLINK_HEADER *Header, UINT32 Tail)
{
  UINT16 Local = 0;
  CR_GLINK_CHANNEL *Channel = RemoteChannel(Link,Header->Param1,&Local);
  UINTN Index,Free,Dropped=0;
  if (Channel == NULL || Channel->Closing ||
      (!Channel->Pmic && !Channel->Qrtr && !Channel->Logs && !Channel->Service))
    return CR_SUCCESS;
  for (Index = 0; Index < Header->Param2; ++Index) {
    UINT32 Pair[2];
    RingCopy(Link->Rx,Link->RxSize,
             (UINT32)((Tail + 8U + Index * 8U) % Link->RxSize),Pair,sizeof(Pair),FALSE);
    UINT32 Size=Pair[0],Id=Pair[1];
    if (Size == 0) {
      if (Channel->Service) { ServiceClose(Link, Channel, CR_COMPROMISED_DATA); return CR_SUCCESS; }
      return CR_COMPROMISED_DATA;
    }
    /* Search the exact IID before a free slot, including IID0. A hole cannot
       shadow an existing busy reservation or create two owners of one IID. */
    for (Free=0; Free<CR_GLINK_REMOTE_INTENTS; ++Free)
      if (Channel->Remote[Free].Present && Channel->Remote[Free].Id==Id) break;
    if (Free==CR_GLINK_REMOTE_INTENTS)
      for (Free=0; Free<CR_GLINK_REMOTE_INTENTS; ++Free)
        if (!Channel->Remote[Free].Present) break;
    if (Free==CR_GLINK_REMOTE_INTENTS) {++Dropped;continue;}
    if (Channel->Remote[Free].Busy) {
      if (Channel->Service) { ServiceClose(Link, Channel, CR_COMPROMISED_DATA); return CR_SUCCESS; }
      return CR_COMPROMISED_DATA;
    }
    Channel->Remote[Free].Id=Id;Channel->Remote[Free].Size=Size;
    Channel->Remote[Free].Present=TRUE;
  }
  if (Dropped!=0)
    log_warn("UCSICr: GLink remote=%u advertised=%u tracked=%u unused=%u\n",
             Header->Param1,Header->Param2,CR_GLINK_REMOTE_INTENTS,(UINT32)Dropped);
  Channel->IntentRequested=FALSE;
  return CR_SUCCESS;
}

STATIC CR_STATUS ReceiveServicePacket(CR_GLINK *Link, CR_GLINK_CHANNEL *Channel,
                                      GLINK_HEADER *Header, UINT8 *Payload, UINTN Length)
{
  CR_GLINK_SERVICE *Service = Channel->Service;
  if (Channel->Closing || Service->Detaching || Service->Faulted) return CR_SUCCESS;
  if (Length < 8 || !Service->Online || !Header->Param2 ||
      Header->Param2 > Channel->Advertised) goto Malformed;
  UINT32 Chunk, Left;
  if (Payload != NULL) {
    cr_memcpy(&Chunk, Payload, 4);
    cr_memcpy(&Left, Payload + 4, 4);
  } else {
    UINT32 Prefix[2];
    RingCopy(Link->Rx, Link->RxSize, (Link->Descriptor[2] + 8U) % Link->RxSize,
             Prefix, sizeof(Prefix), FALSE);
    Chunk = Prefix[0];
    Left = Prefix[1];
  }
  UINT32 Index = Header->Param2 - 1;
  UINT32 Offset = Service->Offset[Index];
  UINT32 Capacity = Service->Config.IntentBytes;
  if (!Chunk || Chunk != Length - 8 || Offset > Capacity ||
      (Header->Command == GLINK_DATA ? Offset != 0 : Offset == 0) ||
      Chunk > Capacity - Offset || Left > Capacity - Offset - Chunk ||
      (Offset && Chunk + Left != Service->Remaining[Index])) goto Malformed;
  UINTN Needed = (Link->Features & 1U) ? 1U : 2U;
  if (!Left && Link->QueueCount > CR_GLINK_QUEUE_SIZE - Needed) return CR_NOT_READY;
  UINT8 *Data = Service->Config.RxStorage + (UINTN)Index * Capacity;
  if (Payload != NULL) cr_memcpy(Data + Offset, Payload + 8, Chunk);
  else RingCopy(Link->Rx, Link->RxSize, (Link->Descriptor[2] + 16U) % Link->RxSize,
                Data + Offset, Chunk, FALSE);
  Service->Offset[Index] = Offset + Chunk;
  Service->Remaining[Index] = Left;
  if (Left) return CR_SUCCESS;
  /* Reserve replies before callback delivery. They cannot reach the peer
     until the synchronous callback has returned and this pump flushes. */
  CR_STATUS Status = Queue(Link, (Link->Features & 1U) ? GLINK_RX_DONE_REUSE : GLINK_RX_DONE,
                           Service->Local, Header->Param2, NULL, 0);
  if (Status != CR_SUCCESS) goto Malformed;
  if (!(Link->Features & 1U) && Advertise(Link, Service->Local, Header->Param2) != CR_SUCCESS)
    goto Malformed;
  Link->ServiceCallback = TRUE;
  Status = Service->Config.Receive(Service->Config.Context, Data, Offset + Chunk,
                                    Service->Generation);
  Link->ServiceCallback = FALSE;
  Service->Offset[Index] = Service->Remaining[Index] = 0;
  if (Status != CR_SUCCESS) ServiceClose(Link, Channel, Status);
  return CR_SUCCESS;
Malformed:
  ServiceClose(Link, Channel, CR_COMPROMISED_DATA);
  return CR_SUCCESS;
}

STATIC CR_STATUS ReceivePacket(CR_GLINK *Link, GLINK_HEADER *Header,
                                UINT8 *Payload, UINTN Length)
{
  CR_GLINK_CHANNEL *Channel;
  UINT16 Local = 0;
  UINTN Index;
  UINT32 *Words = (UINT32 *)Payload;
  CR_STATUS Status;
  Channel = RemoteChannel(Link, Header->Param1, &Local);
  switch (Header->Command) {
    case GLINK_VERSION:
      if (Header->Param1 != 1) return CR_UNSUPPORTED;
      if (Link->VersionAck) {
        if (Link->QrtrState != NULL) Link->QrtrState(Link->Context, FALSE);
        if (Link->LogsState != NULL) Link->LogsState(Link->Context, FALSE);
        for (Index = 0; Index < CR_GLINK_CHANNELS; ++Index) {
          CR_GLINK_SERVICE *Service = Link->Channels[Index].Service;
          cr_memset(&Link->Channels[Index], 0, sizeof(Link->Channels[Index]));
          Link->Channels[Index].Service = Service;
          if (Service) {
            Service->Faulted = Service->PeerClosed = FALSE;
            ServiceDown(Link, Service, CR_NOT_READY);
            cr_memcpy(Link->Channels[Index].Name, Service->Config.Name, 32);
          }
        }
        Link->Ready = FALSE;
        Link->VersionAck = FALSE;
        Link->QueueHead = Link->QueueCount = 0;
        Link->ReadNotify = FALSE;
        ++Link->Generation;
        Link->Features = 1U;
        Status = Queue(Link, GLINK_VERSION, 1, Link->Features & Header->Param2, NULL, 0);
        if (CR_ERROR(Status)) return Status;
      }
      Link->Features &= Header->Param2;
      return Queue(Link, GLINK_VERSION_ACK, 1, Link->Features, NULL, 0);
    case GLINK_VERSION_ACK:
      if (Header->Param1 != 1) return CR_UNSUPPORTED;
      if (Header->Param2 != Link->Features) {
        Link->Features &= Header->Param2;
        return Queue(Link, GLINK_VERSION, 1, Link->Features, NULL, 0);
      }
      Link->VersionAck = TRUE;
      return CR_SUCCESS;
    case GLINK_OPEN:
      if (!Link->VersionAck || Header->Param1 == 0 || Length == 0 || Length > 32 ||
          Payload[Length - 1] != 0) return CR_COMPROMISED_DATA;
      if (Channel != NULL) return CR_COMPROMISED_DATA;
      for (Index = 0; Index < CR_GLINK_CHANNELS; ++Index) {
        CR_GLINK_CHANNEL *Candidate = &Link->Channels[Index];
        if (Candidate->Service && ServiceNameLength(Candidate->Name) == Length &&
            cr_memcmp(Candidate->Name, Payload, Length) == 0) {
          if (Candidate->RemoteId) return CR_COMPROMISED_DATA;
          Status = Queue(Link, GLINK_OPEN_ACK, Header->Param1, 0, NULL, 0);
          if (CR_ERROR(Status)) return Status;
          Candidate->RemoteId = Header->Param1;
          if (Candidate->Service->Detaching || Candidate->Service->Faulted ||
              Candidate->Service->PeerClosed) {
            if (!Candidate->LocalOpen) {
              Status = Queue(Link, GLINK_OPEN, Candidate->Service->Local,
                             (UINT32)Length, Payload, Length);
              if (CR_ERROR(Status)) return Status;
              Candidate->LocalOpen = TRUE;
            }
            ServiceClose(Link, Candidate, CR_NOT_READY);
          }
          return CR_SUCCESS;
        }
      }
      for (Index = 0; Index < CR_GLINK_CHANNELS; ++Index)
        if (Link->Channels[Index].RemoteId == 0 && !Link->Channels[Index].Closing &&
            Link->Channels[Index].Service == NULL) break;
      if (Index == CR_GLINK_CHANNELS) return CR_OUT_OF_RESOURCES;
      Channel = &Link->Channels[Index];
      Local = (UINT16)(Index + 1U);
      Channel->RemoteId = Header->Param1;
      cr_memcpy(Channel->Name, Payload, Length);
      Channel->Pmic = Length == sizeof("PMIC_RTR_ADSP_APPS") &&
        cr_memcmp(Payload, "PMIC_RTR_ADSP_APPS", Length) == 0;
      Channel->Qrtr = Length == sizeof("IPCRTR") &&
        cr_memcmp(Payload, "IPCRTR", Length) == 0;
      Channel->Logs = Length == sizeof("PMIC_LOGS_ADSP_APPS") &&
        cr_memcmp(Payload, "PMIC_LOGS_ADSP_APPS", Length) == 0;
      log_info("UCSICr: GLink channel " CR_LOG_CHAR8_STR_FMT " remote=%u local=%u\n",
              Payload, Header->Param1, Local);
      Status = Queue(Link, GLINK_OPEN_ACK, Header->Param1, 0, NULL, 0);
      if (CR_ERROR(Status)) return Status;
      Status = Queue(Link, GLINK_OPEN, Local, (UINT32)Length, Payload, Length);
      if (!CR_ERROR(Status)) Channel->LocalOpen = TRUE;
      return Status;
    case GLINK_OPEN_ACK:
      if (Header->Param1 == 0 || Header->Param1 > CR_GLINK_CHANNELS)
        return CR_COMPROMISED_DATA;
      Channel = &Link->Channels[Header->Param1 - 1U];
      if (Channel->Service) {
        if (!Channel->LocalOpen || Channel->OpenAck) {
          ServiceClose(Link, Channel, CR_COMPROMISED_DATA);
          return CR_SUCCESS;
        }
        Channel->OpenAck = TRUE;
        return CR_SUCCESS;
      }
      if (Channel->Closing) return CR_SUCCESS;
      if (Channel->RemoteId == 0 || Channel->OpenAck) return CR_COMPROMISED_DATA;
      Channel->OpenAck = TRUE;
      if (Channel->Pmic || Channel->Qrtr || Channel->Logs) {
        UINTN Count = Channel->Logs ? 1U : CR_GLINK_INTENTS;
        for (Index = 1; Index <= Count; ++Index) {
          Status = Advertise(Link, Header->Param1, (UINT32)Index);
          if (CR_ERROR(Status)) return Status;
        }
        if (Channel->Pmic) Link->Ready = TRUE;
        if (Channel->Qrtr && Link->QrtrState != NULL)
          Link->QrtrState(Link->Context, TRUE);
        if (Channel->Logs && Link->LogsState != NULL)
          Link->LogsState(Link->Context, TRUE);
      }
      return CR_SUCCESS;
    case GLINK_CLOSE:
      if (Channel != NULL && Channel->Service) {
        if (Channel->RemoteClosed) return CR_SUCCESS;
        Status = Queue(Link, GLINK_CLOSE_ACK, Header->Param1, 0, NULL, 0);
        if (CR_ERROR(Status)) return Status;
        Channel->RemoteClosed = TRUE;
        ServiceClose(Link, Channel, CR_NOT_READY);
        ServiceFinishWire(Channel);
        return CR_SUCCESS;
      }
      if (Channel == NULL || Channel->Closing) return CR_COMPROMISED_DATA;
      DiscardChannelTx(Link, Local);
      Status = Queue(Link, GLINK_CLOSE_ACK, Header->Param1, 0, NULL, 0);
      if (CR_ERROR(Status)) return Status;
      Status = Queue(Link, GLINK_CLOSE, Local, 0, NULL, 0);
      if (CR_ERROR(Status)) return Status;
      if (Channel->Pmic) { Link->Ready = FALSE; ++Link->Generation; }
      if (Channel->Qrtr && Link->QrtrState != NULL)
        Link->QrtrState(Link->Context, FALSE);
      if (Channel->Logs && Link->LogsState != NULL)
        Link->LogsState(Link->Context, FALSE);
      Channel->Closing = TRUE;
      Channel->OpenAck = FALSE;
      return Status;
    case GLINK_CLOSE_ACK:
      if (Header->Param1 == 0 || Header->Param1 > CR_GLINK_CHANNELS)
        return CR_COMPROMISED_DATA;
      Channel = &Link->Channels[Header->Param1 - 1U];
      if (Channel->Service) {
        if (!Channel->Closing || !Channel->CloseSent || Channel->CloseAck) {
          ServiceClose(Link, Channel, CR_COMPROMISED_DATA);
          return CR_SUCCESS;
        }
        Channel->CloseAck = TRUE;
        ServiceFinishWire(Channel);
        return CR_SUCCESS;
      }
      if (!Channel->Closing) return CR_COMPROMISED_DATA;
      cr_memset(Channel, 0, sizeof(*Channel));
      return CR_SUCCESS;
    case GLINK_SIGNALS:
      return CR_SUCCESS;
    case GLINK_INTENT_REQ_ACK:
      if (Channel == NULL) return CR_COMPROMISED_DATA;
      if (Header->Param2 == 0) Channel->IntentRequested = FALSE;
      return CR_SUCCESS;
    case GLINK_READ_NOTIF:
      return CR_SUCCESS;
    case GLINK_INTENT_REQ:
      return Queue(Link, GLINK_INTENT_REQ_ACK, Local,
        Channel != NULL && Channel->OpenAck && !Channel->Closing &&
        (Channel->Pmic || Channel->Qrtr || Channel->Logs || Channel->Service) &&
        Header->Param2 <= (Channel->Service ? Channel->Service->Config.IntentBytes :
          Channel->Logs ? CR_GLINK_LOG_SIZE : CR_GLINK_MESSAGE_SIZE), NULL, 0);
    case GLINK_RX_DONE:
    case GLINK_RX_DONE_REUSE:
      if (Channel == NULL) return CR_COMPROMISED_DATA;
      if (Channel->Closing) return CR_SUCCESS;
      for (Index = 0; Index < CR_GLINK_REMOTE_INTENTS; ++Index)
        if (Channel->Remote[Index].Present && Channel->Remote[Index].Id == Header->Param2) break;
      if (Index == CR_GLINK_REMOTE_INTENTS || !Channel->Remote[Index].Busy) {
        if (Channel->Service) { ServiceClose(Link, Channel, CR_COMPROMISED_DATA); return CR_SUCCESS; }
        return CR_COMPROMISED_DATA;
      }
      Channel->Remote[Index].Busy = FALSE;
      if (Header->Command == GLINK_RX_DONE)
        cr_memset(&Channel->Remote[Index],0,sizeof(Channel->Remote[Index]));
      return CR_SUCCESS;
    case GLINK_DATA:
    case GLINK_DATA_CONT: {
      if (Channel != NULL && Channel->Service)
        return ReceiveServicePacket(Link, Channel, Header, Payload, Length);
      UINT8 *Data;
      UINT32 *Offset, *Remaining;
      UINTN Capacity;
      UINT32 PreviousOffset, PreviousRemaining;
      UINT32 Chunk = Words[0], Left = Words[1];
      if (Channel != NULL && Channel->Closing) return CR_SUCCESS;
      if (Channel == NULL || (!Channel->Pmic && !Channel->Qrtr && !Channel->Logs) ||
          !Channel->OpenAck || Header->Param2 == 0 || Chunk == 0 ||
          Header->Param2 > (Channel->Logs ? 1U : CR_GLINK_INTENTS) ||
          Length < 8U || Chunk != Length - 8U)
        return CR_COMPROMISED_DATA;
      if (Channel->Logs) {
        Data = Channel->LogData; Offset = &Channel->LogOffset;
        Remaining = &Channel->LogRemaining; Capacity = sizeof(Channel->LogData);
      } else {
        CR_GLINK_LOCAL_INTENT *Intent = &Channel->Local[Header->Param2 - 1U];
        Data = Intent->Data; Offset = &Intent->Offset;
        Remaining = &Intent->Remaining; Capacity = sizeof(Intent->Data);
      }
      PreviousOffset = *Offset; PreviousRemaining = *Remaining;
      if (Header->Command == GLINK_DATA ? *Offset != 0 : *Offset == 0)
        return CR_COMPROMISED_DATA;
      if (Chunk > Capacity - *Offset || Left > Capacity - *Offset - Chunk ||
          (*Offset != 0 && Chunk + Left != *Remaining))
        return CR_COMPROMISED_DATA;
      cr_memcpy(Data + *Offset, Payload + 8, Chunk);
      *Offset += Chunk;
      *Remaining = Left;
      if (Left == 0) {
        if (Channel->Qrtr && Link->QrtrReceive != NULL) {
          Status = Link->QrtrReceive(Link->Context, Data, *Offset);
          if (CR_ERROR(Status)) {
            /* Do not consume or acknowledge this frame until the service
               can retain its reply/flow-control ACK under backpressure. */
            *Offset = PreviousOffset; *Remaining = PreviousRemaining;
            return Status;
          }
        }
        if (Channel->Logs && Link->LogsReceive != NULL) {
          Status = Link->LogsReceive(Link->Context, Data, *Offset);
          if (CR_ERROR(Status)) {
            *Offset = PreviousOffset; *Remaining = PreviousRemaining;
            return Status;
          }
        }
        Status = Queue(Link, Link->Features & 1U ? GLINK_RX_DONE_REUSE : GLINK_RX_DONE,
                       Local, Header->Param2, NULL, 0);
        if (CR_ERROR(Status)) return Status;
        if (!(Link->Features & 1U)) {
          Status = Advertise(Link, Local, Header->Param2);
          if (CR_ERROR(Status)) return Status;
        }
        if (Channel->Pmic && Link->Receive != NULL)
          Link->Receive(Link->Context, Data, *Offset);
        *Offset = 0;
      }
      return CR_SUCCESS;
    }
    default:
      return CR_UNSUPPORTED;
  }
}

CR_STATUS CrGlinkPoll(CR_GLINK *Link)
{
  UINTN Budget;
  CR_STATUS Status;
  if (Link == NULL || !Link->Bound) return CR_NOT_READY;
  if (Link->ServiceCallback) return CR_BUSY;
  Status = CrGlinkServiceTx(Link);
  if (CR_ERROR(Status)) return Status;
  if (Link->Rx == NULL) {
    VOID *Address = NULL;
    UINT32 Size = 0;
    if (Link->Smem.Get == NULL) return CR_NOT_READY;
    Status = Link->Smem.Get(Link->Smem.Context, GLINK_HOST_ADSP, GLINK_RX_FIFO, &Size, &Address);
    if (Status == CR_NOT_FOUND) return CR_NOT_READY;
    if (CR_ERROR(Status)) return Status;
    if (Address == NULL || Size < CR_GLINK_MESSAGE_SIZE + 32U || Size > 131072 ||
        (Size & 7U) != 0 || !ServiceOutside(Link, Address, Size))
      return CR_COMPROMISED_DATA;
    Link->Rx = Address;
    Link->RxSize = Size;
  }
  for (Budget = 0; Budget < 16U; ++Budget) {
    UINT32 Head = Link->Descriptor[3], Tail = Link->Descriptor[2];
    UINTN Available, Length = 0, Size;
    GLINK_HEADER Header;
    UINT32 Prefix[2];
    UINT64 Payload[(CR_GLINK_LOG_SIZE + 8 + 7) / 8];
    CR_GLINK_CHANNEL *Direct = NULL;
    if (Head >= Link->RxSize || Tail >= Link->RxSize || ((Head | Tail) & 7U) != 0) {
      log_err("UCSICr: GLink invalid RX indices head=0x%x tail=0x%x size=0x%x\n",Head,Tail,Link->RxSize);
      return CR_COMPROMISED_DATA;
    }
    CrMemoryFence();
    Available = Head >= Tail ? Head - Tail : Link->RxSize - Tail + Head;
    if (Available < sizeof(Header)) break;
    if (Link->QueueCount > CR_GLINK_QUEUE_SIZE - 5U) break;
    RingCopy(Link->Rx, Link->RxSize, Tail, &Header, sizeof(Header), FALSE);
    if (Header.Command == GLINK_OPEN) Length = Header.Param2 & 0xFFFFU;
    else if (Header.Command == GLINK_INTENT) {
      /* A ring always reserves8 bytes to distinguish full from empty; the
         complete header/pairs must fit without wrapping onto unread data. */
      if (Header.Param2 > (Link->RxSize - 16U) / 8U) {
        log_err("UCSICr: GLink impossible INTENT count=%u remote=%u tail=0x%x rx-size=0x%x\n",
                Header.Param2,Header.Param1,Tail,Link->RxSize);
        return CR_COMPROMISED_DATA;
      }
      Length = Header.Param2 * 8U;
    } else if (Header.Command == GLINK_DATA || Header.Command == GLINK_DATA_CONT) {
      if (Available < 16U) break;
      RingCopy(Link->Rx, Link->RxSize, (Tail + 8U) % Link->RxSize, Prefix, sizeof(Prefix), FALSE);
      UINT16 Local = 0;
      CR_GLINK_CHANNEL *Candidate = RemoteChannel(Link, Header.Param1, &Local);
      if (Candidate != NULL && Candidate->Service) Direct = Candidate;
      if (Prefix[0] > Link->RxSize - 24U ||
          (Direct == NULL && Prefix[0] > CR_GLINK_LOG_SIZE)) return CR_COMPROMISED_DATA;
      Length = 8U + Prefix[0];
    }
    if (Header.Command != GLINK_INTENT && Direct == NULL && Length > sizeof(Payload))
      return CR_COMPROMISED_DATA;
    Size = Align8(sizeof(Header) + Length);
    if (Size > Available) break;
    if (Header.Command != GLINK_INTENT && Direct == NULL && Length != 0)
      RingCopy(Link->Rx, Link->RxSize, (Tail + 8U) % Link->RxSize, Payload, Length, FALSE);
    Status = Direct != NULL ? ReceiveServicePacket(Link, Direct, &Header, NULL, Length) :
      Header.Command == GLINK_INTENT ? ReceiveIntents(Link,&Header,Tail) :
      ReceivePacket(Link, &Header, (UINT8 *)Payload, Length);
    if (Status == CR_NOT_READY) break;
    if (CR_ERROR(Status)) {
      log_err("UCSICr: GLink RX rejected cmd=%u remote=%u param2=0x%x bytes=%u tail=0x%x head=0x%x: " CR_LOG_STATUS_FMT "\n",
              Header.Command,Header.Param1,Header.Param2,(UINT32)Length,Tail,Head,Status);
      return Status;
    }
    GLINK_INDEX_COMMIT Advance = { Link, (UINT32)((Tail + Size) % Link->RxSize), FALSE };
    BOOLEAN Written;
    Status = Commit(Link, IndexCommit, &Advance, &Written);
    if (Status != CR_SUCCESS) return Status;
    if (Header.Command == GLINK_READ_NOTIF && Link->Kick != NULL) Link->Kick(Link->Context);
    Status = CrGlinkServiceTx(Link);
    if (CR_ERROR(Status)) return Status;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS SendChannel(CR_GLINK *Link, CONST VOID *Data, UINTN Length,
                              UINTN Service)
{
  UINTN ChannelIndex, Index;
  CR_STATUS Status;
  UINT64 Payload[(CR_GLINK_MESSAGE_SIZE + 8) / 8];
  UINT32 *Prefix = (UINT32 *)Payload;
  if (Link == NULL || Data == NULL || Length == 0 || Length > CR_GLINK_MESSAGE_SIZE)
    return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  if (!Link->Bound || !Link->VersionAck) return CR_NOT_READY;
  for (ChannelIndex = 0; ChannelIndex < CR_GLINK_CHANNELS; ++ChannelIndex) {
    CR_GLINK_CHANNEL *Channel = &Link->Channels[ChannelIndex];
    if (!(Service == 2U ? Channel->Logs : Service == 1U ? Channel->Qrtr : Channel->Pmic) ||
        !Channel->OpenAck || Channel->Closing) continue;
    for (Index = 0; Index < CR_GLINK_REMOTE_INTENTS; ++Index) {
      CR_GLINK_REMOTE_INTENT *Intent = &Channel->Remote[Index];
      if (!Intent->Present || Intent->Busy || Intent->Size < Length) continue;
      Prefix[0] = (UINT32)Length;
      Prefix[1] = 0;
      cr_memcpy((UINT8 *)Payload + 8, Data, Length);
      Status = Queue(Link, GLINK_DATA, (UINT16)(ChannelIndex + 1U), Intent->Id,
                     Payload, Length + 8U);
      if (!CR_ERROR(Status)) Intent->Busy = TRUE;
      return Status;
    }
    if (!Channel->IntentRequested) {
      Status = Queue(Link, GLINK_INTENT_REQ, (UINT16)(ChannelIndex + 1U), (UINT32)Length, NULL, 0);
      if (CR_ERROR(Status)) return Status;
      Channel->IntentRequested = TRUE;
    }
    return CR_NOT_READY;
  }
  return CR_NOT_READY;
}

CR_STATUS CrGlinkSend(CR_GLINK *Link, CONST VOID *Data, UINTN Length)
{ return SendChannel(Link, Data, Length, 0); }

CR_STATUS CrGlinkSendQrtr(CR_GLINK *Link, CONST VOID *Data, UINTN Length)
{ return SendChannel(Link, Data, Length, 1); }

CR_STATUS CrGlinkSendLogs(CR_GLINK *Link, CONST VOID *Data, UINTN Length)
{ return SendChannel(Link, Data, Length, 2); }

CR_STATUS CrGlinkServiceRegister(CR_GLINK *Link, CR_GLINK_SERVICE *Service,
                                 CONST CR_GLINK_SERVICE_CONFIG *Config)
{
  if (Link == NULL || Service == NULL || Config == NULL ||
      ((UINTN)Service % _Alignof(CR_GLINK_SERVICE)) ||
      ((UINTN)Config % _Alignof(CR_GLINK_SERVICE_CONFIG))) return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  if (ServiceChannel(Link, Service)) return CR_ALREADY_STARTED;
  if (!ServiceOutside(Link, Service, sizeof(*Service)) ||
      !ServiceDisjoint(Service, sizeof(*Service), Config, sizeof(*Config)))
    return CR_INVALID_PARAMETER;
  CR_GLINK_SERVICE_CONFIG Copy = *Config;
  UINTN Length = ServiceNameLength(Copy.Name);
  if (!Length || !Copy.Receive || !Copy.IntentCount ||
      Copy.IntentCount > CR_GLINK_SERVICE_INTENTS || !Copy.IntentBytes ||
      Copy.IntentBytes > CR_GLINK_SERVICE_MESSAGE_MAX ||
      Copy.RxBytes != (UINTN)Copy.IntentCount * Copy.IntentBytes ||
      !Copy.TxBytes || Copy.TxBytes > CR_GLINK_SERVICE_MESSAGE_MAX ||
      !ServiceOutside(Link, Copy.RxStorage, Copy.RxBytes) ||
      !ServiceOutside(Link, Copy.TxStorage, Copy.TxBytes) ||
      !ServiceDisjoint(Copy.RxStorage, Copy.RxBytes, Copy.TxStorage, Copy.TxBytes) ||
      !ServiceDisjoint(Service, sizeof(*Service), Copy.RxStorage, Copy.RxBytes) ||
      !ServiceDisjoint(Service, sizeof(*Service), Copy.TxStorage, Copy.TxBytes) ||
      !ServiceDisjoint(Config, sizeof(*Config), Copy.RxStorage, Copy.RxBytes) ||
      !ServiceDisjoint(Config, sizeof(*Config), Copy.TxStorage, Copy.TxBytes))
    return CR_INVALID_PARAMETER;
  CONST CHAR8 *Reserved[] = {"PMIC_RTR_ADSP_APPS", "IPCRTR", "PMIC_LOGS_ADSP_APPS"};
  CONST UINTN ReservedBytes[] = {sizeof("PMIC_RTR_ADSP_APPS"), sizeof("IPCRTR"),
                                 sizeof("PMIC_LOGS_ADSP_APPS")};
  for (UINTN Index = 0; Index < 3; ++Index)
    if (Length == ReservedBytes[Index] && !cr_memcmp(Copy.Name, Reserved[Index], Length))
      return CR_UNSUPPORTED;
  for (UINTN Index = 0; Index < sizeof(*Service); ++Index)
    if (((CONST UINT8 *)Service)[Index]) return CR_INVALID_PARAMETER;
  CR_GLINK_CHANNEL *Selected = NULL;
  UINTN SelectedIndex = 0;
  for (UINTN Index = 0; Index < CR_GLINK_CHANNELS; ++Index) {
    CR_GLINK_CHANNEL *Channel = &Link->Channels[Index];
    if (ServiceNameLength(Channel->Name) == Length &&
        !cr_memcmp(Channel->Name, Copy.Name, Length)) {
      if (Channel->Service) return CR_BUSY;
      if (Channel->Closing) return CR_NOT_READY;
      Selected = Channel;
      SelectedIndex = Index;
      break;
    }
    if (!Selected && !Channel->RemoteId && !Channel->Service && !Channel->Closing) {
      Selected = Channel;
      SelectedIndex = Index;
    }
  }
  if (!Selected) return CR_OUT_OF_RESOURCES;
  if (Link->ServiceGeneration == ~(UINT64)0) return CR_OUT_OF_RESOURCES;
  Service->Config = Copy;
  Service->Local = (UINT16)(SelectedIndex + 1);
  Service->Registered = TRUE;
  Service->LastError = CR_NOT_READY;
  (VOID)ServiceAdvance(Link, Service);
  Selected->Service = Service;
  cr_memcpy(Selected->Name, Copy.Name, sizeof(Selected->Name));
  /* Registration retains storage even while the shared queue is full. The
     existing IRQ/TX pump owns later progress; no FIFO write occurs here. */
  return CR_SUCCESS;
}

CR_STATUS CrGlinkServiceUnregister(CR_GLINK *Link, CR_GLINK_SERVICE *Service)
{
  if (!Link || !Service) return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  CR_GLINK_CHANNEL *Channel = ServiceChannel(Link, Service);
  if (!Channel) return CR_NOT_FOUND;
  if (!Service->Detaching) {
    ServiceClose(Link, Channel, CR_NOT_READY);
    Service->Detaching = TRUE;
  }
  ServiceFinishWire(Channel);
  if (Channel->Closing || Channel->LocalOpen || Channel->RemoteId) return CR_NOT_READY;
  cr_memset(Channel, 0, sizeof(*Channel));
  cr_memset(Service, 0, sizeof(*Service));
  return CR_SUCCESS;
}

CR_STATUS CrGlinkServiceQuiesce(CR_GLINK *Link, CR_GLINK_SERVICE *Service,
                                UINT64 ExpectedGeneration)
{
  if (!Link || !Service || !ExpectedGeneration ||
      ((UINTN)Service % _Alignof(CR_GLINK_SERVICE))) return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  CR_GLINK_CHANNEL *Channel = ServiceChannel(Link, Service);
  if (!Channel || !Service->Registered || !Service->Local ||
      Service->Local > CR_GLINK_CHANNELS ||
      Channel != &Link->Channels[Service->Local - 1U]) return CR_NOT_FOUND;
  if (Service->Generation != ExpectedGeneration) return CR_NOT_READY;
  /* CPU-only close/discard. Do not progress wire handshakes from the QRTR
     callback: Poll will do that after this exact service has stopped staging. */
  ServiceClose(Link, Channel, CR_NOT_READY);
  return CR_SUCCESS;
}

CR_STATUS CrGlinkServiceReopen(CR_GLINK *Link, CR_GLINK_SERVICE *Service)
{
  if (!Link || !Service) return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  CR_GLINK_CHANNEL *Channel = ServiceChannel(Link, Service);
  if (!Channel) return CR_NOT_FOUND;
  if (Service->Detaching || Channel->Closing || Channel->LocalOpen || Channel->RemoteId)
    return CR_BUSY;
  if (!ServiceAdvance(Link, Service)) return CR_OUT_OF_RESOURCES;
  Service->Faulted = Service->PeerClosed = FALSE;
  Service->LastError = CR_NOT_READY;
  return CR_SUCCESS;
}

CR_STATUS CrGlinkServiceSend(CR_GLINK *Link, CR_GLINK_SERVICE *Service,
                             UINT64 Generation, CONST VOID *Data, UINTN Length)
{
  if (!Link || !Service || !Generation || !Data || !Length) return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  CR_GLINK_CHANNEL *Channel = ServiceChannel(Link, Service);
  if (!Channel) return CR_NOT_FOUND;
  if (!ServiceOutside(Link, Data, Length)) return CR_INVALID_PARAMETER;
  if (!Link->Bound || !Link->VersionAck || !Service->Online || Service->Detaching ||
      Service->Faulted || Channel->Closing || Generation != Service->Generation)
    return CR_NOT_READY;
  if (Length > Service->Config.TxBytes) return CR_BAD_BUFFER_SIZE;
  if (Service->TxLength) return CR_NOT_READY;
  for (UINTN Index = 0; Index < CR_GLINK_REMOTE_INTENTS; ++Index) {
    CR_GLINK_REMOTE_INTENT *Intent = &Channel->Remote[Index];
    if (!Intent->Present || Intent->Busy || Intent->Size < Length) continue;
    cr_memcpy(Service->Config.TxStorage, Data, Length);
    Service->TxLength = (UINT32)Length;
    Service->TxOffset = 0;
    Service->TxIntent = Intent->Id;
    Intent->Busy = TRUE;
    return CR_SUCCESS;
  }
  if (!Channel->IntentRequested) {
    CR_STATUS Status = Queue(Link, GLINK_INTENT_REQ, Service->Local, (UINT32)Length, NULL, 0);
    if (CR_ERROR(Status)) return Status;
    Channel->IntentRequested = TRUE;
  }
  return CR_NOT_READY;
}

CR_STATUS CrGlinkServiceGetState(CR_GLINK *Link, CR_GLINK_SERVICE *Service,
                                 BOOLEAN *Online, UINT64 *Generation, CR_STATUS *LastError)
{
  if (!Link || !Service || !Online || !Generation || !LastError ||
      ((UINTN)Generation % _Alignof(UINT64)) || ((UINTN)LastError % _Alignof(CR_STATUS)))
    return CR_INVALID_PARAMETER;
  if (Link->ServiceCallback) return CR_BUSY;
  if (!ServiceChannel(Link, Service)) return CR_NOT_FOUND;
  if (!ServiceOutside(Link, Online, sizeof(*Online)) ||
      !ServiceOutside(Link, Generation, sizeof(*Generation)) ||
      !ServiceOutside(Link, LastError, sizeof(*LastError)) ||
      !ServiceDisjoint(Online, sizeof(*Online), Generation, sizeof(*Generation)) ||
      !ServiceDisjoint(Online, sizeof(*Online), LastError, sizeof(*LastError)) ||
      !ServiceDisjoint(Generation, sizeof(*Generation), LastError, sizeof(*LastError)))
    return CR_INVALID_PARAMETER;
  *Online = Service->Online;
  *Generation = Service->Generation;
  *LastError = Service->LastError;
  return CR_SUCCESS;
}
