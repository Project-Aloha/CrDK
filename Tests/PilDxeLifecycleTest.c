/** Production PIL DXE lifecycle/firmware I/O regressions. SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <Uefi.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../Driver/PILCrDxe/PILCrDxe.c"

EFI_GUID gEfiFileInfoGuid, gEfiPartitionInfoProtocolGuid, gEfiBlockIoProtocolGuid;
EFI_GUID gEfiDevicePathProtocolGuid, gEfiSimpleFileSystemProtocolGuid;
EFI_GUID gEfiDriverBindingProtocolGuid, gEfiPilCrProtocolGuid;
EFI_GUID gEfiSmemCrProtocolGuid, gEfiCmdDBCrProtocolGuid;
EFI_GUID gEfiRpmhCrProtocolGuid, gEfiInterconnectCrProtocolGuid;
EFI_GUID gEfiEventExitBootServicesGuid, gEfiEventReadyToBootGuid, gEfiAcpiTableProtocolGuid;
static EFI_BOOT_SERVICES Services;
static EFI_DXE_SERVICES DxeServices;
EFI_BOOT_SERVICES *gBS = &Services;
EFI_DXE_SERVICES *gDS = &DxeServices;
static EFI_SMEM_CR_PROTOCOL Smem;
static EFI_CMD_DB_PROTOCOL Db;
static EFI_RPMH_CR_PROTOCOL Rpmh;
static EFI_INTERCONNECT_CR_PROTOCOL Icb;
static CONST CR_PIL_TARGET Target = {
  .PasId=1, .MemoryBase=0x85e00000, .MemorySize=0x2100000,
  .ImagePath=L"\\image\\adsp",
};
static CR_DAL_DEVICE_INFO Info;
static EFI_PARTITION_INFO_PROTOCOL Partition;
static EFI_BLOCK_IO_MEDIA Media;
static EFI_BLOCK_IO_PROTOCOL Block;
static struct { UFS_DEVICE_PATH Ufs; EFI_DEVICE_PATH_PROTOCOL End; } Path;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_FILE_PROTOCOL Files[3]; /* root, MDT/MBN, b02 */
static UINT8 Header[196];
static UINTN Positions[3], FileHandles, Pools, Pages, Reads, PageFrees, AttrCalls;
static UINTN Starts, Stops, StopOwned, Creates, FailCreate, ClosedEvents;
static EFI_TPL CurrentTpl;
static VOID *PageBuffer;
static UINT64 Attributes;
static BOOLEAN FsOwned, Published, NoFirmware, MbnOnly, Truncated, Oversized;
static BOOLEAN Borrowed, FailShutdown, FailAttributes, FailRestore, BadCarveout;
static BOOLEAN VolumeFailure;
static BOOLEAN InterruptedStart,FailRetirement;
static UINTN Retirements;
static EFI_STATUS StartError, RemoveError;
static EFI_EVENT_NOTIFY ExitCallback;
static UINTN ClientCalls;
static EFI_STATUS ClientStatus;
static BOOLEAN ClientQuiet;
static UINTN EbsEntered, EbsStopEntered, EbsStopReturned, EbsDeferred;
static CR_STATUS ClientQuiesce(VOID *Context)
{
  assert(Context == &ClientCalls && CurrentTpl == TPL_NOTIFY && mExiting);
  assert(mPil.PasOwned && Stops == 0 && EbsEntered != 0 && EbsStopEntered == 0);
  ++ClientCalls;
  ClientQuiet = !EFI_ERROR(ClientStatus);
  return ClientStatus;
}

static VOID Application(VOID) { assert(CurrentTpl==TPL_APPLICATION && !mExiting); }
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S) {return strlen(S);}
VOID EFIAPI MemoryFence(VOID) {}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N) {(VOID)P;(VOID)N;abort();}
BOOLEAN EFIAPI DebugAssertEnabled(VOID) {return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D) {fprintf(stderr,"%s:%zu %s\n",F,(size_t)L,D);abort();}
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) {return memset(D,V,N);}
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D,0,N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A,B,N); }
BOOLEAN EFIAPI CompareGuid(CONST GUID *A, CONST GUID *B) { return memcmp(A,B,sizeof(*A))==0; }
RETURN_STATUS EFIAPI StrCpyS(CHAR16 *D, UINTN N, CONST CHAR16 *S)
{
  UINTN I=0;
  while(S[I]!=0) { if(I+1>=N) return RETURN_BUFFER_TOO_SMALL; D[I]=S[I]; ++I; }
  D[I]=0; return RETURN_SUCCESS;
}
RETURN_STATUS EFIAPI StrCatS(CHAR16 *D, UINTN N, CONST CHAR16 *S)
{ UINTN I=0; while(I<N && D[I]!=0) ++I; return I==N?RETURN_BUFFER_TOO_SMALL:StrCpyS(D+I,N-I,S); }
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *Address, UINTN Length)
{ Application(); assert(Address==PageBuffer && Length==EFI_PAGES_TO_SIZE(Pages)); return Address; }
VOID *EFIAPI AllocatePool(UINTN Size) { Application(); ++Pools; return malloc(Size); }
VOID EFIAPI FreePool(VOID *Buffer) { Application(); assert(Pools!=0); --Pools; free(Buffer); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (VOID)Level; return TRUE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {
  (VOID)Level;
  if (strstr(Format,"ExitBootServices entered") != NULL) ++EbsEntered;
  if (strstr(Format,"clients drained; ADSP stop begin") != NULL) ++EbsStopEntered;
  if (strstr(Format,"ExitBootServices ADSP shutdown:") != NULL) ++EbsStopReturned;
  if (strstr(Format,"active startup retained") != NULL) ++EbsDeferred;
}

EFI_STATUS CrDalGetDeviceInfo(CR_DAL_DEVICE_TYPE Type, UINT32 Instance,
                             UINT32 Revision, UINTN Size, CONST CR_DAL_DEVICE_INFO **Result)
{
  assert(Type==CrDalDevicePil && Instance==0 && Revision==CR_DAL_DATA_REVISION_1);
  assert(Size==sizeof(Target)); *Result=&Info; return EFI_SUCCESS;
}
EFI_STATUS CrPilInitialize(CR_PIL_CONTEXT *Pil, CONST CR_PIL_TARGET *T,
                           EFI_SMEM_CR_PROTOCOL *S, EFI_CMD_DB_PROTOCOL *D,
                           EFI_RPMH_CR_PROTOCOL *R, EFI_INTERCONNECT_CR_PROTOCOL *I)
{
  assert(Pil==&mPil && T==&Target && S==&Smem && D==&Db && R==&Rpmh && I==&Icb);
  ZeroMem(Pil,sizeof(*Pil)); Pil->Target=T; Pil->Stage="idle"; return EFI_SUCCESS;
}
EFI_STATUS CrPilStart(CR_PIL_CONTEXT *Pil, CONST VOID *H, UINTN Size,
                      CONST CR_PIL_IMAGE *Image, VOID *Metadata, VOID *Carveout,
                      CR_PIL_READ_SEGMENT Read, VOID *Root)
{
  UINT8 Segment[4];
  EFI_STATUS Status;
  Application(); ++Starts;
  assert(Pil==&mPil && H!=NULL && Size==sizeof(Header) && Image->Split && Image->Count==3);
  assert(Metadata==PageBuffer && ((UINTN)Metadata&4095)==0 && (UINTN)Metadata<=MAX_UINT32);
  assert((Attributes&EFI_CACHE_ATTRIBUTE_MASK)==EFI_MEMORY_WC && Carveout==(VOID *)(UINTN)Target.MemoryBase);
  assert(memcmp(Metadata,Header,180)==0 && ((UINT8 *)Metadata)[180]==0x88);
  assert(Root==&Files[0] && FsOwned && FileHandles==2 && Read!=NULL);
  if(InterruptedStart) {Pil->SmcInterrupted=TRUE;Pil->Stage="fixture interrupted capability";return EFI_TIMEOUT;}
  if(Borrowed) return EFI_ALREADY_STARTED;
  Pil->PasOwned=TRUE; Pil->Stage="fixture auth";
  Status=Read(Root,2,Segment,sizeof(Segment));
  if(EFI_ERROR(Status)) return Status;
  assert(Segment[0]==0xa5 && Segment[3]==0xa5);
  if(EFI_ERROR(StartError)) return StartError;
  Pil->Running=TRUE; Pil->Stage="running"; return EFI_SUCCESS;
}
EFI_STATUS CrPilStop(CR_PIL_CONTEXT *Pil)
{
  assert(!mExiting || mClients.Count == 0 || ClientQuiet);
  if (mExiting) assert(EbsEntered != 0 && EbsStopEntered != 0);
  ++Stops; assert(Pil==&mPil);
  if(Pil->SmcInterrupted) {
    ++Retirements;if(FailRetirement) return EFI_TIMEOUT;
    Pil->SmcInterrupted=FALSE;
  }
  if(Pil->PasOwned) { ++StopOwned; if(FailShutdown) return EFI_DEVICE_ERROR; }
  Pil->PasOwned=Pil->Running=FALSE; return EFI_SUCCESS;
}
static EFI_TPL EFIAPI Raise(EFI_TPL Tpl)
{ EFI_TPL Old=CurrentTpl; assert(Tpl>=Old); CurrentTpl=Tpl; return Old; }
static VOID EFIAPI Restore(EFI_TPL Tpl) { CurrentTpl=Tpl; }
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Registration, VOID **Protocol)
{
  assert(Registration==NULL);
  if(Guid==&gEfiSmemCrProtocolGuid) *Protocol=&Smem;
  else if(Guid==&gEfiCmdDBCrProtocolGuid) *Protocol=&Db;
  else if(Guid==&gEfiRpmhCrProtocolGuid) *Protocol=&Rpmh;
  else if(Guid==&gEfiInterconnectCrProtocolGuid) *Protocol=&Icb;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE Controller, EFI_GUID *Guid, VOID **Protocol)
{
  Application(); assert(Controller==(EFI_HANDLE)0x5678);
  if(Guid==&gEfiPartitionInfoProtocolGuid) *Protocol=&Partition;
  else if(Guid==&gEfiBlockIoProtocolGuid) *Protocol=&Block;
  else if(Guid==&gEfiDevicePathProtocolGuid) *Protocol=&Path;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE Controller, EFI_GUID *Guid, VOID **Protocol,
                              EFI_HANDLE Agent, EFI_HANDLE ControllerHandle, UINT32 Flags)
{
  Application(); assert(Controller==(EFI_HANDLE)0x5678 && ControllerHandle==Controller);
  assert(Agent==(EFI_HANDLE)0x1234 && Guid==&gEfiSimpleFileSystemProtocolGuid);
  assert(Flags==EFI_OPEN_PROTOCOL_BY_DRIVER && !FsOwned); FsOwned=TRUE; *Protocol=&Fs;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseProtocol(EFI_HANDLE Controller, EFI_GUID *Guid,
                                       EFI_HANDLE Agent, EFI_HANDLE ControllerHandle)
{
  Application(); assert(Controller==(EFI_HANDLE)0x5678 && ControllerHandle==Controller);
  assert(Agent==(EFI_HANDLE)0x1234 && Guid==&gEfiSimpleFileSystemProtocolGuid && FsOwned);
  assert(FileHandles==0); FsOwned=FALSE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Create(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Callback,
                                CONST VOID *Context, CONST EFI_GUID *Guid, EFI_EVENT *Event)
{
  Application(); assert(Type==EVT_NOTIFY_SIGNAL && Context==NULL);
  if(++Creates==FailCreate) return EFI_OUT_OF_RESOURCES;
  if(Guid==&gEfiEventExitBootServicesGuid) { assert(Tpl==TPL_NOTIFY); ExitCallback=Callback; *Event=(EFI_EVENT)1; }
  else { assert(Guid==&gEfiEventReadyToBootGuid && Tpl==TPL_CALLBACK); *Event=(EFI_EVENT)2; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT Event)
{ Application(); assert(Event==(EFI_EVENT)1 || Event==(EFI_EVENT)2); ++ClosedEvents; return EFI_SUCCESS; }
static VOID ProtocolArgs(va_list Args)
{
  assert(va_arg(Args,EFI_GUID *)==&gEfiDriverBindingProtocolGuid);
  assert(va_arg(Args,VOID *)==&mBinding);
  assert(va_arg(Args,EFI_GUID *)==&gEfiPilCrProtocolGuid);
  assert(va_arg(Args,VOID *)==&mProtocol && va_arg(Args,VOID *)==NULL);
}
static EFI_STATUS EFIAPI Publish(EFI_HANDLE *ImageHandle, ...)
{
  va_list Args; Application(); assert(*ImageHandle==(EFI_HANDLE)0x1234 && Creates==2);
  va_start(Args,ImageHandle); ProtocolArgs(Args); va_end(Args); Published=TRUE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Remove(EFI_HANDLE ImageHandle, ...)
{
  va_list Args; Application(); assert(ImageHandle==(EFI_HANDLE)0x1234 && Published && !FsOwned);
  va_start(Args,ImageHandle); ProtocolArgs(Args); va_end(Args);
  if(EFI_ERROR(RemoveError)) return RemoveError;
  Published=FALSE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockAllocatePages(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                       UINTN Number, EFI_PHYSICAL_ADDRESS *Address)
{
  Application(); assert(Type==AllocateMaxAddress && MemoryType==EfiReservedMemoryType);
  assert(*Address==MAX_UINT32 && Number==1 && Pages==0);
  PageBuffer=mmap(NULL,EFI_PAGES_TO_SIZE(Number),PROT_READ|PROT_WRITE,
                  MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  assert(PageBuffer!=MAP_FAILED && (UINTN)PageBuffer<=MAX_UINT32);
  Pages=Number; *Address=(UINTN)PageBuffer; Attributes=EFI_MEMORY_WB|EFI_MEMORY_XP; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MockFreePages(EFI_PHYSICAL_ADDRESS Address, UINTN Number)
{
  Application(); assert(Address==(UINTN)PageBuffer && Number==Pages && Pages!=0);
  assert(Attributes==(EFI_MEMORY_WB|EFI_MEMORY_XP) && (!mPil.PasOwned || mReady));
  assert(munmap(PageBuffer,EFI_PAGES_TO_SIZE(Pages))==0);
  Pages=0; PageBuffer=NULL; ++PageFrees; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS Address, EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D)
{
  Application(); ZeroMem(D,sizeof(*D));
  if(Address==Target.MemoryBase) {
    D->BaseAddress=0x85200000; D->Length=0x1c200000;
    D->GcdMemoryType=BadCarveout?EfiGcdMemoryTypeSystemMemory:EfiGcdMemoryTypeReserved;
    return EFI_SUCCESS;
  }
  assert(Address==(UINTN)PageBuffer && Pages!=0);
  D->BaseAddress=Address; D->Length=EFI_PAGES_TO_SIZE(Pages); D->Attributes=Attributes;
  D->Capabilities=EFI_MEMORY_WB|EFI_MEMORY_WC; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetAttributes(EFI_PHYSICAL_ADDRESS Address, UINT64 Length, UINT64 Value)
{
  Application(); assert(Address==(UINTN)PageBuffer && Length==EFI_PAGES_TO_SIZE(Pages)); ++AttrCalls;
  assert((Value&EFI_MEMORY_XP)!=0);
  if((Value&EFI_CACHE_ATTRIBUTE_MASK)==EFI_MEMORY_WC) {
    Attributes=Value; /* deliberately partial success before returning failure */
    if(FailAttributes) return EFI_DEVICE_ERROR;
  } else {
    assert(Value==(EFI_MEMORY_WB|EFI_MEMORY_XP));
    if(FailRestore) return EFI_DEVICE_ERROR;
    Attributes=Value;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FileClose(EFI_FILE_PROTOCOL *File)
{ Application(); assert(File>=Files && File<Files+3 && FileHandles!=0); --FileHandles; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root)
{
  Application(); assert(This==&Fs && FsOwned && FileHandles==0);
  if(VolumeFailure) return EFI_DEVICE_ERROR;
  *Root=&Files[0]; ++FileHandles; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FileOpen(EFI_FILE_PROTOCOL *Root, EFI_FILE_PROTOCOL **File,
                                  CHAR16 *Name, UINT64 Mode, UINT64 Flags)
{
  UINTN N=0;
  Application(); assert(Root==&Files[0] && Mode==EFI_FILE_MODE_READ && Flags==0);
  while(Name[N]!=0) ++N;
  assert(N==15 && memcmp(Name,L"\\image\\adsp.",12*sizeof(CHAR16))==0);
  if(Name[12]==L'b') { assert(Name[13]==L'0' && Name[14]==L'2'); *File=&Files[2]; Positions[2]=0; }
  else {
    assert((Name[12]==L'm' && Name[13]==L'd' && Name[14]==L't') ||
           (Name[12]==L'm' && Name[13]==L'b' && Name[14]==L'n'));
    if(NoFirmware || (MbnOnly && Name[13]==L'd')) return EFI_NOT_FOUND;
    *File=&Files[1]; Positions[1]=0;
  }
  ++FileHandles; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GetInfo(EFI_FILE_PROTOCOL *File, EFI_GUID *Guid, UINTN *Size, VOID *Buffer)
{
  EFI_FILE_INFO *F=Buffer;
  Application(); assert(Guid==&gEfiFileInfoGuid && *Size>=SIZE_OF_EFI_FILE_INFO+2);
  ZeroMem(Buffer,*Size); F->Size=SIZE_OF_EFI_FILE_INFO+2;
  F->FileSize=File==&Files[1]?sizeof(Header):(Oversized?5:4);
  *Size=(UINTN)F->Size; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FileRead(EFI_FILE_PROTOCOL *File, UINTN *Size, VOID *Buffer)
{
  UINTN Index=(UINTN)(File-Files), Length, Actual;
  Application(); assert(Index==1 || Index==2); ++Reads;
  Actual=Index==1?sizeof(Header):(Truncated?2:4);
  Length=MIN(*Size,MIN(7U,Actual-Positions[Index])); /* genuine partial reads */
  if(Index==1) memcpy(Buffer,Header+Positions[Index],Length);
  else memset(Buffer,0xa5,Length);
  Positions[Index]+=Length; *Size=Length; return EFI_SUCCESS;
}
static VOID Put16(UINT8 *P, UINT16 V) { P[0]=(UINT8)V; P[1]=(UINT8)(V>>8); }
static VOID Put32(UINT8 *P, UINT32 V) { for(UINTN I=0;I<4;++I) P[I]=(UINT8)(V>>(I*8)); }
static VOID Fixture(VOID)
{
  assert(Pools==0 && Pages==0 && !FsOwned && FileHandles==0);
  ZeroMem(&mPil,sizeof(mPil)); mController=NULL; mReady=mStarting=mExiting=mInstalled=FALSE;
  ZeroMem(&mClients,sizeof(mClients)); ClientCalls=0; ClientStatus=EFI_SUCCESS; ClientQuiet=FALSE;
  mMetadata=0; mMetadataPages=0; mMetadataChanged=FALSE; mLastError=EFI_NOT_READY;
  mExit=mReadyBoot=NULL;
  EbsEntered=EbsStopEntered=EbsStopReturned=EbsDeferred=0;
  ZeroMem(&mHandoff,sizeof(mHandoff));mRpmh=NULL;
  CurrentTpl=TPL_APPLICATION; Reads=PageFrees=AttrCalls=Starts=Stops=StopOwned=0;
  Creates=FailCreate=ClosedEvents=0; Published=NoFirmware=MbnOnly=Truncated=Oversized=FALSE;
  Borrowed=FailShutdown=FailAttributes=FailRestore=BadCarveout=VolumeFailure=FALSE;
  InterruptedStart=FailRetirement=FALSE;Retirements=0;
  StartError=RemoveError=EFI_SUCCESS;
  ZeroMem(&Info,sizeof(Info)); Info.Data=&Target;
  ZeroMem(&Partition,sizeof(Partition)); Partition.Type=PARTITION_TYPE_GPT;
  CopyMem(&Partition.Info.Gpt.PartitionTypeGUID,&mBasicData,sizeof(mBasicData));
  ZeroMem(&Media,sizeof(Media)); Media.MediaPresent=TRUE; Block.Media=&Media;
  Path.Ufs.Header.Type=MESSAGING_DEVICE_PATH; Path.Ufs.Header.SubType=MSG_UFS_DP;
  Path.Ufs.Header.Length[0]=sizeof(Path.Ufs); Path.Ufs.Header.Length[1]=0;
  Path.End.Type=END_DEVICE_PATH_TYPE; Path.End.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;
  Path.End.Length[0]=sizeof(Path.End); Path.End.Length[1]=0;
  Fs.OpenVolume=OpenVolume;
  ZeroMem(Files,sizeof(Files));
  for(UINTN I=0;I<3;++I) { Files[I].Close=FileClose; Files[I].Open=FileOpen; Files[I].Read=FileRead; Files[I].GetInfo=GetInfo; }
  ZeroMem(Header,sizeof(Header)); memcpy(Header,"\177ELF",4); Header[4]=Header[5]=Header[6]=1;
  Put32(Header+20,1); Put32(Header+28,52); Put16(Header+40,52); Put16(Header+42,32); Put16(Header+44,3);
  Put32(Header+52+16,180); /* padded signed header */
  Put32(Header+84,1); Put32(Header+84+4,180); Put32(Header+84+16,16); Put32(Header+84+24,2U<<24);
  Put32(Header+116,1); Put32(Header+116+4,0x1000); Put32(Header+116+12,0x100000);
  Put32(Header+116+16,4); Put32(Header+116+20,8); Put32(Header+116+24,BIT27);
  memset(Header+180,0x88,16);
  Services.RaiseTPL=Raise; Services.RestoreTPL=Restore; Services.LocateProtocol=Locate;
  Services.HandleProtocol=Handle; Services.OpenProtocol=Open; Services.CloseProtocol=CloseProtocol;
  Services.CreateEventEx=Create; Services.CloseEvent=CloseEvent;
  Services.InstallMultipleProtocolInterfaces=Publish; Services.UninstallMultipleProtocolInterfaces=Remove;
  Services.AllocatePages=MockAllocatePages; Services.FreePages=MockFreePages;
  DxeServices.GetMemorySpaceDescriptor=Descriptor; DxeServices.SetMemorySpaceAttributes=SetAttributes;
}
static EFI_STATUS Bind(VOID)
{
  assert(PILCrDxeEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS && Published);
  assert(mBinding.Supported(&mBinding,(EFI_HANDLE)0x5678,NULL)==EFI_SUCCESS && !FsOwned && Reads==0);
  return mBinding.Start(&mBinding,(EFI_HANDLE)0x5678,NULL);
}
static VOID Unload(VOID)
{
  assert(PILCrDxeUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS);
  assert(!Published && !FsOwned && Pools==0 && Pages==0 && ClosedEvents==2);
}
int main(VOID)
{
  EFI_PIL_CR_STATE State;
  Fixture(); FailCreate=2;
  assert(PILCrDxeEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_OUT_OF_RESOURCES && ClosedEvents==1 && !Published);
  Fixture(); assert(Bind()==EFI_SUCCESS && mReady && mPil.PasOwned);
  assert(Pages==0 && Pools==0 && PageFrees==1 && FileHandles==0 && Reads>28);
  assert(mProtocol.GetState(&mProtocol,&State)==EFI_SUCCESS && State.Ready && State.Owned);
  Unload(); assert(StopOwned==1);
  Fixture(); NoFirmware=TRUE;
  assert(Bind()==EFI_NOT_FOUND && Starts==0 && !FsOwned && FileHandles==0);
  assert(mProtocol.GetState(&mProtocol,&State)==EFI_NOT_READY && State.LastError==EFI_NOT_FOUND);
  NoFirmware=FALSE; MbnOnly=TRUE; /* retry after firmware appears, without reload */
  assert(mBinding.Start(&mBinding,(EFI_HANDLE)0x5678,NULL)==EFI_SUCCESS && mReady); Unload();
  Fixture(); VolumeFailure=TRUE; assert(Bind()==EFI_DEVICE_ERROR && !FsOwned); Unload();
  Fixture(); BadCarveout=TRUE; assert(Bind()==EFI_ACCESS_DENIED && Starts==0 && Pages==0); Unload();
  Fixture(); Truncated=TRUE; assert(Bind()==EFI_BAD_BUFFER_SIZE && StopOwned==1 && Pages==0); Unload();
  Fixture(); Oversized=TRUE; assert(Bind()==EFI_BAD_BUFFER_SIZE && StopOwned==1 && Pages==0); Unload();
  Fixture(); FailAttributes=TRUE;
  assert(Bind()==EFI_DEVICE_ERROR && AttrCalls==2 && Pages==0 && Starts==0); Unload();
  Fixture(); FailAttributes=FailRestore=TRUE;
  assert(Bind()==EFI_SUCCESS && Pages==1 && !mReady && Starts==0);
  assert(mProtocol.GetState(&mProtocol,&State)==EFI_NOT_READY && State.LastError==EFI_DEVICE_ERROR);
  FailAttributes=FailRestore=FALSE; Unload();
  Fixture(); StartError=EFI_SECURITY_VIOLATION; FailShutdown=TRUE;
  assert(Bind()==EFI_SUCCESS && !mReady && Pages==1 && mPil.PasOwned && AttrCalls==1);
  assert(PILCrDxeUnload((EFI_HANDLE)0x1234)==EFI_DEVICE_ERROR && Published && FsOwned && Pages==1);
  FailShutdown=FALSE; Unload();
  Fixture(); InterruptedStart=FailRetirement=TRUE;
  assert(Bind()==EFI_SUCCESS && !mReady && mPil.SmcInterrupted && !mPil.PasOwned && Pages==1 && FsOwned);
  UINTN OldStarts=Starts;
  assert(mBinding.Start(&mBinding,(EFI_HANDLE)0x5678,NULL)==EFI_UNSUPPORTED && Starts==OldStarts);
  assert(PILCrDxeUnload((EFI_HANDLE)0x1234)==EFI_TIMEOUT && Published && FsOwned && Pages==1 && ClosedEvents==0);
  FailRetirement=FALSE;Unload();assert(!mPil.SmcInterrupted && Retirements==3 && StopOwned==0);
  Fixture(); NoFirmware=TRUE;assert(Bind()==EFI_NOT_FOUND && !FsOwned && mController==NULL);
  mPil.SmcInterrupted=FailRetirement=TRUE;
  assert(PILCrDxeUnload((EFI_HANDLE)0x1234)==EFI_TIMEOUT && Published && ClosedEvents==0);
  FailRetirement=FALSE;Unload();assert(!mPil.SmcInterrupted && Retirements==2);
  Fixture(); Borrowed=TRUE; assert(Bind()==EFI_SUCCESS && mReady && !mPil.PasOwned);
  Unload(); assert(StopOwned==0);
  Fixture(); assert(Bind()==EFI_SUCCESS); RemoveError=EFI_ACCESS_DENIED;
  assert(PILCrDxeUnload((EFI_HANDLE)0x1234)==EFI_ACCESS_DENIED && Published && !FsOwned && ClosedEvents==0);
  RemoveError=EFI_SUCCESS; Unload();
  Fixture(); assert(Bind()==EFI_SUCCESS);
  assert(mProtocol.RegisterClient(&mProtocol,&ClientCalls,ClientQuiesce,&ClientCalls)==EFI_SUCCESS);
  assert(PILCrDxeUnload((EFI_HANDLE)0x1234)==EFI_ACCESS_DENIED && Stops==0 && FsOwned);
  assert(mBinding.Stop(&mBinding,(EFI_HANDLE)0x5678,0,NULL)==EFI_ACCESS_DENIED && Stops==0);
  assert(mProtocol.UnregisterClient(&mProtocol,&ClientCalls)==EFI_SUCCESS);
  Unload();
  Fixture(); assert(Bind()==EFI_SUCCESS);
  assert(mProtocol.RegisterClient(&mProtocol,&ClientCalls,ClientQuiesce,&ClientCalls)==EFI_SUCCESS);
  UINTN OldReads=Reads, OldAttrs=AttrCalls, OldFrees=PageFrees;
  ClientStatus=EFI_TIMEOUT;
  CurrentTpl=TPL_NOTIFY; ExitCallback((EFI_EVENT)1,NULL);
  assert(ClientCalls==1 && Stops==0 && mPil.PasOwned && mLastError==EFI_TIMEOUT);
  assert(EbsEntered==1 && EbsStopEntered==0 && EbsStopReturned==0);
  assert(mProtocol.RegisterClient(&mProtocol,&OldReads,ClientQuiesce,&ClientCalls)==EFI_ACCESS_DENIED);
  ClientStatus=EFI_SUCCESS;
  CurrentTpl=TPL_NOTIFY; ExitCallback((EFI_EVENT)1,NULL);
  assert(ClientCalls==2 && ClientQuiet);
  assert(EbsEntered==2 && EbsStopEntered==1 && EbsStopReturned==1);
  assert(mExiting && !mReady && !mPil.PasOwned && StopOwned==1 && Pages==0);
  assert(Reads==OldReads && AttrCalls==OldAttrs && PageFrees==OldFrees);
  assert(mProtocol.GetState(&mProtocol,&State)==EFI_NOT_READY && State.Exiting && !State.Owned);
  assert(PILCrDxeUnload((EFI_HANDLE)0x1234)==EFI_ACCESS_DENIED);
  /* Entry diagnostics must also identify an interrupted active startup without
     claiming a quiesced client or issuing secure teardown. */
  CurrentTpl=TPL_APPLICATION; mExiting=FALSE; mStarting=TRUE;
  UINTN BeforeStops=Stops, BeforeClients=ClientCalls;
  CurrentTpl=TPL_NOTIFY; ExitCallback((EFI_EVENT)1,NULL);
  assert(EbsEntered==3 && EbsDeferred==1 && EbsStopEntered==1 && EbsStopReturned==1);
  assert(Stops==BeforeStops && ClientCalls==BeforeClients && mLastError==EFI_NOT_READY);
  puts("PIL DXE: UFS firmware/partial reads, reserved NC metadata, rollback retention, borrow/unload and allocation-free exit passed");
  return 0;
}
