/* Independent named-service wire replay against the actual GLINK core.
   Host fixtures are synthetic protocol traffic, never hardware captures.
   SPDX-License-Identifier: MIT */
#include <ucsi/Glink.h>
#include <Library/AudioGprCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#include <Library/DebugLib.h>
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D, 0, N); }
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) { return memset(D, V, N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A, B, N); }
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L) { (void)L; return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (void)L; (void)F; }
#endif

static unsigned Checks;
#define CHECK(X) do { ++Checks; if (!(X)) { \
  fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #X); assert(0); } } while (0)
static const char *Case;
static CR_GLINK Link;
static CR_GLINK_SERVICE Service;
static CR_GLINK_SMEM Smem;
static UINT32 Descriptor[8];
static UINT8 Tx[16384], Rx[16384];
static UINT8 OwnedRx[20 * 65536], OwnedTx[65536];
static UINT8 Copied[65536], Input[65536], OnWire[65536];
static UINTN Calls, CopiedBytes, PmicCalls, States, OnlineStates, WireBytes, WireFragments;
static UINTN ExpectedWireBytes;
static UINT64 LastGeneration;
static CR_STATUS CallbackResult;
static UINT16 Local, Remote;
static UINTN Commands[16];
static UINT32 NegotiatedFeatures = 1;
static BOOLEAN CheckQuiesceReentry;

static UINT32 Get32(const UINT8 *P)
{
  return (UINT32)P[0] | (UINT32)P[1] << 8 | (UINT32)P[2] << 16 | (UINT32)P[3] << 24;
}

static void Put32(UINT8 *P, UINT32 V)
{
  for (unsigned I = 0; I < 4; ++I) P[I] = (UINT8)(V >> (8 * I));
}

static CR_STATUS Get(VOID *Context, UINT32 Host, UINT32 Item, UINT32 *Bytes, VOID **Address)
{
  CHECK(Context == &Smem && Host == 2);
  if (Item == 478) { *Bytes = sizeof(Descriptor); *Address = Descriptor; }
  else if (Item == 479) { *Bytes = sizeof(Tx); *Address = Tx; }
  else if (Item == 480) { *Bytes = sizeof(Rx); *Address = Rx; }
  else return CR_NOT_FOUND;
  return CR_SUCCESS;
}

static CR_STATUS Allocate(VOID *Context, UINT32 Host, UINT32 Item, UINT32 Bytes, VOID **Address)
{
  CHECK(Context == &Smem && Host == 2 && (Item == 478 || Item == 479));
  CHECK(Bytes == (Item == 478 ? sizeof(Descriptor) : sizeof(Tx)));
  *Address = NULL;
  return CR_ALREADY_STARTED;
}

static void PmicReceive(VOID *Context, CONST UINT8 *Data, UINTN Bytes)
{
  CHECK(Context == &Link && Bytes == 1 && Data[0] == 0xa5);
  ++PmicCalls;
}

static CR_STATUS Receive(VOID *Context, CONST UINT8 *Data, UINTN Bytes, UINT64 Generation)
{
  CHECK(Context == &Service && Bytes <= sizeof(Copied));
  CHECK(Generation != 0 && Generation == Service.Generation);
  if (CheckQuiesceReentry)
    CHECK(CrGlinkServiceQuiesce(&Link, &Service, Generation) == CR_BUSY);
  memcpy(Copied, Data, Bytes);
  CopiedBytes = Bytes;
  LastGeneration = Generation;
  ++Calls;
  return CallbackResult;
}

static void State(VOID *Context, BOOLEAN Online, UINT64 Generation, CR_STATUS Reason)
{
  CHECK(Context == &Service && Generation != 0);
  CHECK(!Online || Reason == CR_SUCCESS);
  if (CheckQuiesceReentry)
    CHECK(CrGlinkServiceQuiesce(&Link, &Service, Generation) == CR_BUSY);
  ++States;
  OnlineStates += !!Online;
  LastGeneration = Generation;
}

/* Encode commands without the core's private wire structs. */
static void Packet(UINT16 Command, UINT16 Channel, UINT32 Param, const void *Payload, UINTN Bytes)
{
  UINT8 Header[8] = {(UINT8)Command, (UINT8)(Command >> 8),
                    (UINT8)Channel, (UINT8)(Channel >> 8)};
  Put32(Header + 4, Param);
  UINTN Size = (Bytes + 15) & ~(UINTN)7;
  UINTN Used = (Descriptor[3] + sizeof(Rx) - Descriptor[2]) % sizeof(Rx);
  CHECK(Size <= sizeof(Rx) - Used - 8);
  for (UINTN I = 0; I < Size; ++I) {
    UINT8 Value = I < 8 ? Header[I] : I < Bytes + 8 ? ((const UINT8 *)Payload)[I - 8] : 0;
    Rx[(Descriptor[3] + I) % sizeof(Rx)] = Value;
  }
  Descriptor[3] = (UINT32)((Descriptor[3] + Size) % sizeof(Rx));
}

static void Data(UINT16 Channel, UINT32 Intent, BOOLEAN First,
                 const UINT8 *Bytes, UINT32 Chunk, UINT32 Left)
{
  UINT8 Payload[16352];
  CHECK(Chunk <= sizeof(Payload) - 8);
  Put32(Payload, Chunk); Put32(Payload + 4, Left);
  memcpy(Payload + 8, Bytes, Chunk);
  Packet(First ? 9 : 12, Channel, Intent, Payload, Chunk + 8);
}

static void Pump(void)
{
  CR_STATUS Status = CrGlinkPoll(&Link);
  if (Status != CR_SUCCESS) {
    UINT32 Tail = Descriptor[2];
    fprintf(stderr, "case=%s status=%llx tail=%u head=%u command=%u channel=%u\n",
            Case, (unsigned long long)Status, Tail, Descriptor[3],
            Rx[Tail] | (unsigned)Rx[(Tail + 1) % sizeof(Rx)] << 8,
            Rx[(Tail + 2) % sizeof(Rx)] | (unsigned)Rx[(Tail + 3) % sizeof(Rx)] << 8);
  }
  CHECK(Status == CR_SUCCESS);
}

/* Decode the peer-facing FIFO independently, including wrapped fragments. */
static void Drain(void)
{
  while (Descriptor[0] != Descriptor[1]) {
    UINT8 Header[16];
    UINT32 Tail = Descriptor[0];
    UINTN Available = (Descriptor[1] + sizeof(Tx) - Tail) % sizeof(Tx);
    CHECK(Available >= 8);
    for (unsigned I = 0; I < sizeof(Header); ++I) Header[I] = Tx[(Tail + I) % sizeof(Tx)];
    UINT16 Command = Header[0] | (UINT16)Header[1] << 8;
    UINT16 Channel = Header[2] | (UINT16)Header[3] << 8;
    UINT32 Param = Get32(Header + 4);
    UINTN Size = 8;
    CHECK(Command < 16);
    ++Commands[Command];
    if (Command == 2) Size += Param & 0xffff;
    if (Command == 5) Size += (UINTN)Param * 8;
    if (Command == 9 || Command == 12) {
      UINT32 Chunk = Get32(Header + 8), Left = Get32(Header + 12);
      Size += 8 + Chunk;
      if (Channel == Local) {
        CHECK(Chunk > 0 && Chunk <= 1024 && Chunk <= sizeof(OnWire) - WireBytes);
        CHECK(Command == (WireBytes ? 12 : 9));
        for (UINTN I = 0; I < Chunk; ++I) OnWire[WireBytes + I] = Tx[(Tail + 16 + I) % sizeof(Tx)];
        WireBytes += Chunk;
        CHECK(WireBytes <= ExpectedWireBytes && Left == ExpectedWireBytes - WireBytes);
        ++WireFragments;
      }
    }
    Size = (Size + 7) & ~(UINTN)7;
    CHECK(Size <= Available);
    Descriptor[0] = (UINT32)((Tail + Size) % sizeof(Tx));
  }
}

static void Register(UINT32 Count, UINT32 Bytes)
{
  CR_GLINK_SERVICE_CONFIG Config = {0};
  memcpy(Config.Name, "adsp_apps", sizeof("adsp_apps"));
  Config.IntentCount = Count; Config.IntentBytes = Bytes;
  Config.RxStorage = OwnedRx; Config.RxBytes = (UINTN)Count * Bytes;
  Config.TxStorage = OwnedTx; Config.TxBytes = sizeof(OwnedTx);
  Config.Receive = Receive; Config.State = State; Config.Context = &Service;
  CHECK(CrGlinkServiceRegister(&Link, &Service, &Config) == CR_SUCCESS);
  Local = Service.Local;
  memset(&Config, 0xd3, sizeof(Config)); /* Registration must own its copy. */
}

static void Fixture(void)
{
  memset(&Link, 0, sizeof(Link)); memset(&Service, 0, sizeof(Service));
  memset(Descriptor, 0, sizeof(Descriptor)); memset(Tx, 0, sizeof(Tx));
  memset(Rx, 0, sizeof(Rx)); memset(Commands, 0, sizeof(Commands));
  memset(OwnedRx, 0xcc, sizeof(OwnedRx)); memset(OwnedTx, 0xcc, sizeof(OwnedTx));
  Calls = CopiedBytes = PmicCalls = States = OnlineStates = WireBytes = WireFragments = 0;
  ExpectedWireBytes = sizeof(OnWire);
  LastGeneration = 0; CallbackResult = CR_SUCCESS; Local = 0; Remote = 91;
  CheckQuiesceReentry = FALSE;
  Link.Receive = PmicReceive; Link.Context = &Link;
  Smem.Get = Get; Smem.Allocate = Allocate; Smem.Context = &Smem;
  CHECK(CrGlinkBind(&Link, &Smem) == CR_SUCCESS);
  Pump(); Drain();
  Packet(0, 1, NegotiatedFeatures, NULL, 0); Packet(1, 1, NegotiatedFeatures, NULL, 0);
  Packet(2, 77, sizeof("PMIC_RTR_ADSP_APPS"), "PMIC_RTR_ADSP_APPS", sizeof("PMIC_RTR_ADSP_APPS"));
  Packet(4, 1, 0, NULL, 0);
  Pump(); Drain();
  CHECK(Link.Ready);
}

static void Connect(UINT32 Count, UINT32 Bytes)
{
  Register(Count, Bytes);
  Pump(); Drain(); /* Local OPEN is visible before peer OPEN/ACK. */
  Packet(2, Remote, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
  Packet(4, Local, 0, NULL, 0);
  Pump(); Drain();
  CHECK(Service.Online && OnlineStates == 1);
}

static void PmicPacket(void)
{
  UINT8 Value = 0xa5;
  Data(77, 1, TRUE, &Value, 1, 0);
}

static void Handshakes(void)
{
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    Fixture();
    if (Mode == 0) { /* Remote channel already announced before client registration. */
      Packet(2, Remote, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
      Packet(4, 2, 0, NULL, 0); Pump(); Drain();
      Register(20, 512);
    } else {
      Register(20, 512);
      if (Mode == 1) { Pump(); Drain(); }
      Packet(2, Remote, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
      Packet(4, Local, 0, NULL, 0);
    }
    Pump(); Drain();
    CHECK(Local == 2 && Service.Online && OnlineStates == 1);
    CHECK(Commands[2] == 2); /* One PMIC OPEN, one audio OPEN. */
    for (unsigned I = 0; I < 20; ++I) {
      UINT8 Byte = (UINT8)I;
      Data(Remote, I + 1, TRUE, &Byte, 1, 1);
    }
    Pump(); Drain(); Pump(); Drain();
    CHECK(Calls == 0);
    for (unsigned I = 0; I < 20; ++I) {
      UINT8 Byte = (UINT8)(0x80 + I);
      Data(Remote, I + 1, FALSE, &Byte, 1, 0);
      Pump(); Drain();
      CHECK(Calls == I + 1 && CopiedBytes == 2 && Copied[0] == I && Copied[1] == Byte);
    }
  }
}

static void LargeTransfers(void)
{
  Fixture(); Connect(2, 65536);
  for (UINTN I = 0; I < sizeof(Input); ++I) Input[I] = (UINT8)(I * 29 + I / 251);
  /* A legal 12 KiB wire fragment exceeds the legacy PMIC-log scratch buffer. */
  for (UINT32 Offset = 0; Offset < sizeof(Input);) {
    UINT32 Chunk = (UINT32)sizeof(Input) - Offset;
    if (Chunk > 12288) Chunk = 12288;
    Data(Remote, 2, Offset == 0, Input + Offset, Chunk, (UINT32)sizeof(Input) - Offset - Chunk);
    Pump(); Drain(); Offset += Chunk;
    CHECK(Calls == (Offset == sizeof(Input) ? 1U : 0U));
  }
  CHECK(CopiedBytes == sizeof(Input) && !memcmp(Copied, Input, sizeof(Input)));
  UINT8 Intent[8]; Put32(Intent, sizeof(Input)); Put32(Intent + 4, 0); /* IID0 is legal. */
  Packet(5, Remote, 1, Intent, sizeof(Intent)); Pump(); Drain();
  CHECK(CrGlinkServiceSend(&Link, &Service, Service.Generation, Input, sizeof(Input)) == CR_SUCCESS);
  memset(Input, 0xa7, sizeof(Input)); /* Accepted send must retain its own bytes. */
  for (unsigned I = 0; I < 100 && WireBytes < sizeof(OnWire); ++I) { Pump(); Drain(); }
  CHECK(WireBytes == sizeof(OnWire) && WireFragments == 64);
  CHECK(!memcmp(OnWire, Copied, sizeof(OnWire)));
}

static void FaultIsolation(void)
{
  for (unsigned Kind = 0; Kind < 7; ++Kind) {
    Fixture(); Connect(20, 512);
    UINT8 Bytes[8] = {0};
    BOOLEAN InvokesCallback = Kind == 0 || Kind == 5;
    if (InvokesCallback) { /* Even nonzero success-class status is not acceptance. */
      CallbackResult = Kind == 0 ? CR_NOT_READY : (CR_STATUS)1;
      Data(Remote, 1, TRUE, Bytes, 1, 0);
    } else if (Kind == 1) { /* A zero-sized remote intent belongs to audio. */
      Put32(Bytes + 4, 8); Packet(5, Remote, 1, Bytes, sizeof(Bytes));
    } else if (Kind == 2) { /* Unknown RX_DONE belongs to audio. */
      Packet(14, Remote, 123, NULL, 0);
    } else if (Kind == 3) { /* Continuation with no first fragment. */
      Data(Remote, 1, FALSE, Bytes, 1, 0);
    } else if (Kind == 4) { /* Advertised capacity violation in a fully framed message. */
      Data(Remote, 1, TRUE, Bytes, 1, 512);
    } else { /* Duplicate advertisement must not clear a busy intent reservation. */
      Put32(Bytes, 512); Put32(Bytes + 4, 0);
      Packet(5, Remote, 1, Bytes, sizeof(Bytes)); Pump(); Drain();
      ExpectedWireBytes = sizeof(Bytes);
      CHECK(CrGlinkServiceSend(&Link, &Service, Service.Generation, Bytes, sizeof(Bytes)) == CR_SUCCESS);
      Pump(); Drain();
      Packet(5, Remote, 1, Bytes, sizeof(Bytes));
    }
    PmicPacket(); Pump(); Drain();
    CHECK(PmicCalls == 1 && Descriptor[2] == Descriptor[3] && Link.Ready);
    CHECK(!Service.Online && Service.PeerClosed && Calls == (InvokesCallback ? 1U : 0U));
    Pump(); Drain(); CHECK(Calls == (InvokesCallback ? 1U : 0U));
  }
}

static void CloseAndEpochs(void)
{
  Fixture(); Register(20, 512); Pump(); Drain();
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_NOT_READY);
  Pump(); Drain();
  /* A CLOSE_ACK alone cannot retire an unseen remote half of a local OPEN. */
  Packet(11, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_NOT_READY);
  CHECK(Service.Registered && Service.Config.Receive == Receive);
  Packet(2, Remote, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
  Packet(4, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_NOT_READY);
  UINT8 Discarded = 0x33;
  Data(Remote, 1, TRUE, &Discarded, 1, 0); PmicPacket(); Pump(); Drain();
  CHECK(Calls == 0 && PmicCalls == 1);
  Packet(3, Remote, 0, NULL, 0); Pump(); Drain();
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_SUCCESS);

  Fixture(); Register(20, 512); Pump(); Drain();
  UINT64 Before = Service.Generation;
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_NOT_READY);
  Pump(); Drain();
  CHECK(Service.Registered && Service.Generation > Before);
  /* Local CLOSE can complete before a delayed OPEN_ACK and remote OPEN. */
  Packet(4, Local, 0, NULL, 0);
  Packet(2, Remote, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
  Pump(); Drain();
  Packet(11, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_NOT_READY);
  Packet(3, Remote, 0, NULL, 0); Pump(); Drain();
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_SUCCESS);
  CHECK(!Service.Registered && !Service.Config.Receive);

  Fixture(); Connect(20, 512); Before = Service.Generation;
  Packet(3, Remote, 0, NULL, 0); Pump(); Drain();
  Packet(11, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(Service.PeerClosed && !Service.Online && Service.Generation > Before);
  /* Peer may reannounce, but explicit local reopen admission stays closed. */
  Packet(2, Remote, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
  Packet(4, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(!Service.Online && OnlineStates == 1);
  Packet(3, Remote, 0, NULL, 0); Packet(11, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(CrGlinkServiceReopen(&Link, &Service) == CR_SUCCESS);
  Pump(); Drain();
  Packet(2, Remote, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
  Packet(4, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(Service.Online && Service.Generation > Before && OnlineStates == 2);
  UINT8 Byte = 1;
  CHECK(CrGlinkServiceSend(&Link, &Service, Before, &Byte, 1) == CR_NOT_READY);
  Before = Service.Generation;
  CrGlinkReset(&Link);
  CHECK(Service.Registered && !Service.Online && Service.Generation > Before);
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_SUCCESS);
}

static void FinalFragmentBackpressure(void)
{
  Fixture(); Connect(20, 512);
  UINT8 Byte = 0x42;
  Data(Remote, 1, TRUE, &Byte, 1, 1); Pump(); Drain();
  /* Peer advertises enough independent PMIC intents to fill the real TX queue. */
  UINT8 Pairs[64 * 8];
  for (unsigned I = 0; I < 64; ++I) { Put32(Pairs + I * 8, 1024); Put32(Pairs + I * 8 + 4, I); }
  Packet(5, 77, 64, Pairs, sizeof(Pairs)); Pump(); Drain();
  UINT8 Filler[1024] = {0};
  unsigned Accepted = 0;
  for (unsigned I = 0; I < 64; ++I) {
    CR_STATUS Status = CrGlinkSend(&Link, Filler, sizeof(Filler));
    if (Status == CR_NOT_READY) break;
    CHECK(Status == CR_SUCCESS); ++Accepted;
    Pump(); /* Do not let the peer consume any outgoing FIFO bytes. */
  }
  CHECK(Accepted >= 32 && Link.QueueCount > CR_GLINK_QUEUE_SIZE - 5);
  Data(Remote, 1, FALSE, &Byte, 1, 0);
  UINT32 Tail = Descriptor[2];
  for (unsigned I = 0; I < 4; ++I) { Pump(); CHECK(!Calls && Descriptor[2] == Tail); }
  for (unsigned I = 0; I < 100 && !Calls; ++I) { Drain(); Pump(); }
  CHECK(Calls == 1 && CopiedBytes == 2 && Copied[0] == Byte && Copied[1] == Byte);
  for (unsigned I = 0; I < 4; ++I) { Drain(); Pump(); CHECK(Calls == 1); }
}

static void GprReadiness(void)
{
  Fixture(); Connect(20, 512);
  static const UINT8 ExpectedQuery[] = {
    0x60,0x18,0,0, 2,3,0,0, 3,0,0,0, 1,0,0,0,
    0x78,0x56,0x34,0x12, 0x21,0x10,0,1
  };
  static const UINT8 Reply[] = {
    0x60,0x1c,0,0, 3,2,0,0, 1,0,0,0, 3,0,0,0,
    0x78,0x56,0x34,0x12, 7,0x10,0,2, 1,0,0,0
  };
  UINT8 Intent[8], Query[24];
  Put32(Intent, 512); Put32(Intent + 4, 27);
  Packet(5, Remote, 1, Intent, sizeof(Intent)); Pump(); Drain();
  CHECK(CrAudioGprBuildSpfQuery(3, 0x12345678, Query, sizeof(Query)) == CR_SUCCESS);
  CHECK(!memcmp(Query, ExpectedQuery, sizeof(Query)));
  ExpectedWireBytes = sizeof(Query);
  CHECK(CrGlinkServiceSend(&Link, &Service, Service.Generation, Query, sizeof(Query)) == CR_SUCCESS);
  Pump(); Drain();
  CHECK(WireBytes == sizeof(Query) && !memcmp(OnWire, ExpectedQuery, sizeof(Query)));
  UINT64 AdmittedGeneration = Service.Generation;
  Data(Remote, 20, TRUE, Reply, 9, sizeof(Reply) - 9); Pump(); Drain();
  CHECK(Calls == 0);
  Data(Remote, 20, FALSE, Reply + 9, sizeof(Reply) - 9, 0); Pump(); Drain();
  CHECK(Calls == 1 && LastGeneration == AdmittedGeneration);
  CR_AUDIO_SPF_REPLY Result;
  CHECK(CrAudioGprMatchSpfReply(Copied, CopiedBytes, 3, 0x12345678, &Result) == CR_SUCCESS);
  CHECK(Result.Kind == CrAudioSpfReady && Result.Value == 1);
  CHECK(CrAudioGprMatchSpfReply(Copied, CopiedBytes, 3, 0x12345679, &Result) == CR_NOT_FOUND);
}

static void LocalQuiesce(void)
{
  Fixture(); Connect(20, 512);
  UINT64 Epoch = Service.Generation;
  static CR_GLINK SavedLink;
  CR_GLINK_SERVICE SavedService = Service, Copy = Service;
  SavedLink = Link;
  CHECK(CrGlinkServiceQuiesce(NULL, &Service, Epoch) == CR_INVALID_PARAMETER);
  CHECK(CrGlinkServiceQuiesce(&Link, NULL, Epoch) == CR_INVALID_PARAMETER);
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, 0) == CR_INVALID_PARAMETER);
  CHECK(CrGlinkServiceQuiesce(&Link, &Copy, Epoch) == CR_NOT_FOUND);
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, Epoch + 1) == CR_NOT_READY);
  CHECK(!memcmp(&SavedLink, &Link, sizeof(Link)) && !memcmp(&SavedService, &Service, sizeof(Service)));
  Service.Local = 1;
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, Epoch) == CR_NOT_FOUND);
  Service.Local = Local; Service.Registered = FALSE;
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, Epoch) == CR_NOT_FOUND);
  Service.Registered = TRUE;
  CHECK(!memcmp(&SavedLink, &Link, sizeof(Link)) && !memcmp(&SavedService, &Service, sizeof(Service)));

  CheckQuiesceReentry = TRUE;
  UINT8 Byte = 0x7a;
  Data(Remote, 1, TRUE, &Byte, 1, 0); Pump(); Drain();
  CHECK(Calls == 1 && Service.Online);
  UINT32 AudioIntent[2] = {65536, 301}, PmicIntent[2] = {1024, 302};
  Packet(5, Remote, 1, AudioIntent, sizeof(AudioIntent));
  Packet(5, 77, 1, PmicIntent, sizeof(PmicIntent)); Pump(); Drain();
  for (UINTN I = 0; I < sizeof(Input); ++I) Input[I] = (UINT8)(I * 17 + 9);
  CHECK(CrGlinkServiceSend(&Link, &Service, Epoch, Input, sizeof(Input)) == CR_SUCCESS);
  Descriptor[0] = (Descriptor[1] + 8U) % sizeof(Tx);
  CHECK(CrGlinkSend(&Link, &Byte, 1) == CR_SUCCESS);
  Data(Remote, 2, TRUE, &Byte, 1, 1); Pump();
  CHECK(Service.Offset[1] == 1 && Service.Remaining[1] == 1 && Service.TxLength && Service.TxOffset && Link.QueueCount > 1);
  CR_GLINK_QUEUED PmicQueued = Link.Queue[Link.QueueHead];
  CHECK(PmicQueued.Data[0] == 9 && PmicQueued.Data[2] == 1);
  UINT32 SavedDescriptor[8]; static UINT8 SavedTx[sizeof(Tx)], SavedRx[sizeof(Rx)];
  memcpy(SavedDescriptor, Descriptor, sizeof(Descriptor));
  memcpy(SavedTx, Tx, sizeof(Tx)); memcpy(SavedRx, Rx, sizeof(Rx));
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, Epoch) == CR_SUCCESS);
  CHECK(!memcmp(SavedDescriptor, Descriptor, sizeof(Descriptor)) && !memcmp(SavedTx, Tx, sizeof(Tx)) && !memcmp(SavedRx, Rx, sizeof(Rx)));
  CHECK(Service.Registered && Service.PeerClosed && !Service.Online && !Service.Detaching && !Service.Faulted);
  CHECK(!Service.TxLength && !Service.TxOffset && !Service.Offset[1] && !Service.Remaining[1]);
  CHECK(!memcmp(OwnedTx, Input, sizeof(Input)) && Service.Config.RxStorage == OwnedRx && Service.Config.TxStorage == OwnedTx);
  CHECK(Service.Config.Receive == Receive && Service.Config.State == State && Service.Generation > Epoch);
  CHECK(Link.QueueCount == 1 && !memcmp(&PmicQueued, &Link.Queue[Link.QueueHead], sizeof(PmicQueued)));
  CHECK(CrGlinkServiceSend(&Link, &Service, Service.Generation, &Byte, 1) == CR_NOT_READY);
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, Epoch) == CR_NOT_READY);
  Epoch = Service.Generation;
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, Epoch) == CR_SUCCESS && Service.Generation == Epoch);
  CHECK(CrGlinkServiceReopen(&Link, &Service) == CR_BUSY);
  Descriptor[0] = Descriptor[1]; Pump(); Drain();
  CHECK(!WireBytes && !WireFragments && Commands[3] == 1 && Link.Ready);
  Data(Remote, 2, FALSE, &Byte, 1, 0); PmicPacket(); Pump(); Drain();
  CHECK(Calls == 1 && PmicCalls == 1 && Service.Registered);
  Packet(11, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(CrGlinkServiceReopen(&Link, &Service) == CR_BUSY);
  Packet(3, Remote, 0, NULL, 0); Pump(); Drain();
  CHECK(CrGlinkServiceReopen(&Link, &Service) == CR_SUCCESS);
  Pump(); Drain();
  Packet(2, Remote, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
  Packet(4, Local, 0, NULL, 0); Pump(); Drain();
  CHECK(Service.Online && Service.Generation > Epoch && Service.Registered);
  CHECK(CrGlinkServiceSend(&Link, &Service, Epoch, &Byte, 1) == CR_NOT_READY);

  /* A queued OPEN remains ahead of CLOSE; quiescence never unregisters it. */
  Fixture(); Register(20, 512);
  Descriptor[0] = (Descriptor[1] + 8U) % sizeof(Tx);
  CHECK(CrGlinkServiceTx(&Link) == CR_SUCCESS);
  CHECK(Link.QueueCount == 1 && Link.Queue[Link.QueueHead].Data[0] == 2);
  CR_GLINK_QUEUED OpenQueued = Link.Queue[Link.QueueHead];
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, Service.Generation) == CR_SUCCESS);
  CHECK(Link.QueueCount == 1 && !memcmp(&OpenQueued, &Link.Queue[Link.QueueHead], sizeof(OpenQueued)));
  Descriptor[0] = Descriptor[1]; Pump(); Drain();
  CHECK(Commands[2] == 2 && Commands[3] == 1 && Service.Registered);
  CHECK(CrGlinkServiceUnregister(&Link, &Service) == CR_NOT_READY);

  /* No reset/reopen may turn counter exhaustion into a reused wire epoch. */
  Fixture(); Connect(20, 512); Epoch = Service.Generation;
  Link.ServiceGeneration = ~(UINT64)0;
  CHECK(CrGlinkServiceQuiesce(&Link, &Service, Epoch) == CR_SUCCESS);
  CHECK(Service.Faulted && !Service.Online && Service.LastError == CR_OUT_OF_RESOURCES);
  CHECK(Service.Generation == Epoch && Link.ServiceGeneration == ~(UINT64)0);
  CrGlinkReset(&Link);
  CHECK(Service.Faulted && !Service.Online && Service.Registered && Link.ServiceGeneration == ~(UINT64)0);
  CHECK(CrGlinkServiceReopen(&Link, &Service) == CR_OUT_OF_RESOURCES);
}

static void NonReusableIntents(void)
{
  NegotiatedFeatures = 0;
  Fixture(); Connect(20, 512);
  CHECK(Link.Features == 0);
  UINT8 Payload[512];
  for (unsigned Round = 0; Round < 2; ++Round) {
    for (unsigned Intent = 1; Intent <= 20; ++Intent) {
      memset(Payload, (int)(Intent + Round * 20), sizeof(Payload));
      Data(Remote, Intent, TRUE, Payload, sizeof(Payload), 0); Pump(); Drain();
      CHECK(Calls == Round * 20 + Intent && CopiedBytes == sizeof(Payload));
      CHECK(!memcmp(Copied, Payload, sizeof(Payload)));
    }
  }
  CHECK(Commands[6] == 40 && Commands[14] == 0 && Commands[5] == 45);
  NegotiatedFeatures = 1;
}

int main(int Argc, char **Argv)
{
  assert(Argc <= 2);
#define RUN(Name, Function) do { if (Argc == 1 || !strcmp(Argv[1], Name)) { \
  Case = Name; Function(); } } while (0)
  RUN("handshake", Handshakes);
  RUN("large", LargeTransfers);
  RUN("fault", FaultIsolation);
  RUN("close", CloseAndEpochs);
  RUN("backpressure", FinalFragmentBackpressure);
  RUN("gpr", GprReadiness);
  RUN("nonreuse", NonReusableIntents);
  RUN("quiesce", LocalQuiesce);
  assert(Checks > 0);
  printf("GLINK service peer tests: %u checks passed\n", Checks);
  return 0;
}
