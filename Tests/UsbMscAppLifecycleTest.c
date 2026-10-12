/* Actual UefiMain + BOT core, with independent EFI lifecycle failure model.
 * No DWC3/register or physical USB claim. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MSC_APP_SOURCE
#define MSC_APP_SOURCE "../Application/UsbMscTest/UsbMscTest.c"
#endif
#include MSC_APP_SOURCE

typedef enum {
  Normal, ExclusiveDenied, LocateFailed, DiskFailed, ControlFailed, BulkFailed,
  MaxFailed, SmallMax, PolicyFailed, StartRetry, StartPartial, ConfigureFailed,
  InitialStopFailed, FinalStopFailed, DrainFailed, DrainBusy,
  ControlFreeFailed, BulkFreeFailed, ControlReset, ControlDetach,
  UfsOnly, NoSdNode, UfsDenied, SdDenied, MissingUfs, SdInvalid, AppAllocationFailed
} TEST_CASE;
typedef struct { VOID *P; UINTN Bytes; BOOLEAN Live; } ALLOCATION;
typedef struct { EFI_USBFN_MESSAGE Message; UINTN Size; EFI_USBFN_MESSAGE_PAYLOAD Payload; } EVENT;
static EFI_BOOT_SERVICES Services;
static EFI_SYSTEM_TABLE System;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;
EFI_BOOT_SERVICES *gBS = &Services;
EFI_SYSTEM_TABLE *gST = &System;
EFI_GUID gEfiUsbFunctionIoProtocolGuid, gEfiBlockIoProtocolGuid, gEfiDevicePathProtocolGuid;
static EFI_USBFN_IO_PROTOCOL Io;
static TEST_CASE Case;
static EFI_BLOCK_IO_PROTOCOL DiskIo[2];
static EFI_BLOCK_IO_MEDIA DiskMedia[2];
static UINT8 DiskPath[2][64];
static UINTN DiskOpens[2],DiskCloses[2],DiskReconnects[2],DiskFlushes[2];
static UINTN ReleaseOrder[16],ReleaseCount;
static BOOLEAN DiskHeld[2];
static const EFI_HANDLE DiskHandle[2]={(EFI_HANDLE)(UINTN)0x333,(EFI_HANDLE)(UINTN)0x334};
static UINTN DiskIndex(EFI_HANDLE H){assert(H==DiskHandle[0]||H==DiskHandle[1]);return H==DiskHandle[0]?0:1;}
static ALLOCATION Pools[32], Buffers[8];
static EVENT Events[64];
static UINTN PoolCount, BufferCount, Head, Tail, KeyCalls, Ticks, Opens, Closes;
static UINTN Starts, Stops, Configures, Transfers, Frees, Drains, PolicyCalls;
static UINTN StartCleanup, ControlAborts, Interruptions, ControlStatusAfterReset;
static BOOLEAN Exclusive, Dirty, Running, StoppedSession, QueueDrained, Interrupted;
static VOID *Pending[4];
static UINTN PendingBytes[4];
static const EFI_HANDLE Image = (EFI_HANDLE)(UINTN)0x111;
static const EFI_HANDLE Controller = (EFI_HANDLE)(UINTN)0x222;

VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D, 0, N); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
UINTN EFIAPI Print(CONST CHAR16 *Format, ...) { (VOID)Format; return 0; }
VOID EFIAPI DebugAssert(CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description)
{ fprintf(stderr,"UEFI assertion %s:%zu %s\n",File,(size_t)Line,Description); abort(); }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
static VOID *Pool(UINTN N, BOOLEAN Clear)
{
  if ((Case==DiskFailed && N==CR_USB_MSC_TRANSFER_MAX+4095) || (Case==AppAllocationFailed && N==sizeof(MSC_APP))) return NULL;
  assert(PoolCount < ARRAY_SIZE(Pools));
  VOID *P = Clear ? calloc(1,N) : malloc(N); assert(P);
  Pools[PoolCount++] = (ALLOCATION){P,N,TRUE}; return P;
}
VOID *EFIAPI AllocatePool(UINTN N) { return Pool(N,FALSE); }
VOID *EFIAPI AllocateZeroPool(UINTN N) { return Pool(N,TRUE); }
VOID EFIAPI FreePool(VOID *P)
{
  for (UINTN I=0;I<PoolCount;++I) if (Pools[I].P==P && Pools[I].Live) {
    Pools[I].Live=FALSE; free(P); return;
  }
  assert(!"unknown or repeated pool free");
}
static void CheckOwner(EFI_USBFN_IO_PROTOCOL *This)
{ assert(This==&Io && Exclusive); }
static void Queue(EFI_USBFN_MESSAGE M, UINTN Size, CONST VOID *Payload)
{
  assert(Tail-Head<ARRAY_SIZE(Events)); EVENT *E=&Events[Tail++%ARRAY_SIZE(Events)];
  memset(E,0,sizeof(*E)); E->Message=M; E->Size=Size;
  if (Size) memcpy(&E->Payload,Payload,Size); QueueDrained=FALSE;
}
static void Finish(UINTN Ep, EFI_USBFN_TRANSFER_STATUS Status)
{
  if (!Pending[Ep]) return;
  EFI_USBFN_TRANSFER_RESULT R={0}; R.Buffer=Pending[Ep]; R.BytesTransferred=Status==UsbTransferStatusComplete?PendingBytes[Ep]:0;
  R.TransferStatus=Status; R.EndpointIndex=(UINT8)(Ep/2);
  R.Direction=(Ep&1)?EfiUsbEndpointDirectionDeviceTx:EfiUsbEndpointDirectionDeviceRx;
  Queue((Ep&1)?EfiUsbMsgEndpointStatusChangedTx:EfiUsbMsgEndpointStatusChangedRx,sizeof(R),&R);
  Pending[Ep]=NULL;
}
static void Setup(UINT8 Type,UINT8 Request,UINT16 Value,UINT16 Length)
{ EFI_USB_DEVICE_REQUEST R={Type,Request,Value,0,Length}; Queue(EfiUsbMsgSetupPacket,sizeof(R),&R); }
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **Handles)
{
  assert(Type==ByProtocol&&!Key);
  if(Guid==&gEfiBlockIoProtocolGuid){
    *Handles=Pool(2*sizeof(**Handles),FALSE);(*Handles)[0]=DiskHandle[1];(*Handles)[1]=DiskHandle[0];*Count=2;return EFI_SUCCESS;
  }
  assert(Guid==&gEfiUsbFunctionIoProtocolGuid);
  if (Case==LocateFailed) return EFI_NOT_FOUND;
  *Handles=Pool(sizeof(**Handles),FALSE); (*Handles)[0]=Controller; *Count=1; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **Interface,EFI_HANDLE Agent,EFI_HANDLE Child,UINT32 Attributes)
{
  if(G==&gEfiBlockIoProtocolGuid){
    UINTN I=DiskIndex(H);assert(Agent==Image&&!Child&&Attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&!DiskHeld[I]);
    assert(!Exclusive);++DiskOpens[I];
    if((Case==UfsDenied&&I==0)||(Case==SdDenied&&I==1))return EFI_ACCESS_DENIED;
    DiskHeld[I]=TRUE;*Interface=&DiskIo[I];return EFI_SUCCESS;
  }
  assert(H==Controller&&G==&gEfiUsbFunctionIoProtocolGuid&&Agent==Image&&!Child);
  assert(Attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE); ++Opens;
  if (Case==ExclusiveDenied||Exclusive) return EFI_ACCESS_DENIED;
  Exclusive=TRUE; *Interface=&Io; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE Agent,EFI_HANDLE Child)
{
  if(G==&gEfiBlockIoProtocolGuid){
    UINTN I=DiskIndex(H);assert(Agent==Image&&!Child&&DiskHeld[I]&&(!Running||Case==FinalStopFailed)&&!Exclusive);
    if(I==0)assert(!DiskHeld[1]);
    if(Starts)assert(Stops>=2&&Closes>0);
    if(Starts&&Case!=InitialStopFailed&&Case!=FinalStopFailed&&Case!=DrainFailed&&Case!=DrainBusy)assert(QueueDrained&&Head==Tail);
    assert(ReleaseCount<ARRAY_SIZE(ReleaseOrder));ReleaseOrder[ReleaseCount++]=I;
    DiskHeld[I]=FALSE;++DiskCloses[I];return EFI_SUCCESS;
  }
  assert(H==Controller&&G==&gEfiUsbFunctionIoProtocolGuid&&Agent==Image&&!Child&&Exclusive);
  Exclusive=FALSE; ++Closes; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *K)
{
  assert(This==&Input); ++KeyCalls;
  UINTN Limit=Case==StartRetry?405:Case==ControlDetach?210:6;
  if(KeyCalls>Limit) {K->ScanCode=SCAN_ESC;K->UnicodeChar=0;return EFI_SUCCESS;}
  return EFI_NOT_READY;
}
static EFI_STATUS EFIAPI Stall(UINTN Us) {assert(Us==1000);++Ticks;assert(Ticks<1000);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Max(EFI_USBFN_IO_PROTOCOL *This,UINTN *Size)
{ CheckOwner(This);if(Case==MaxFailed)return EFI_DEVICE_ERROR;*Size=Case==SmallMax?512:CR_USB_MSC_TRANSFER_MAX;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Allocate(EFI_USBFN_IO_PROTOCOL *This,UINTN N,VOID **P)
{
  CheckOwner(This);assert(N==MSC_CONTROL_BYTES||N==CR_USB_MSC_TRANSFER_MAX);
  *P=NULL;if((Case==ControlFailed&&N==MSC_CONTROL_BYTES)||(Case==BulkFailed&&N==CR_USB_MSC_TRANSFER_MAX))return EFI_OUT_OF_RESOURCES;
  assert(BufferCount<ARRAY_SIZE(Buffers));*P=malloc(N);assert(*P);
  Buffers[BufferCount++]=(ALLOCATION){*P,N,TRUE};return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free(EFI_USBFN_IO_PROTOCOL *This,VOID *P)
{
  CheckOwner(This);assert(!Dirty&&!Running&&QueueDrained&&Head==Tail);++Frees;
  for(UINTN E=0;E<ARRAY_SIZE(Pending);++E)assert(Pending[E]!=P);
  for(UINTN I=0;I<BufferCount;++I)if(Buffers[I].P==P&&Buffers[I].Live){
    if((Case==ControlFreeFailed&&Buffers[I].Bytes==MSC_CONTROL_BYTES)||(Case==BulkFreeFailed&&Buffers[I].Bytes==CR_USB_MSC_TRANSFER_MAX))return EFI_DEVICE_ERROR;
    Buffers[I].Live=FALSE;free(P);return EFI_SUCCESS;
  }
  assert(!"unknown transfer buffer free");return EFI_INVALID_PARAMETER;
}
static EFI_STATUS EFIAPI Stop(EFI_USBFN_IO_PROTOCOL *This)
{
  CheckOwner(This);++Stops;
  if((Case==InitialStopFailed&&Starts==0)||(Case==FinalStopFailed&&Starts))return EFI_TIMEOUT;
  if(Starts) {++StartCleanup;StoppedSession=TRUE;}
  Dirty=Running=FALSE;for(UINTN E=0;E<ARRAY_SIZE(Pending);++E)Finish(E,UsbTransferStatusAborted);
  QueueDrained=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Start(EFI_USBFN_IO_PROTOCOL *This)
{
  CheckOwner(This);assert(!Dirty&&!Running);++Starts;
  if(Case==StartRetry&&Starts<3)return EFI_NOT_READY;
  Dirty=TRUE;
  if(Case==StartPartial)return EFI_DEVICE_ERROR; /* DMA allocation rollback failed. */
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Policy(EFI_USBFN_IO_PROTOCOL *This,UINT8 Ep,EFI_USBFN_ENDPOINT_DIRECTION D,EFI_USBFN_POLICY_TYPE Type,UINTN Size,VOID *Value)
{
  CheckOwner(This);++PolicyCalls;assert(!Dirty&&!Running&&Ep==1&&D==EfiUsbEndpointDirectionDeviceTx&&Type==EfiUsbPolicyZeroLengthTermination&&Size==sizeof(BOOLEAN)&&!*(BOOLEAN *)Value);
  return Case==PolicyFailed?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Configure(EFI_USBFN_IO_PROTOCOL *This,EFI_USB_DEVICE_INFO *Info)
{
  CheckOwner(This);assert(Dirty&&Info==&mMscInfo);++Configures;Running=TRUE;
  if(Case==ConfigureFailed)return EFI_DEVICE_ERROR;
  EFI_USB_BUS_SPEED Speed=UsbBusSpeedHigh;Queue(EfiUsbMsgBusEventSpeed,sizeof(Speed),&Speed);
  if((Case==ControlReset||Case==ControlDetach)&&!Interrupted)Setup(0x80,6,0x0100,18);
  else Setup(0,9,1,0);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Transfer(EFI_USBFN_IO_PROTOCOL *This,UINT8 Ep,EFI_USBFN_ENDPOINT_DIRECTION D,UINTN *N,VOID *P)
{
  CheckOwner(This);assert(Running&&Ep<=1);++Transfers;UINTN E=Ep*2+(D==EfiUsbEndpointDirectionDeviceTx);
  assert(!Pending[E]);BOOLEAN Known=FALSE;
  for(UINTN I=0;I<BufferCount;++I)if(Buffers[I].Live&&Buffers[I].P==P&&*N<=Buffers[I].Bytes)Known=TRUE;
  assert(Known);Pending[E]=P;PendingBytes[E]=*N;
  if(Ep==0){
    if(Interrupted&&D==EfiUsbEndpointDirectionDeviceRx)++ControlStatusAfterReset;
    if(!((Case==ControlReset||Case==ControlDetach)&&!Interrupted&&*N==18))Finish(E,UsbTransferStatusComplete);
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Abort(EFI_USBFN_IO_PROTOCOL *This,UINT8 Ep,EFI_USBFN_ENDPOINT_DIRECTION D)
{CheckOwner(This);assert(Ep<=1);Finish(Ep*2+(D==EfiUsbEndpointDirectionDeviceTx),UsbTransferStatusAborted);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI SetStall(EFI_USBFN_IO_PROTOCOL *This,UINT8 Ep,EFI_USBFN_ENDPOINT_DIRECTION D,BOOLEAN *Halt)
{CheckOwner(This);assert(Ep<=1&&Halt);(VOID)D;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI GetStall(EFI_USBFN_IO_PROTOCOL *This,UINT8 Ep,EFI_USBFN_ENDPOINT_DIRECTION D,BOOLEAN *Halt)
{CheckOwner(This);assert(Ep<=1&&Halt);(VOID)D;*Halt=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI EventHandler(EFI_USBFN_IO_PROTOCOL *This,EFI_USBFN_MESSAGE *M,UINTN *Size,EFI_USBFN_MESSAGE_PAYLOAD *P)
{
  CheckOwner(This);++Drains;assert(*Size>=sizeof(*P));
  if(Case==StartPartial&&Dirty)return EFI_DEVICE_ERROR;
  if(StoppedSession&&Case==DrainFailed)return EFI_DEVICE_ERROR;
  if(StoppedSession&&Case==DrainBusy){*M=EfiUsbMsgBusEventAttach;*Size=0;return EFI_SUCCESS;}
  if(!Interrupted&&(Case==ControlReset||Case==ControlDetach)&&Pending[1]){
    Interrupted=TRUE;++Interruptions;
    Queue(Case==ControlReset?EfiUsbMsgBusEventReset:EfiUsbMsgBusEventDetach,0,NULL);
    if(Case==ControlReset){Finish(1,UsbTransferStatusAborted);Setup(0,9,1,0);}
  }
  if(Head==Tail){*M=EfiUsbMsgNone;*Size=0;QueueDrained=TRUE;return EFI_SUCCESS;}
  EVENT *E=&Events[Head++%ARRAY_SIZE(Events)];*M=E->Message;*Size=E->Size;*P=E->Payload;
  if((E->Message==EfiUsbMsgEndpointStatusChangedTx||E->Message==EfiUsbMsgEndpointStatusChangedRx)&&E->Payload.utr.EndpointIndex==0&&E->Payload.utr.TransferStatus==UsbTransferStatusAborted)++ControlAborts;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI HandleProtocol(EFI_HANDLE H, EFI_GUID *G, VOID **P)
{
  UINTN I=DiskIndex(H);
  if(G==&gEfiBlockIoProtocolGuid)*P=&DiskIo[I];
  else {assert(G==&gEfiDevicePathProtocolGuid);*P=DiskPath[I];}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI DiskRead(EFI_BLOCK_IO_PROTOCOL *This,UINT32 Id,EFI_LBA Lba,UINTN Bytes,VOID *Data)
{(VOID)This;(VOID)Id;(VOID)Lba;(VOID)Bytes;(VOID)Data;assert(!"unexpected disk data request");return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI DiskFlush(EFI_BLOCK_IO_PROTOCOL *This)
{UINTN I=(UINTN)(This-DiskIo);assert(I<2&&DiskHeld[I]);++DiskFlushes[I];return EFI_SUCCESS;}
static EFI_STATUS EFIAPI DiskReconnect(EFI_HANDLE H,EFI_HANDLE *Drivers,EFI_DEVICE_PATH_PROTOCOL *P,BOOLEAN Recursive)
{UINTN I=DiskIndex(H);assert(!Drivers&&!P&&Recursive&&!DiskHeld[I]&&!Exclusive&&(!Running||Case==FinalStopFailed));++DiskReconnects[I];return EFI_SUCCESS;}
static void Initialize(TEST_CASE C)
{
  assert(!Exclusive);memset(Pools,0,sizeof(Pools));memset(Buffers,0,sizeof(Buffers));memset(Pending,0,sizeof(Pending));
  PoolCount=BufferCount=Head=Tail=KeyCalls=Ticks=Opens=Closes=Starts=Stops=Configures=Transfers=Frees=Drains=PolicyCalls=0;
  StartCleanup=ControlAborts=Interruptions=ControlStatusAfterReset=0;
  Dirty=Running=StoppedSession=QueueDrained=Interrupted=FALSE;Case=C;
  Services.HandleProtocol=HandleProtocol;Services.LocateHandleBuffer=Locate;Services.OpenProtocol=Open;Services.CloseProtocol=Close;Services.Stall=Stall;
  Services.ConnectController=DiskReconnect;
  memset(DiskHeld,0,sizeof(DiskHeld));memset(DiskOpens,0,sizeof(DiskOpens));memset(DiskCloses,0,sizeof(DiskCloses));
  memset(DiskReconnects,0,sizeof(DiskReconnects));memset(DiskFlushes,0,sizeof(DiskFlushes));ReleaseCount=0;
  memset(DiskIo,0,sizeof(DiskIo));memset(DiskMedia,0,sizeof(DiskMedia));memset(DiskPath,0,sizeof(DiskPath));
  for(UINTN I=0;I<2;++I){
    DiskMedia[I].MediaPresent=TRUE;DiskMedia[I].MediaId=2+I;DiskMedia[I].BlockSize=I?512:4096;
    DiskMedia[I].IoAlign=4096;DiskMedia[I].LastBlock=I?65535:0x123456789ULL;
    DiskIo[I].Media=&DiskMedia[I];DiskIo[I].ReadBlocks=DiskIo[I].WriteBlocks=DiskRead;DiskIo[I].FlushBlocks=DiskFlush;
  }
  UFS_DEVICE_PATH Ufs={{MESSAGING_DEVICE_PATH,MSG_UFS_DP,{sizeof(UFS_DEVICE_PATH),0}},0,0};
  SD_DEVICE_PATH Sd={{MESSAGING_DEVICE_PATH,MSG_SD_DP,{sizeof(SD_DEVICE_PATH),0}},0};
  EFI_DEVICE_PATH_PROTOCOL End={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
  memcpy(DiskPath[0],&Ufs,sizeof(Ufs));memcpy(DiskPath[0]+sizeof(Ufs),&End,sizeof(End));
  memcpy(DiskPath[1],&Sd,sizeof(Sd));memcpy(DiskPath[1]+sizeof(Sd),&End,sizeof(End));
  if(C==UfsOnly)DiskMedia[1].MediaPresent=FALSE;
  if(C==NoSdNode)DiskPath[1][0]=MEDIA_DEVICE_PATH;
  if(C==MissingUfs)((UFS_DEVICE_PATH *)DiskPath[0])->Lun=1;
  if(C==SdInvalid)DiskMedia[1].IoAlign=3;
  Input.ReadKeyStroke=Key;System.ConIn=&Input;
  memset(&Io,0,sizeof(Io));Io.Revision=EFI_USBFN_IO_PROTOCOL_REVISION;
  Io.GetMaxTransferSize=Max;Io.AllocateTransferBuffer=Allocate;Io.FreeTransferBuffer=Free;Io.StartController=Start;Io.StopController=Stop;
  Io.SetEndpointPolicy=Policy;Io.ConfigureEnableEndpoints=Configure;Io.EventHandler=EventHandler;Io.Transfer=Transfer;
  Io.AbortTransfer=Abort;Io.SetEndpointStallState=SetStall;Io.GetEndpointStallState=GetStall;
}
static void Run(TEST_CASE C)
{
  Initialize(C);EFI_STATUS S=UefiMain(Image,&System);UINTN Live=0;
  for(UINTN I=0;I<PoolCount;++I)assert(!Pools[I].Live);
  for(UINTN I=0;I<BufferCount;++I)Live+=Buffers[I].Live;
  assert(!Exclusive);
  switch(C){
    case UfsOnly:case NoSdNode:
      assert(S==EFI_SUCCESS&&Starts==1&&!Live&&!DiskHeld[0]&&!DiskHeld[1]);
      assert(DiskOpens[0]==1&&DiskCloses[0]==1&&DiskReconnects[0]==1&&!DiskOpens[1]);break;
    case UfsDenied:assert(S==EFI_ACCESS_DENIED&&!Starts&&!Opens&&DiskOpens[0]==1&&!DiskHeld[0]&&DiskReconnects[0]==1&&!DiskOpens[1]);break;
    case SdDenied:assert(S==EFI_ACCESS_DENIED&&!Starts&&!Opens&&DiskOpens[0]==1&&DiskCloses[0]==1&&DiskOpens[1]==1&&!DiskCloses[1]&&DiskReconnects[1]==1);break;
    case MissingUfs:assert(S==EFI_NOT_FOUND&&!Starts&&!Opens&&!DiskOpens[0]&&!DiskOpens[1]);break;
    case SdInvalid:assert(S==EFI_UNSUPPORTED&&!Starts&&!Opens&&DiskCloses[0]==1&&!DiskOpens[1]);break;
    case AppAllocationFailed:assert(S==EFI_OUT_OF_RESOURCES&&!PoolCount&&!Starts&&!Opens&&!DiskOpens[0]&&!DiskOpens[1]);break;
    case Normal:assert(S==EFI_SUCCESS&&Starts==1&&StartCleanup==1&&Transfers>=2&&!Live);break;
    case ExclusiveDenied:assert(S==EFI_ACCESS_DENIED&&Opens==1&&!Closes&&!Drains&&!Starts&&!BufferCount);break;
    case LocateFailed:assert(S==EFI_NOT_FOUND&&!Opens&&!Closes);break;
    case DiskFailed:case ControlFailed:case BulkFailed:assert(S==EFI_OUT_OF_RESOURCES&&!Starts&&!Live);break;
    case MaxFailed:assert(S==EFI_DEVICE_ERROR&&!Starts&&!BufferCount);break;
    case SmallMax:assert(S==EFI_UNSUPPORTED&&!Starts&&!BufferCount);break;
    case PolicyFailed:assert(S==EFI_DEVICE_ERROR&&!Starts&&!Live);break;
    case StartRetry:assert(S==EFI_SUCCESS&&Starts==3&&Configures==1&&!Live);break;
    case StartPartial:assert(S==EFI_DEVICE_ERROR&&Starts==1&&StartCleanup>=1&&!Dirty&&!Live);break;
    case ConfigureFailed:assert(S==EFI_DEVICE_ERROR&&Configures==1&&StartCleanup>=1&&!Live);break;
    case InitialStopFailed:assert(S==EFI_TIMEOUT&&Stops>=2&&!Starts&&Live==2&&!Frees);break;
    case FinalStopFailed:assert(S==EFI_TIMEOUT&&Dirty&&Live==2&&!Frees);break;
    case DrainFailed:assert(S==EFI_DEVICE_ERROR&&!Dirty&&Live==2&&!Frees);break;
    case DrainBusy:assert(S==EFI_NOT_READY&&!Dirty&&Live==2&&!Frees);break;
    case ControlFreeFailed:assert(S==EFI_DEVICE_ERROR&&!Dirty&&Live==2&&Frees==1);break;
    case BulkFreeFailed:assert(S==EFI_DEVICE_ERROR&&!Dirty&&Live==1&&Frees==2);break;
    case ControlReset:assert(S==EFI_SUCCESS&&Interruptions==1&&ControlAborts==1&&!ControlStatusAfterReset&&Starts==1&&!Live);break;
    case ControlDetach:assert(S==EFI_SUCCESS&&Interruptions==1&&ControlAborts==1&&!ControlStatusAfterReset&&Starts==2&&!Live);break;
  }
  assert(!DiskHeld[0]&&!DiskHeld[1]);
  if(C<=ControlDetach&&C!=DiskFailed){
    assert(DiskOpens[0]==1&&DiskOpens[1]==1&&DiskCloses[0]==1&&DiskCloses[1]==1);
    assert(ReleaseCount==2&&ReleaseOrder[0]==1&&ReleaseOrder[1]==0);
    assert(DiskFlushes[0]==1&&DiskFlushes[1]==1);
  }
  /* Even when Stop fails, storage operations are synchronous and the USB
     provider has no callbacks into the app/backend. Disks are released after
     the stop attempt and USB close; only USB DMA bounce buffers remain live.
     Complete a modeled late DMA write to prove those pointers are retained
     and disjoint from the freed app context and block bounce allocations. */
  for(UINTN E=0;E<ARRAY_SIZE(Pending);++E)if(Pending[E]){
    BOOLEAN Retained=FALSE;
    for(UINTN I=0;I<BufferCount;++I)if(Buffers[I].P==Pending[E]&&Buffers[I].Live)Retained=TRUE;
    assert(Retained);memset(Pending[E],0xA5,PendingBytes[E]);
  }
  /* Model teardown after recording retained ownership; never credit this as
     application cleanup. A real provider must retain unsafe client buffers. */
  for(UINTN I=0;I<BufferCount;++I)if(Buffers[I].Live)free(Buffers[I].P);
  Dirty=Running=FALSE;
  printf("lifecycle case %u PASS status=%llx starts=%zu stops=%zu retained=%zu\n",C,(unsigned long long)S,(size_t)Starts,(size_t)Stops,(size_t)Live);
}
static void SequentialEntries(void)
{
  Initialize(Normal);
  for(UINTN Entry=0;Entry<3;++Entry){
    KeyCalls=0;
    assert(UefiMain(Image,&System)==EFI_SUCCESS);
    assert(!Exclusive&&!Dirty&&!Running&&QueueDrained&&Head==Tail);
    for(UINTN I=0;I<PoolCount;++I)assert(!Pools[I].Live);
    for(UINTN I=0;I<BufferCount;++I)assert(!Buffers[I].Live);
    for(UINTN I=0;I<ARRAY_SIZE(Pending);++I)assert(!Pending[I]);
    assert(Starts==Entry+1&&Stops==2*(Entry+1)&&Closes==Entry+1);
    assert(DiskCloses[0]==Entry+1&&DiskCloses[1]==Entry+1&&!DiskHeld[0]&&!DiskHeld[1]);
    assert(ReleaseOrder[Entry*2]==1&&ReleaseOrder[Entry*2+1]==0);
  }
  puts("Three sequential UefiMain entries on the same provider, no fixture reset, PASS");
}
int main(int Argc,char **Argv)
{
  if(Argc==2){Run((TEST_CASE)strtoul(Argv[1],NULL,10));return 0;}
  for(TEST_CASE C=Normal;C<=AppAllocationFailed;++C)Run(C);
  SequentialEntries();
  puts("Actual MSC UefiMain lifecycle: exclusive ownership, partial failures, drain/free, repeated entry, EP0 reset/replug PASS");return 0;
}
