/* Actual automatic EFI block adapter; modeled protocols, no real disks.
 * SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../Application/UsbMscTest/UsbMscBlock.c"

#define DEVICES 8U
#define UFS0 4U
#define SD 1U
EFI_BOOT_SERVICES Services; EFI_BOOT_SERVICES *gBS=&Services;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiDevicePathProtocolGuid;
static EFI_BLOCK_IO_PROTOCOL Io[DEVICES]; static EFI_BLOCK_IO_MEDIA Media[DEVICES];
static UINT8 Paths[DEVICES][128],Storage[DEVICES][65536];
static UINTN Order[DEVICES],Opens[DEVICES],Closes[DEVICES],Reconnects[DEVICES];
static UINTN Reads[DEVICES],Writes[DEVICES],Flushes[DEVICES],Pools,Allocations,FailAllocation,Cases;
static EFI_STATUS OpenResult[DEVICES],ReadResult[DEVICES],FlushResult[DEVICES];
static BOOLEAN Held[DEVICES],ChangeOnOpen[DEVICES];
static UINTN Events[32],EventCount,DeviceCount;
static EFI_HANDLE Image=(EFI_HANDLE)100;
static MSC_BLOCK Blocks[2];static CR_USB_MSC_BLOCK_BACKEND Back[2];static UINTN Count;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI AllocatePool(UINTN N){if(++Allocations==FailAllocation)return NULL;++Pools;return malloc(N);}
VOID EFIAPI FreePool(VOID *P){assert(P&&Pools);--Pools;free(P);}
UINTN EFIAPI Print(CONST CHAR16 *F,...){(VOID)F;return 0;}
static void Record(UINTN Operation,UINTN I){assert(EventCount<ARRAY_SIZE(Events));Events[EventCount++]=Operation*DEVICES+I;}
static UINTN Index(EFI_HANDLE H){UINTN I=(UINTN)H-1;assert(I<DEVICES);return I;}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **H)
{
  assert(T==ByProtocol&&G==&gEfiBlockIoProtocolGuid&&!K);
  *N=DeviceCount;*H=AllocatePool(DeviceCount*sizeof(**H));if(!*H)return EFI_OUT_OF_RESOURCES;
  for(UINTN I=0;I<DeviceCount;++I)(*H)[I]=(EFI_HANDLE)(Order[I]+1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **P)
{UINTN I=Index(H);if(G==&gEfiBlockIoProtocolGuid)*P=&Io[I];else {assert(G==&gEfiDevicePathProtocolGuid);*P=Paths[I];}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **P,EFI_HANDLE A,EFI_HANDLE C,UINT32 Attr)
{
  UINTN I=Index(H);assert(G==&gEfiBlockIoProtocolGuid&&A==Image&&!C&&Attr==EFI_OPEN_PROTOCOL_EXCLUSIVE&&!Held[I]);
  ++Opens[I];Record(1,I);if(OpenResult[I]!=EFI_SUCCESS)return OpenResult[I];
  Held[I]=TRUE;*P=&Io[I];if(ChangeOnOpen[I])++Io[I].Media->MediaId;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE A,EFI_HANDLE C)
{UINTN I=Index(H);assert(G==&gEfiBlockIoProtocolGuid&&A==Image&&!C&&Held[I]);Held[I]=FALSE;++Closes[I];Record(2,I);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Reconnect(EFI_HANDLE H,EFI_HANDLE *D,EFI_DEVICE_PATH_PROTOCOL *P,BOOLEAN R)
{UINTN I=Index(H);assert(!D&&!P&&R&&!Held[I]);++Reconnects[I];Record(3,I);return EFI_SUCCESS;}
static UINTN Transfer(EFI_BLOCK_IO_PROTOCOL *This,UINT32 Id,EFI_LBA Lba,UINTN N,VOID *P)
{
  UINTN I=(UINTN)(This-Io);assert(I<DEVICES&&Held[I]&&Id==This->Media->MediaId);
  assert(Lba<=This->Media->LastBlock&&N<=sizeof(Storage[I])&&N%This->Media->BlockSize==0);
  assert(!This->Media->IoAlign||!((UINTN)P&(This->Media->IoAlign-1)));return I;
}
static EFI_STATUS EFIAPI BRead(EFI_BLOCK_IO_PROTOCOL *This,UINT32 Id,EFI_LBA Lba,UINTN N,VOID *P)
{UINTN I=Transfer(This,Id,Lba,N,P);++Reads[I];memcpy(P,Storage[I],N);return ReadResult[I];}
static EFI_STATUS EFIAPI BWrite(EFI_BLOCK_IO_PROTOCOL *This,UINT32 Id,EFI_LBA Lba,UINTN N,VOID *P)
{UINTN I=Transfer(This,Id,Lba,N,P);++Writes[I];assert(!"automatic exports must remain read-only");return EFI_WRITE_PROTECTED;}
static EFI_STATUS EFIAPI BFlush(EFI_BLOCK_IO_PROTOCOL *This)
{UINTN I=(UINTN)(This-Io);assert(I<DEVICES&&Held[I]);++Flushes[I];return FlushResult[I];}
static void End(UINTN I,UINTN Offset)
{EFI_DEVICE_PATH_PROTOCOL E={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};memcpy(Paths[I]+Offset,&E,4);}
static void Ufs(UINTN I,UINT8 Lun)
{UFS_DEVICE_PATH U={{MESSAGING_DEVICE_PATH,MSG_UFS_DP,{sizeof(U),0}},0,Lun};memset(Paths[I],0,sizeof(Paths[I]));memcpy(Paths[I],&U,sizeof(U));End(I,sizeof(U));}
static void Sd(UINTN I,BOOLEAN Standard)
{
  memset(Paths[I],0,sizeof(Paths[I]));
  if(Standard){SD_DEVICE_PATH P={{MESSAGING_DEVICE_PATH,MSG_SD_DP,{sizeof(P),0}},0};memcpy(Paths[I],&P,sizeof(P));End(I,sizeof(P));}
  else{VENDOR_DEVICE_PATH P={{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(P),0}},{0x9d7d72ab,0x187c,0x4f29,{0x9d,0xa2,0x84,0x50,0x53,0x44,0x43,0x32}}};memcpy(Paths[I],&P,sizeof(P));End(I,sizeof(P));}
}
static void Init(void)
{
  assert(!Pools);for(UINTN I=0;I<DEVICES;++I)assert(!Held[I]);
  memset(&Services,0,sizeof(Services));memset(Io,0,sizeof(Io));memset(Media,0,sizeof(Media));memset(Paths,0,sizeof(Paths));
  memset(Blocks,0,sizeof(Blocks));memset(Back,0,sizeof(Back));Count=99;
  Services.HandleProtocol=Handle;Services.LocateHandleBuffer=Locate;Services.OpenProtocol=Open;Services.CloseProtocol=Close;Services.ConnectController=Reconnect;
  EventCount=Allocations=FailAllocation=0;DeviceCount=DEVICES;
  for(UINTN I=0;I<DEVICES;++I){
    Opens[I]=Closes[I]=Reconnects[I]=Reads[I]=Writes[I]=Flushes[I]=0;
    OpenResult[I]=ReadResult[I]=FlushResult[I]=EFI_SUCCESS;ChangeOnOpen[I]=FALSE;Order[I]=I;
    Io[I].Media=&Media[I];Io[I].ReadBlocks=BRead;Io[I].WriteBlocks=BWrite;Io[I].FlushBlocks=BFlush;
    Media[I].MediaId=42+I;Media[I].MediaPresent=TRUE;Media[I].BlockSize=4096;Media[I].LastBlock=0x100000010ULL;Media[I].IoAlign=4096;
    Ufs(I,(UINT8)(I+1));memset(Storage[I],(int)(0x40+I),sizeof(Storage[I]));
  }
  Ufs(UFS0,0);Sd(SD,FALSE);Media[SD].BlockSize=512;Media[SD].LastBlock=65535;
  Ufs(2,0xC4); /* well-known/security LUN, never a candidate */
  Ufs(3,0);Media[3].LogicalPartition=TRUE; /* logical alias is excluded */
  Paths[5][0]=MEDIA_DEVICE_PATH; /* unrelated/partition path */
}
static void Release(void)
{for(UINTN I=2;I;--I)assert(MscReleaseBlock(&Blocks[I-1])==EFI_SUCCESS);assert(!Pools);for(UINTN I=0;I<DEVICES;++I)assert(!Held[I]);}
static void Selected(BOOLEAN HasSd)
{
  assert(MscAttachBlocks(Image,Blocks,Back,&Count)==EFI_SUCCESS);assert(Count==(HasSd?2U:1U));
  assert(Blocks[0].Handle==(EFI_HANDLE)(UFS0+1)&&Back[0].Context==&Blocks[0]&&Back[0].BlockSize==4096&&Back[0].ReadOnly);
  if(HasSd)assert(Blocks[1].Handle==(EFI_HANDLE)(SD+1)&&Back[1].Context==&Blocks[1]&&Back[1].BlockSize==512&&Back[1].ReadOnly);
  assert(Opens[UFS0]==1&&Opens[SD]==(HasSd?1U:0U));
  for(UINTN I=0;I<DEVICES;++I)if(I!=UFS0&&I!=SD)assert(!Opens[I]);
  ++Cases;
}
static void Selection(void)
{
  for(UINTN Shift=0;Shift<DEVICES;++Shift){
    Init();for(UINTN I=0;I<DEVICES;++I)Order[I]=(I+Shift)%DEVICES;Selected(TRUE);
    assert(Events[0]==DEVICES+UFS0&&Events[1]==DEVICES+SD);Release();
    assert(Events[2]==2*DEVICES+SD&&Events[3]==3*DEVICES+SD&&Events[4]==2*DEVICES+UFS0&&Events[5]==3*DEVICES+UFS0);
  }
  Init();Sd(SD,TRUE);Selected(TRUE);Release();
  Init();Media[SD].MediaPresent=FALSE;Selected(FALSE);Release();assert(!Opens[SD]&&!Reconnects[SD]);
  Init();Ufs(SD,7);Selected(FALSE);Release();
}
static void Access(void)
{
  UINT8 Data[65536];Init();Selected(TRUE);
  assert(Back[0].Read(Back[0].Context,0x100000000ULL,1,Data)==CR_SUCCESS&&Reads[UFS0]==1&&Data[0]==0x44);
  assert(Back[1].Read(Back[1].Context,0,128,Data)==CR_SUCCESS&&Reads[SD]==1&&Data[65535]==0x41);
  for(UINTN I=0;I<2;++I){
    assert(Back[I].Check(Back[I].Context)==CR_SUCCESS);
    assert(Back[I].Write==NULL);
    assert(Back[I].Flush(Back[I].Context)==CR_SUCCESS);
  }
  assert(!Writes[UFS0]&&!Writes[SD]&&Flushes[UFS0]==1&&Flushes[SD]==1);
  assert(Back[0].Read(Back[0].Context,Media[UFS0].LastBlock,2,Data)==CR_INVALID_PARAMETER&&Reads[UFS0]==1);
  assert(Back[1].Read(Back[1].Context,0,129,Data)==CR_INVALID_PARAMETER&&Reads[SD]==1);
  ++Media[SD].MediaId;assert(Back[1].Check(Back[1].Context)==CR_ABORT);--Media[SD].MediaId;
  assert(Back[1].Read(Back[1].Context,0,1,Data)==CR_ABORT&&Reads[SD]==1);
  assert(Back[0].Check(Back[0].Context)==CR_SUCCESS&&Back[0].Read(Back[0].Context,0,1,Data)==CR_SUCCESS&&Reads[UFS0]==2);
  Release();++Cases;
  Init();Selected(TRUE);ReadResult[UFS0]=EFI_NO_MEDIA;
  assert(Back[0].Read(Back[0].Context,0,1,Data)==CR_NOT_FOUND);ReadResult[UFS0]=EFI_SUCCESS;
  assert(Back[0].Check(Back[0].Context)==CR_ABORT&&Back[1].Check(Back[1].Context)==CR_SUCCESS);
  assert(Back[1].Read(Back[1].Context,0,1,Data)==CR_SUCCESS);Release();++Cases;
  Init();Selected(TRUE);ReadResult[UFS0]=EFI_WARN_UNKNOWN_GLYPH;
  assert(Back[0].Read(Back[0].Context,0,1,Data)==CR_DEVICE_ERROR);Release();++Cases;
}
static void Rejections(void)
{
  for(UINTN Which=0;Which<7;++Which){
    Init();
    if(Which==0)Ufs(UFS0,7); /* no exact LUN0 */
    if(Which==1)Ufs(6,0); /* duplicate whole UFS0 */
    if(Which==2)Sd(6,FALSE); /* duplicate native SD alias */
    if(Which==3)Sd(6,TRUE); /* native/standard alias ambiguity */
    if(Which==4)Media[UFS0].MediaPresent=FALSE;
    if(Which==5){Ufs(UFS0,0xC4);Media[3].LogicalPartition=TRUE;}
    if(Which==6){EFI_DEVICE_PATH_PROTOCOL *E=(VOID *)(Paths[UFS0]+sizeof(UFS_DEVICE_PATH));E->SubType=END_INSTANCE_DEVICE_PATH_SUBTYPE;}
    assert(MscAttachBlocks(Image,Blocks,Back,&Count)!=EFI_SUCCESS);
    if(Which<4||Which==5||Which==6)for(UINTN I=0;I<DEVICES;++I)assert(!Opens[I]);
    Release();++Cases;
  }
  /* A present recognized SD is an error, not silently omitted. */
  for(UINTN Which=0;Which<7;++Which){
    Init();
    if(Which==0)OpenResult[SD]=EFI_ACCESS_DENIED;
    if(Which==1)Media[SD].IoAlign=3;
    if(Which==2)Media[SD].LastBlock=MAX_UINT64;
    if(Which==3)ChangeOnOpen[SD]=TRUE;
    if(Which==4)FlushResult[SD]=EFI_DEVICE_ERROR;
    if(Which==5)Media[SD].BlockSize=520;
    if(Which==6)Io[SD].ReadBlocks=NULL;
    assert(MscAttachBlocks(Image,Blocks,Back,&Count)!=EFI_SUCCESS);Release();
    if(Which==0){assert(Opens[UFS0]==1&&Closes[UFS0]==1&&Opens[SD]==1&&!Closes[SD]&&Reconnects[SD]==1);assert(Events[EventCount-2]==2*DEVICES+UFS0&&Events[EventCount-1]==3*DEVICES+UFS0);}
    ++Cases;
  }
  for(UINTN Fail=1;Fail<=3;++Fail){Init();FailAllocation=Fail;assert(MscAttachBlocks(Image,Blocks,Back,&Count)==EFI_OUT_OF_RESOURCES);Release();++Cases;}
  Init();OpenResult[UFS0]=EFI_ACCESS_DENIED;assert(MscAttachBlocks(Image,Blocks,Back,&Count)==EFI_ACCESS_DENIED);Release();assert(!Opens[SD]&&Reconnects[UFS0]==1);++Cases;
}
int main(void){Selection();Access();Rejections();printf("Automatic EFI MSC adapter: %zu selections/fault scenarios, exact UFS0 plus optional SD, aliases, read-only, aligned I/O, media isolation and rollback PASS\n",(size_t)Cases);return 0;}
