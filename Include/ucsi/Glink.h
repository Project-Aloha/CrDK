/** UCSICr bounded SM8450 ADSP GLink transport. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>


#define CR_GLINK_MESSAGE_SIZE 1024U
/* charger-ulog-glink.c: 12-byte PMIC envelope and fixed 8192-byte log. */
#define CR_GLINK_LOG_SIZE (12U + 8192U)
#define CR_GLINK_CHANNELS 8U
#define CR_GLINK_INTENTS 4U
#define CR_GLINK_REMOTE_INTENTS 64U
#define CR_GLINK_QUEUE_SIZE 32U
#define CR_GLINK_SERVICE_INTENTS 32U
#define CR_GLINK_SERVICE_MESSAGE_MAX 65536U

typedef struct _CR_GLINK_SERVICE CR_GLINK_SERVICE;
typedef CR_STATUS (*CR_GLINK_SERVICE_RECEIVE)(VOID *Context, CONST UINT8 *Data,
                                             UINTN Length, UINT64 Generation);
typedef VOID (*CR_GLINK_SERVICE_STATE)(VOID *Context, BOOLEAN Online,
                                      UINT64 Generation, CR_STATUS Reason);
typedef struct {
  CHAR8 Name[32];
  UINT32 IntentCount, IntentBytes;
  UINT8 *RxStorage;
  UINTN RxBytes;
  UINT8 *TxStorage;
  UINTN TxBytes;
  CR_GLINK_SERVICE_RECEIVE Receive;
  CR_GLINK_SERVICE_STATE State;
  VOID *Context;
} CR_GLINK_SERVICE_CONFIG;

/* Caller-owned, zeroed, stable storage. Fields below are core-private while
   registered. Rx/Tx storage is retained independently of a wire OPEN. */
struct _CR_GLINK_SERVICE {
  CR_GLINK_SERVICE_CONFIG Config;
  UINT64 Generation;
  UINT32 Offset[CR_GLINK_SERVICE_INTENTS];
  UINT32 Remaining[CR_GLINK_SERVICE_INTENTS];
  UINT32 TxLength, TxOffset, TxIntent;
  UINT16 Local;
  BOOLEAN Registered, Online, Detaching, Faulted, PeerClosed;
  CR_STATUS LastError;
};

typedef VOID (*CR_GLINK_RECEIVE)(VOID *Context, CONST UINT8 *Data, UINTN Length);
typedef CR_STATUS (*CR_GLINK_QRTR_RECEIVE)(VOID *Context, CONST UINT8 *Data, UINTN Length);
typedef VOID (*CR_GLINK_STATE)(VOID *Context, BOOLEAN Online);
typedef VOID (*CR_GLINK_KICK)(VOID *Context);
/* Optional trusted current-instance commit fence. The operation performs only
 * a bounded shared-memory copy/publication: no provider, callback, allocation,
 * wait or polling. It sets Written before the first possible store. A later
 * fence error must preserve Written and all committed progress; never replay.
 * Gate returns exact success only if admission and operation both succeeded.
 * Native callers with no gate retain their existing serialized behavior. */
typedef CR_STATUS (*CR_GLINK_COMMIT_OPERATION)(VOID *Context, BOOLEAN *Written);
typedef CR_STATUS (*CR_GLINK_COMMIT)(VOID *Context, CR_GLINK_COMMIT_OPERATION Operation,
                                    VOID *OperationContext, BOOLEAN *Written);
typedef CR_STATUS (*CR_GLINK_SMEM_GET)(VOID *Context, UINT32 Host, UINT32 Item,
                                     UINT32 *Size, VOID **Address);
typedef CR_STATUS (*CR_GLINK_SMEM_ALLOCATE)(VOID *Context, UINT32 Host, UINT32 Item,
                                          UINT32 Size, VOID **Address);
typedef struct {
  CR_GLINK_SMEM_GET Get;
  CR_GLINK_SMEM_ALLOCATE Allocate;
  VOID *Context;
} CR_GLINK_SMEM;
/* Remote IID0 is valid on the wire; allocation state is independent. */
typedef struct { UINT32 Id, Size; BOOLEAN Busy, Present; } CR_GLINK_REMOTE_INTENT;
typedef struct {
  UINT8 Data[CR_GLINK_MESSAGE_SIZE];
  UINT32 Offset, Remaining;
} CR_GLINK_LOCAL_INTENT;
typedef struct {
  UINT16 RemoteId;
  BOOLEAN OpenAck, Pmic, Qrtr, Logs, Closing;
  CHAR8 Name[32];
  CR_GLINK_SERVICE *Service;
  BOOLEAN LocalOpen, CloseSent, CloseAck, RemoteClosed;
  UINT32 Advertised;
  CR_GLINK_REMOTE_INTENT Remote[CR_GLINK_REMOTE_INTENTS];
  CR_GLINK_LOCAL_INTENT Local[CR_GLINK_INTENTS];
  UINT8 LogData[CR_GLINK_LOG_SIZE];
  UINT32 LogOffset, LogRemaining;
  BOOLEAN IntentRequested;
} CR_GLINK_CHANNEL;
typedef struct {
  UINT16 Size;
  UINT8 Data[CR_GLINK_MESSAGE_SIZE + 16];
} CR_GLINK_QUEUED;
typedef struct {
  CR_GLINK_SMEM Smem;
  volatile UINT32 *Descriptor;
  UINT8 *Tx, *Rx;
  UINT32 TxSize, RxSize, Features;
  BOOLEAN Bound, VersionAck, Ready;
  BOOLEAN ReadNotify;
  UINT32 Generation;
  UINT64 ServiceGeneration;
  BOOLEAN ServiceCallback;
  CR_GLINK_CHANNEL Channels[CR_GLINK_CHANNELS];
  CR_GLINK_QUEUED Queue[CR_GLINK_QUEUE_SIZE];
  UINTN QueueHead, QueueCount;
  CR_GLINK_RECEIVE Receive;
  CR_GLINK_QRTR_RECEIVE QrtrReceive;
  CR_GLINK_STATE QrtrState;
  CR_GLINK_QRTR_RECEIVE LogsReceive;
  CR_GLINK_STATE LogsState;
  CR_GLINK_KICK Kick;
  VOID *Context;
  CR_GLINK_COMMIT Commit;
  VOID *CommitContext;
} CR_GLINK;

CR_STATUS CrGlinkBind(CR_GLINK *Link, CONST CR_GLINK_SMEM *Smem);
CR_STATUS CrGlinkPoll(CR_GLINK *Link);
/* TX progress does not inspect the RX FIFO. RX service is IRQ-triggered. */
CR_STATUS CrGlinkServiceTx(CR_GLINK *Link);
CR_STATUS CrGlinkSend(CR_GLINK *Link, CONST VOID *Data, UINTN Length);
CR_STATUS CrGlinkSendQrtr(CR_GLINK *Link, CONST VOID *Data, UINTN Length);
CR_STATUS CrGlinkSendLogs(CR_GLINK *Link, CONST VOID *Data, UINTN Length);
VOID CrGlinkReset(CR_GLINK *Link);
/* Reset preserves the installed immutable commit gate just like Kick. */

/* Same external serialization as Poll/Send/Reset. Callbacks are synchronous,
   nonblocking and must not reenter GLINK, allocate, wait, or retain Data.
   Receive returns exact SUCCESS only after retaining its own copy. Any other
   result closes that service, consumes the framed packet and leaves sibling
   services running. State reports channel availability, never ADSP/APM health.

   Register copies Config, retains Service/RxStorage/TxStorage until exact
   successful Unregister, and starts a named OPEN after edge negotiation.
   Supports remote-first, local-first and crossed OPEN; names are unique and
   built-in PMIC/QRTR/log names are reserved. Buffers and mutable objects must
   not overlap each other or any other registered service/transport storage.
   RxBytes must equal IntentCount*IntentBytes; TxBytes is 1..64KiB. Complete
   messages are bounded by their advertised intent; TX uses <=1024-byte wire
   fragments and copies accepted input before returning.

   Unregister starts closing and returns NOT_READY while peer close/ack remains;
   caller keeps all storage/code alive and services IRQ/TX. A timeout does not
   permit freeing it. Qualified edge Reset invalidates all service generations
   and retires old wire IDs; it preserves registrations and monotonic epochs.
   Reopen is explicit after a channel failure/remote close. Outer OS adapters
   must close/drain their own callbacks/work before freeing a client.
   ServiceGetState outputs must be disjoint from all retained core storage. */
CR_STATUS CrGlinkServiceRegister(CR_GLINK *Link, CR_GLINK_SERVICE *Service,
                                 CONST CR_GLINK_SERVICE_CONFIG *Config);
CR_STATUS CrGlinkServiceUnregister(CR_GLINK *Link, CR_GLINK_SERVICE *Service);
/* Close local admission for this exact current registered channel epoch.
   Serialized with Poll/Send/Reset; the built-in QrtrReceive/QrtrState callbacks
   may call this before Poll resumes TX. Named Receive/State callbacks may not
   reenter it. State notification remains synchronous under that serialization.

   Exact SUCCESS proves only that unsent local service payload/intent traffic
   and partial RX bookkeeping were discarded. Ordered OPEN/CLOSE controls,
   registration, caller buffers and callback code remain retained. Already
   published SMEM bytes and remote commands/graphs/DMA are NOT retired. There
   is no MMIO access, allocation or wait here. Keep servicing the shared edge;
   only exact successful Unregister permits releasing retained storage.

   Zero, wrong registration/local identity or stale epoch fails before any
   mutation. Epoch exhaustion still closes admission and latches a permanent
   service fault, reported by State/GetState; SUCCESS does not promise Reopen.
   A retry with the resulting current epoch is allowed while closing. */
CR_STATUS CrGlinkServiceQuiesce(CR_GLINK *Link, CR_GLINK_SERVICE *Service,
                                UINT64 ExpectedGeneration);
CR_STATUS CrGlinkServiceReopen(CR_GLINK *Link, CR_GLINK_SERVICE *Service);
CR_STATUS CrGlinkServiceSend(CR_GLINK *Link, CR_GLINK_SERVICE *Service,
                             UINT64 Generation, CONST VOID *Data, UINTN Length);
CR_STATUS CrGlinkServiceGetState(CR_GLINK *Link, CR_GLINK_SERVICE *Service,
                                 BOOLEAN *Online, UINT64 *Generation,
                                 CR_STATUS *LastError);
