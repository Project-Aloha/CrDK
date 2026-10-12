/* Actual named-channel wire replay, independent literal framing oracle. MIT */
#define main ExistingTransportMain
#include "UsbTypeCTransportTest.c"
#undef main

static CR_GLINK_SERVICE Audio;
static UINT8 AudioRx[20 * 65536], AudioTx[65536], Message[65536];
static UINT8 AppCopy[65536], WireCopy[65536];
static UINTN Calls, AppBytes, Changes, WireBytes, Fragments, Opens;
static CR_STATUS CallbackResult;
static UINT64 ObservedGeneration;
static UINT32 AliasItem;
static VOID *AliasAddress;

static CR_STATUS AliasedGet(VOID *Context, UINT32 Host, UINT32 Item,
                            UINT32 *Size, VOID **Address)
{
  CR_STATUS Status = Get(Context, Host, Item, Size, Address);
  if (Status == CR_SUCCESS && Item == AliasItem) *Address = AliasAddress;
  return Status;
}

static CR_STATUS AudioReceive(VOID *Context, CONST UINT8 *Data, UINTN Bytes, UINT64 Epoch)
{
  assert(Context == &Audio && Bytes <= sizeof(AppCopy));
  assert(Epoch == Audio.Generation);
  assert(CrGlinkServiceUnregister(&Link, &Audio) == CR_BUSY);
  assert(CrGlinkPoll(&Link) == CR_BUSY);
  ++Calls;
  memcpy(AppCopy, Data, Bytes);
  AppBytes = Bytes;
  return CallbackResult;
}

static VOID AudioState(VOID *Context, BOOLEAN Online, UINT64 Epoch, CR_STATUS Reason)
{
  assert(Context == &Audio && Epoch >= ObservedGeneration);
  assert(!Online || Reason == CR_SUCCESS);
  ObservedGeneration = Epoch;
  ++Changes;
}

static CR_GLINK_SERVICE_CONFIG Config(UINT32 Capacity)
{
  CR_GLINK_SERVICE_CONFIG C = {0};
  memcpy(C.Name, "adsp_apps", sizeof("adsp_apps"));
  C.IntentCount = 20;
  C.IntentBytes = Capacity;
  C.RxStorage = AudioRx;
  C.RxBytes = 20U * Capacity;
  C.TxStorage = AudioTx;
  C.TxBytes = sizeof(AudioTx);
  C.Receive = AudioReceive;
  C.State = AudioState;
  C.Context = &Audio;
  return C;
}

static VOID Start(UINT32 Capacity, BOOLEAN RemoteFirst)
{
  Fixture();
  memset(&Audio, 0, sizeof(Audio));
  Calls = AppBytes = Changes = WireBytes = Fragments = Opens = 0;
  ObservedGeneration = 0;
  CallbackResult = CR_SUCCESS;
  if (RemoteFirst) {
    Packet(2, 88, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
    Packet(4, 2, 0, NULL, 0);
    assert(CrGlinkPoll(&Link) == CR_SUCCESS);
  }
  CR_GLINK_SERVICE_CONFIG C = Config(Capacity);
  assert(CrGlinkServiceRegister(&Link, &Audio, &C) == CR_SUCCESS);
  assert(Audio.Local == 2 && !Audio.Online);
  assert(CrGlinkServiceTx(&Link) == CR_SUCCESS);
  if (!RemoteFirst) {
    /* ACK may arrive before peer OPEN: distinct local/remote directions. */
    Packet(4, 2, 0, NULL, 0);
    assert(CrGlinkPoll(&Link) == CR_SUCCESS && !Audio.Online);
    Packet(2, 88, sizeof("adsp_apps"), "adsp_apps", sizeof("adsp_apps"));
    assert(CrGlinkPoll(&Link) == CR_SUCCESS);
  }
  assert(Audio.Online && Changes == 1 && Link.Ready);
  Descriptor[0] = Descriptor[1];
}

static VOID Close(VOID)
{
  assert(CrGlinkServiceUnregister(&Link, &Audio) == CR_NOT_READY);
  assert(CrGlinkServiceTx(&Link) == CR_SUCCESS);
  Packet(11, 2, 0, NULL, 0);
  assert(CrGlinkPoll(&Link) == CR_SUCCESS);
  assert(CrGlinkServiceUnregister(&Link, &Audio) == CR_NOT_READY);
  Packet(3, 88, 0, NULL, 0);
  assert(CrGlinkPoll(&Link) == CR_SUCCESS);
  assert(CrGlinkServiceUnregister(&Link, &Audio) == CR_SUCCESS && !Audio.Registered);
  assert(Link.Ready);
}

static VOID ReadTx(UINT32 At, VOID *Out, UINTN Bytes)
{
  for (UINTN I = 0; I < Bytes; ++I) ((UINT8 *)Out)[I] = Tx[(At + I) % sizeof(Tx)];
}

static VOID ConsumeTx(VOID)
{
  while (Descriptor[0] != Descriptor[1]) {
    UINT32 At = Descriptor[0], Param;
    UINT16 Command, Local;
    ReadTx(At, &Command, 2);
    ReadTx(At + 2, &Local, 2);
    ReadTx(At + 4, &Param, 4);
    UINTN Bytes = 0;
    if (Command == 2) { Bytes = Param; if (Local == 2) ++Opens; }
    if (Command == 5) Bytes = Param * 8;
    if (Command == 9 || Command == 12) {
      UINT32 Prefix[2];
      ReadTx(At + 8, Prefix, sizeof(Prefix));
      Bytes = 8 + Prefix[0];
      assert(Local == 2 && Param == 0 && Prefix[0] <= 1024);
      assert(Command == (WireBytes ? 12 : 9));
      assert(Prefix[1] == sizeof(Message) - WireBytes - Prefix[0]);
      ReadTx(At + 16, WireCopy + WireBytes, Prefix[0]);
      WireBytes += Prefix[0];
      ++Fragments;
    }
    Descriptor[0] = (UINT32)((At + ((8 + Bytes + 7) & ~(UINTN)7)) % sizeof(Tx));
  }
}

static VOID LargeSend(VOID)
{
  Start(512, FALSE);
  UINT32 Pair[2] = {65536, 0};
  Packet(5, 88, 1, Pair, sizeof(Pair));
  assert(CrGlinkPoll(&Link) == CR_SUCCESS);
  Descriptor[0] = Descriptor[1];
  for (UINTN I = 0; I < sizeof(Message); ++I) Message[I] = (UINT8)(I * 29 + 7);
  assert(CrGlinkServiceSend(&Link, &Audio, Audio.Generation + 1, Message, sizeof(Message)) == CR_NOT_READY);
  assert(CrGlinkServiceSend(&Link, &Audio, Audio.Generation, Message, sizeof(Message)) == CR_SUCCESS);
  memset(Message, 0, sizeof(Message)); /* accepted TX owns its own copy */
  for (UINTN Iteration = 0; Iteration < 100 && (Audio.TxLength || Link.QueueCount); ++Iteration) {
    assert(CrGlinkServiceTx(&Link) == CR_SUCCESS);
    ConsumeTx();
  }
  assert(WireBytes == sizeof(Message) && Fragments == 64 && !Audio.TxLength && !Link.QueueCount);
  for (UINTN I = 0; I < sizeof(Message); ++I) assert(WireCopy[I] == (UINT8)(I * 29 + 7));
  assert(Link.Channels[1].Remote[0].Busy);
  Packet(14, 88, 0, NULL, 0);
  assert(CrGlinkPoll(&Link) == CR_SUCCESS && !Link.Channels[1].Remote[0].Busy);
  Close();
}

static VOID ReceiveAndIsolation(VOID)
{
  Start(512, TRUE);
  UINT8 Frame[24] = {0};
  Put32(Frame, 16);
  for (UINT32 I = 1; I <= 20; ++I) {
    Put32(Frame + 8, I);
    Packet(9, 88, I, Frame, sizeof(Frame));
    assert(CrGlinkPoll(&Link) == CR_SUCCESS && Calls == I && AppBytes == 16);
    assert(!memcmp(AppCopy, Frame + 8, 16));
    Descriptor[0] = Descriptor[1];
  }
  CallbackResult = CR_NOT_READY;
  Packet(9, 88, 1, Frame, sizeof(Frame));
  Packet(9, 77, 1, Frame, sizeof(Frame));
  UINT64 Epoch = Audio.Generation;
  assert(CrGlinkPoll(&Link) == CR_SUCCESS && Calls == 21 && !Audio.Online);
  assert(Audio.Generation > Epoch && ReceivedLength == 16 && Link.Ready);
  assert(Descriptor[2] == Descriptor[3]);
  Close();
}

static VOID ResetAndArguments(VOID)
{
  Start(512, FALSE);
  UINT64 Epoch = Audio.Generation;
  BOOLEAN Online = FALSE;
  UINT64 GotEpoch = 0;
  CR_STATUS Error = CR_DEVICE_ERROR;
  assert(CrGlinkServiceGetState(&Link, &Audio, &Online, &GotEpoch, &Error) == CR_SUCCESS);
  assert(Online && GotEpoch == Epoch && Error == CR_SUCCESS);
  assert(CrGlinkServiceGetState(&Link, &Audio, &Audio.Online, &GotEpoch, &Error) == CR_INVALID_PARAMETER);
  assert(CrGlinkServiceSend(&Link, &Audio, Epoch, AudioTx, 16) == CR_INVALID_PARAMETER);
  CrGlinkReset(&Link);
  assert(Audio.Registered && !Audio.Online && Audio.Generation > Epoch);
  assert(CrGlinkServiceUnregister(&Link, &Audio) == CR_SUCCESS);
  CR_GLINK_SERVICE_CONFIG C = Config(512);
  CR_GLINK Saved = Link;
  C.RxStorage = AudioTx;
  assert(CrGlinkServiceRegister(&Link, &Audio, &C) == CR_INVALID_PARAMETER);
  assert(!memcmp(&Link, &Saved, sizeof(Link)) && !Audio.Registered);
  C = Config(512);
  assert(CrGlinkServiceRegister(&Link, &Audio, &C) == CR_SUCCESS);
  assert(Audio.Generation > Epoch);
  Link.ServiceGeneration = ~(UINT64)0;
  CrGlinkReset(&Link);
  assert(Audio.Faulted && !Audio.Online && Link.ServiceGeneration == ~(UINT64)0);
  assert(CrGlinkServiceReopen(&Link, &Audio) == CR_OUT_OF_RESOURCES);
  assert(CrGlinkServiceUnregister(&Link, &Audio) == CR_SUCCESS);
  assert(CrGlinkServiceRegister(&Link, &Audio, &C) == CR_OUT_OF_RESOURCES);
}

static VOID SmemAliases(VOID)
{
  VOID *Aliases[] = {&Link, &Audio, AudioRx, AudioTx};
  for (UINT32 Item = 478; Item <= 480; ++Item) {
    for (UINTN Index = 0; Index < sizeof(Aliases) / sizeof(Aliases[0]); ++Index) {
      Start(512, FALSE);
      CrGlinkReset(&Link);
      Smem.Get = AliasedGet;
      AliasItem = Item;
      AliasAddress = Aliases[Index];
      UINT8 Before[32];
      memcpy(Before, AliasAddress, sizeof(Before));
      if (Item != 480) {
        assert(CrGlinkBind(&Link, &Smem) == CR_COMPROMISED_DATA);
        assert(!memcmp(Before, AliasAddress, sizeof(Before)));
        assert(!Link.Bound && !Link.Descriptor);
        AliasItem = 0;
        assert(CrGlinkBind(&Link, &Smem) == CR_SUCCESS);
      } else {
        assert(CrGlinkBind(&Link, &Smem) == CR_SUCCESS);
        assert(CrGlinkPoll(&Link) == CR_COMPROMISED_DATA && !Link.Rx);
      }
      CrGlinkReset(&Link);
      assert(CrGlinkServiceUnregister(&Link, &Audio) == CR_SUCCESS);
    }
  }
}

int main(VOID)
{
  LargeSend();
  ReceiveAndIsolation();
  ResetAndArguments();
  SmemAliases();
  puts("Named GLINK actual core PASS: crossed/late open,20 intents,64KiB copied TX,exact callbacks,PMIC isolation,close retention,epochs and aliases");
  return 0;
}
