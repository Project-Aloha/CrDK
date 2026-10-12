/* USB wire -> actual DWC3 UsbFunctionIo -> actual MSC application/core.
   SPDX-License-Identifier: MIT */
#define main Dwc3OriginalFixtureMain
#include "Dwc3FunctionTest.c"
#undef main
#include "../Application/UsbMscTest/UsbMscTest.c"

UINTN EFIAPI Print(CONST CHAR16 *Format, ...) { (VOID)Format; return 0; }
static MSC_APP *App;
static UINT8 ReplyBytes[512];
static UINTN ReplyLength;
static UINT8 ExpectedMaxLun, TestLun;

static void AppDrain(void)
{
  BOOLEAN Empty;
  assert(MscDrain(App, TRUE, &Empty) == EFI_SUCCESS && Empty);
}
static void HostStatus(void)
{
  UINT8 Ep = mStatusDirection;
  Event(((UINT32)Ep << 1) | (3U << 6) | (2U << 12));
  assert(CrUsbFnPoll() == EFI_SUCCESS && mEndpoints[Ep].Active);
  Finish(Ep, 0); AppDrain();
  assert(mPhase == 0 && mEndpoints[0].Active);
}
static void HostControl(UINT8 Type, UINT8 Code, UINT16 Value, UINT16 Index, UINT16 Length)
{
  EFI_USB_DEVICE_REQUEST R = {Type, Code, Value, Index, Length};
  assert(mPhase == 0 && mEndpoints[0].Active);
  memcpy(mDescriptors.Cpu, &R, sizeof(R)); Finish(0, 0); AppDrain();
  ReplyLength = 0;
  if (mPhase == 0) return; // stalled control request, next SETUP armed
  if (Length != 0) {
    assert((Type & 0x80) && mEndpoints[1].Active);
    ReplyLength = mEndpoints[1].Requested;
    assert(ReplyLength <= sizeof(ReplyBytes));
    memcpy(ReplyBytes, mEndpoints[1].Data.Cpu, ReplyLength);
    Finish(1, 0); AppDrain();
  }
  HostStatus();
}
static void ConfigureApp(UINT32 Speed)
{
  EventHead = 0; // a newly programmed event ring starts at its first entry
  assert(MscStart(App) == EFI_SUCCESS);
  Registers[0xC70C / 4] = Speed;
  Event(1U | (2U << 8)); AppDrain();
  HostControl(0x80, 6, 0x0100, 0, 64);
  assert(ReplyLength == 18 && ReplyBytes[8] == 9 && ReplyBytes[9] == 0x12);
  HostControl(0x80, 6, 0x0200, 0, 255);
  assert(ReplyLength == 32 && ReplyBytes[14] == 8 && ReplyBytes[16] == 0x50);
  assert(ReplyBytes[20] == 1 && ReplyBytes[27] == 0x81);
  assert(ReplyBytes[22] == (Speed == 1 ? 64 : 0) && ReplyBytes[23] == (Speed == 1 ? 0 : 2));
  HostControl(0x80, 6, 0x0700, 0, 255);
  assert(ReplyLength == 32 && ReplyBytes[1] == 7 && ReplyBytes[22] == (Speed == 1 ? 0 : 64));
  HostControl(0x80, 6, 0x0302, 0x409, 255);
  assert(ReplyLength == 36 && ReplyBytes[1] == 3 && ReplyBytes[2] == 'C');
  HostControl(0x80, 6, 0x0f00, 0, 5); // USB 2.0 has no BOS descriptor
  assert(mPhase == 0);
  HostControl(0, 5, 9, 0, 0); // handled by controller, not the app
  HostControl(0, 9, 1, 0, 0);
  assert(App->Configuration == 1);
  HostControl(0xA1, 0xFE, 0, 0, 1);
  assert(ReplyLength == 1 && ReplyBytes[0] == ExpectedMaxLun);
}
static void BulkOut(CONST VOID *Data, UINTN Bytes)
{
  assert(App->BulkPending && App->Action.Type == CrUsbMscBulkOut);
  assert(mEndpoints[2].Active && Bytes <= mEndpoints[2].Programmed);
  memcpy(mEndpoints[2].Data.Cpu, Data, Bytes);
  Finish(2, (UINT32)(mEndpoints[2].Programmed - Bytes)); AppDrain();
}
static void BulkIn(VOID *Data, UINTN Bytes)
{
  assert(MscPump(App) == EFI_SUCCESS && App->BulkPending);
  assert(App->Action.Type == CrUsbMscBulkIn && mEndpoints[3].Active);
  assert(mEndpoints[3].Requested == Bytes);
  if (Bytes) memcpy(Data, mEndpoints[3].Data.Cpu, Bytes);
  Finish(3, 0); AppDrain();
}
static void Cbw(UINT8 Op, UINT32 Length, BOOLEAN In, UINT32 Lba, UINT16 Blocks)
{
  UINT8 W[31] = {'U','S','B','C',0x12,0x34,0x56,0x78};
  for (UINTN I = 0; I < 4; ++I) W[8 + I] = (UINT8)(Length >> (I * 8));
  W[12] = In ? 0x80 : 0;
  W[13] = TestLun;
  W[14] = (Op == 0x12 || Op == 0x00) ? 6 : 10;
  W[15] = Op;
  if (Op == 0x12) W[19] = (UINT8)Length;
  if (Op == 0x28 || Op == 0x2a) {
    for (UINTN I = 0; I < 4; ++I) W[17 + I] = (UINT8)(Lba >> ((3 - I) * 8));
    W[22] = (UINT8)(Blocks >> 8); W[23] = (UINT8)Blocks;
  }
  assert(MscPump(App) == EFI_SUCCESS && App->Action.Length == 31);
  BulkOut(W, sizeof(W));
}
static void CswOk(void)
{
  UINT8 W[13]; BulkIn(W, sizeof(W));
  assert(!memcmp(W, "USBS\x12\x34\x56\x78", 8));
  assert(W[8] == 0 && W[9] == 0 && W[10] == 0 && W[11] == 0 && W[12] == 0);
}
static UINT8 BlockData[2][65536];
static UINTN BlockReads[2], BlockWrites[2], BlockFlushes[2];
static UINTN WhichBlock(VOID *C,UINT64 Lba,UINT32 Blocks)
{
  UINTN I=C==BlockData[0]?0:1;
  assert(C==BlockData[I]&&Lba==(I?200:100)&&Blocks==(I?128:16));return I;
}
static CR_STATUS BlockRead(VOID *C,UINT64 Lba,UINT32 Blocks,VOID *Data)
{ UINTN I=WhichBlock(C,Lba,Blocks);memcpy(Data,BlockData[I],65536);++BlockReads[I];return CR_SUCCESS; }
static CR_STATUS BlockWrite(VOID *C,UINT64 Lba,UINT32 Blocks,CONST VOID *Data)
{ UINTN I=WhichBlock(C,Lba,Blocks);memcpy(BlockData[I],Data,65536);++BlockWrites[I];return CR_SUCCESS; }
static CR_STATUS BlockFlush(VOID *C)
{ UINTN I=C==BlockData[0]?0:1;assert(C==BlockData[I]);++BlockFlushes[I];return CR_SUCCESS; }
int main(void)
{
  UINT8 Data[512], Result[512];
  Fixture();
  App = calloc(1, sizeof(*App)); assert(App);
  App->Io = &gCrUsbFunctionIo;
  VOID *RamDisk = malloc(CR_USB_MSC_DISK_BYTES); assert(RamDisk);
  App->Control = malloc(MSC_CONTROL_BYTES); App->Bulk = malloc(CR_USB_MSC_TRANSFER_MAX);
  assert(App->Control && App->Bulk);
  assert(CrUsbMscInit(&App->Core, RamDisk, CR_USB_MSC_DISK_BYTES) == CR_SUCCESS);
  assert(CrUsbCoreInit(UsbTypeCDevice, FALSE) == EFI_SUCCESS);
  CrUsbFnSetAvailable(TRUE); AppDrain(); ConfigureApp(0);
  Cbw(0x12, 36, TRUE, 0, 0); BulkIn(Result, 36);
  assert(!memcmp(Result + 8, "CRANE   ", 8)); CswOk();
  Cbw(0x25, 8, TRUE, 0, 0); BulkIn(Result, 8);
  assert(!memcmp(Result, "\0\0\x7f\xff\0\0\2\0", 8)); CswOk();
  for (UINTN I = 0; I < sizeof(Data); ++I) Data[I] = (UINT8)(I ^ 0xA5);
  Cbw(0x2a, 512, FALSE, 8000, 1); assert(MscPump(App) == EFI_SUCCESS);
  BulkOut(Data, sizeof(Data)); CswOk();
  Cbw(0x28, 512, TRUE, 8000, 1); BulkIn(Result, 512);
  assert(!memcmp(Data, Result, 512)); CswOk();

  // A bus reset while waiting for CBW aborts the native transfer. The app
  // must drain its old completion before reusing the same client buffer.
  assert(MscPump(App) == EFI_SUCCESS && App->BulkPending);
  Event(1U | (1U << 8)); AppDrain();
  assert(!App->BulkPending && App->Configuration == 0);
  Event(1U | (2U << 8)); AppDrain(); HostControl(0, 9, 1, 0, 0);
  Cbw(0x28, 512, TRUE, 8000, 1); BulkIn(Result, 512);
  assert(!memcmp(Data, Result, 512)); CswOk();

  // Invalid CBW must halt both pipes, and a lone clear must not bypass BOT reset.
  assert(MscPump(App) == EFI_SUCCESS); memset(Data, 0, 31); BulkOut(Data, 31);
  assert(MscPump(App) == EFI_SUCCESS); assert(MscPump(App) == EFI_SUCCESS);
  assert(mEndpoints[2].Stalled && mEndpoints[3].Stalled);
  HostControl(2, 1, 0, 0x81, 0); assert(mEndpoints[3].Stalled);
  HostControl(0x21, 0xff, 0, 0, 0);
  HostControl(2, 1, 0, 0x81, 0); HostControl(2, 1, 0, 1, 0);
  Cbw(0, 0, FALSE, 0, 0); CswOk();

  // Reset during an outstanding DATA OUT; no stale completion becomes a CBW.
  Cbw(0x2a, 512, FALSE, 8001, 1); assert(MscPump(App) == EFI_SUCCESS);
  HostControl(0x21, 0xff, 0, 0, 0);
  HostControl(2, 1, 0, 0x81, 0); HostControl(2, 1, 0, 1, 0);
  Cbw(0, 0, FALSE, 0, 0); CswOk();

  assert(MscPump(App) == EFI_SUCCESS); CrUsbFnSetAvailable(FALSE); AppDrain();
  assert(!App->Started && !App->Touched && Allocations == 0 && Mappings == 0);
  assert(MscStart(App) == EFI_NOT_READY);
  CrUsbFnSetAvailable(TRUE); AppDrain(); ConfigureApp(1); // full-speed reconnect
  Cbw(0x28, 512, TRUE, 8000, 1); BulkIn(Result, 512); CswOk();
  for (UINTN I = 0; I < sizeof(Result); ++I) assert(Result[I] == (UINT8)(I ^ 0xA5));
  assert(MscPump(App) == EFI_SUCCESS);
  FailHalt = TRUE; assert(MscStop(App) == EFI_TIMEOUT && App->Touched && Allocations != 0);
  FailHalt = FALSE; assert(MscStop(App) == EFI_SUCCESS); AppDrain();
  assert(Allocations == 0 && Mappings == 0);
  // Restart the real DWC3/app with independent 4KiB UFS and 512-byte SD media.
  // Writable fixture backends exercise transport routing; the app adapter's
  // production read-only policy is covered by UsbMscBlockAdapterTest.
  memset(&App->Core,0,sizeof(App->Core));
  CR_USB_MSC_BLOCK_BACKEND Backends[2]={
    {BlockData[0],4096,0x100000010ULL,FALSE,BlockRead,BlockWrite,BlockFlush,NULL},
    {BlockData[1],512,0x1ffffULL,FALSE,BlockRead,BlockWrite,BlockFlush,NULL}};
  assert(CrUsbMscInitBlocks(&App->Core,Backends,2)==CR_SUCCESS);
  ExpectedMaxLun=1;ConfigureApp(0);
  UINT8 *Big=malloc(65536),*BigResult=malloc(65536);assert(Big&&BigResult);
  for(UINTN I=0;I<65536;++I)Big[I]=(UINT8)(I^0x6B);
  Cbw(0x2a,65536,FALSE,100,16);assert(MscPump(App)==EFI_SUCCESS);
  assert(!BlockWrites[0]);BulkOut(Big,65536);CswOk();assert(BlockWrites[0]==1&&BlockFlushes[0]==1);
  Cbw(0x28,65536,TRUE,100,16);BulkIn(BigResult,65536);CswOk();
  assert(BlockReads[0]==1&&!memcmp(Big,BigResult,65536));
  Cbw(0x25,8,TRUE,0,0);BulkIn(Result,8);CswOk();
  assert(!memcmp(Result,"\xff\xff\xff\xff\0\0\x10\0",8));
  TestLun=1;
  for(UINTN I=0;I<65536;++I)Big[I]=(UINT8)(I^0xD3);
  Cbw(0x2a,65536,FALSE,200,128);assert(MscPump(App)==EFI_SUCCESS);
  BulkOut(Big,65536);CswOk();assert(BlockWrites[1]==1&&BlockFlushes[1]==1);
  Cbw(0x28,65536,TRUE,200,128);BulkIn(BigResult,65536);CswOk();
  assert(BlockReads[1]==1&&!memcmp(Big,BigResult,65536));
  Cbw(0x25,8,TRUE,0,0);BulkIn(Result,8);CswOk();
  assert(!memcmp(Result,"\0\1\xff\xff\0\0\2\0",8));
  // Reset an in-flight SD transfer; the UFS contents and routing survive it.
  Cbw(0x2a,65536,FALSE,200,128);assert(MscPump(App)==EFI_SUCCESS);
  HostControl(0x21,0xff,0,0,0);
  HostControl(2,1,0,0x81,0);HostControl(2,1,0,1,0);
  assert(BlockWrites[1]==1);
  TestLun=0;Cbw(0x28,65536,TRUE,100,16);BulkIn(BigResult,65536);CswOk();
  assert(BlockReads[0]==2);
  for(UINTN I=0;I<65536;++I)assert(BigResult[I]==(UINT8)(I^0x6B));
  // Nonexistent LUN is a SCSI inquiry response, not a transport reset.
  TestLun=2;Cbw(0x12,36,TRUE,0,0);BulkIn(Result,36);CswOk();assert(Result[0]==0x7f);
  TestLun=1;Cbw(0x28,65536,TRUE,200,128);BulkIn(BigResult,65536);CswOk();
  assert(BlockReads[1]==2&&!memcmp(Big,BigResult,65536));
  TestLun=0;
  // Host-short WRITE is a BOT phase error and must not partially change disk.
  Cbw(0x2a,4096,FALSE,100,16);assert(MscPump(App)==EFI_SUCCESS);BulkOut(Big,4096);
  BulkIn(Result,13);assert(Result[12]==2&&BlockWrites[0]==1&&BlockWrites[1]==1);
  assert(MscStop(App)==EFI_SUCCESS);AppDrain();free(Big);free(BigResult);
  free(RamDisk); free(App->Control); free(App->Bulk); free(App);
  puts("MSC app: actual DWC3 HS/FS, dual-LUN 4KiB/512-byte routing, reset isolation, replug and retained halt failure PASS");
  return 0;
}
