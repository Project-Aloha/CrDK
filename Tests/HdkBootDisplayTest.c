/* Actual HDK GOP/native route parsers/MU framebuffer BLT lifecycle fixture.
 * Hardware registers, SMMU producer and UEFI services are the only doubles.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#undef NULL
#define _PCD_VALUE_PcdHdkDisplayBoard HDK_TEST_BOARD
#include "../Driver/HdkBootDisplayCrDxe/HdkBootDisplayCrDxe.c"

EFI_GUID gEfiGraphicsOutputProtocolGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiSmmuCrProtocolGuid=EFI_SMMU_CR_PROTOCOL_GUID;
EFI_GUID gEfiEventExitBootServicesGuid={.Data1=42};
EFI_BOOT_SERVICES *gBS;
EFI_DXE_SERVICES *gDS;

typedef struct {UINT64 A;UINT32 V;} REGISTER;
STATIC REGISTER Regs[2048];
STATIC UINTN Nr,Reads,Delays,Installs,Closes,Allocations,Frees,BootCalls;
STATIC UINTN FailAllocation;
STATIC UINT32 Progress;
STATIC EFI_TPL Tpl;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *Published;
STATIC EFI_EVENT_NOTIFY ExitNotify;
STATIC VOID *ExitContext;
STATIC BOOLEAN ProgressStopped,RuntimeChange,RouteInvalid,GateOff,ReservedOff;
STATIC BOOLEAN InstallFailure,CloseFailure,Reentry,Reentered,PostExit;
STATIC BOOLEAN ReentryBlt,Translated,WrongPhysical;
STATIC BOOLEAN AlternatePacking,Padding,WriteBack;
STATIC BOOLEAN CpuMapOff,ReadOnly,PartialInstall;
STATIC UINTN CachePublishes;
STATIC EFI_STATUS SmmuStatus;
STATIC VOID *Frame;
STATIC CONST UINT64 FrameAddress=0x40000000; /* CPU-accessible host physical model. */
STATIC CONST UINTN FrameCapacity=0x1000000;
STATIC VOID ServiceCall(VOID){assert(!PostExit);++BootCalls;}
STATIC UINT32 *Reg(UINT64 A){
  for(UINTN I=0;I<Nr;++I)if(Regs[I].A==A)return &Regs[I].V;
  assert(Nr<ARRAY_SIZE(Regs));Regs[Nr]=(REGISTER){A,0};return &Regs[Nr++].V;
}
UINT32 EFIAPI MmioRead32(UINTN A){
  ++Reads;assert(!(A&3));
  assert((A>=0xae00000&&A<0xaf20000)||(A>=0x100000&&A<0x2f0000));
  if(A==(HDK_TEST_BOARD==8245?0x10b00c:0x127004)&&GateOff)return 0;
  if(A==(HDK_TEST_BOARD==8245?0xae6bab0:0xae360ac))return Progress;
  if(A==0xae25014&&RuntimeChange)return (UINT32)FrameAddress+0x1000;
  if(A==0xae25030&&RouteInvalid)return *Reg(A)|(1U<<30);
  return *Reg(A);
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){(VOID)A;(VOID)V;assert(0);return 0;}
UINTN EFIAPI MicroSecondDelay(UINTN Us){
  ServiceCall();assert(Us==3000);++Delays;if(!ProgressStopped)++Progress;return Us;
}
VOID EFIAPI MemoryFence(VOID){}
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){ServiceCall();return Tpl;}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N){
  assert(P==Frame&&N==mDisplay.Mode.FrameBufferSize);++CachePublishes;return P;
}
ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap(VOID){
  STATIC ARM_MEMORY_REGION_DESCRIPTOR_EX Map[2];
  memset(Map,0,sizeof(Map));Map[0].Address=FrameAddress;Map[0].Length=FrameCapacity;
  Map[0].HobOption=CpuMapOff?HobOnlyNoCacheSetting:AddMem;Map[0].MemoryType=EfiReservedMemoryType;
  Map[0].ArmAttributes=WriteBack?WRITE_BACK_XN:UNCACHED_UNBUFFERED_XN;return Map;
}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI SetMem32(VOID *P,UINTN N,UINT32 V){assert(!(N&3));for(UINTN I=0;I<N/4;++I)((UINT32 *)P)[I]=V;return P;}
VOID *EFIAPI SetMem64(VOID *P,UINTN N,UINT64 V){assert(!(N&7));for(UINTN I=0;I<N/8;++I)((UINT64 *)P)[I]=V;return P;}
INTN EFIAPI HighBitSet32(UINT32 V){return V?31-__builtin_clz(V):-1;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugCodeEnabled(VOID){return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"EDK assert %s:%zu %s\n",F,(size_t)L,D);assert(0);}
VOID *EFIAPI AllocatePool(UINTN N){ServiceCall();if(++Allocations==FailAllocation)return NULL;return malloc(N);}
VOID *EFIAPI AllocateCopyPool(UINTN N,CONST VOID *P){VOID *D=AllocatePool(N);if(D)memcpy(D,P,N);return D;}
VOID EFIAPI FreePool(VOID *P){ServiceCall();++Frees;free(P);}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL T){ServiceCall();assert(T>=Tpl);EFI_TPL Old=Tpl;Tpl=T;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL T){ServiceCall();assert(T<=Tpl);Tpl=T;}
STATIC EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  ServiceCall();memset(D,0,sizeof(*D));
  if(A>=FrameAddress&&A<FrameAddress+FrameCapacity){
    D->BaseAddress=FrameAddress;D->Length=FrameCapacity;
    D->GcdMemoryType=ReservedOff?EfiGcdMemoryTypeSystemMemory:EfiGcdMemoryTypeReserved;
    D->Attributes=(WriteBack?EFI_MEMORY_WB:EFI_MEMORY_UC)|(ReadOnly?EFI_MEMORY_RO:0);return EFI_SUCCESS;
  }
  if((A>=0xae00000&&A<0xaf20000)||(A>=0x100000&&A<0x2f0000)){
    D->BaseAddress=A;D->Length=0x200000;D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI Dma(EFI_SMMU_CR_PROTOCOL *This,UINT64 Device,UINT64 Bytes,
    CR_SMMU_DISPLAY_TABLE_READ Table,VOID *Cookie,CR_SMMU_DISPLAY_OBSERVATION *O){
  (VOID)This;ServiceCall();assert(Table==NULL&&Cookie==NULL);
  memset(O,0,sizeof(*O));if(SmmuStatus!=EFI_SUCCESS)return SmmuStatus;
  O->Size=sizeof(*O);O->Revision=1;O->Count=1;O->DeviceAddress=Device;
  O->PhysicalAddress=WrongPhysical?Device+0x1000:Device;O->Bytes=Bytes;
  O->Streams[0].Smr=0x84020820;O->Streams[0].S2cr=Translated?0:(1U<<16);
  O->Streams[0].Sctlr=Translated?1:0;return EFI_SUCCESS;
}
STATIC EFI_SMMU_CR_PROTOCOL Smmu={.Revision=EFI_SMMU_CR_DISPLAY_OBSERVATION_REVISION,.QueryInheritedDisplay=Dma};
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *Registration,VOID **P){
  (VOID)Registration;ServiceCall();
  if(G==&gEfiGraphicsOutputProtocolGuid){*P=Published;return Published?EFI_SUCCESS:EFI_NOT_FOUND;}
  assert(G==&gEfiSmmuCrProtocolGuid);*P=&Smmu;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Notify,
    CONST VOID *C,CONST EFI_GUID *Group,EFI_EVENT *E){
  ServiceCall();assert(Type==EVT_NOTIFY_SIGNAL&&T==TPL_NOTIFY&&Group==&gEfiEventExitBootServicesGuid);
  ExitNotify=Notify;ExitContext=(VOID *)C;*E=(EFI_EVENT)(UINTN)0x1234;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT E){
  ServiceCall();assert(E==(EFI_EVENT)(UINTN)0x1234);++Closes;
  if(CloseFailure)return EFI_DEVICE_ERROR;ExitNotify=NULL;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
  ServiceCall();va_list A;EFI_GUID *G;EFI_GRAPHICS_OUTPUT_PROTOCOL *Candidate=NULL;UINTN Count=0;
  va_start(A,H);while((G=va_arg(A,EFI_GUID *))!=NULL){VOID *P=va_arg(A,VOID *);++Count;
    if(G==&gEfiGraphicsOutputProtocolGuid)Candidate=P;
    else {assert(G==&gEfiDevicePathProtocolGuid);assert(!memcmp(P,&mPath,sizeof(mPath)));}}
  va_end(A);assert(Count==2&&Candidate&&Candidate->Mode->FrameBufferBase==FrameAddress);++Installs;
  if(ReentryBlt){EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color={1,2,3,0};UINT32 Before=((UINT32 *)Frame)[0];
    assert(Candidate->Blt(Candidate,&Color,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_NOT_READY);
    assert(((UINT32 *)Frame)[0]==Before);}
  if(Reentry&&!Reentered){Reentered=TRUE;
    EFI_STATUS R=HdkBootDisplayEntry(NULL,NULL);assert(R==EFI_ALREADY_STARTED);
    assert(mDisplay.Busy&&mDisplay.ExitEvent&&mDisplay.Blt);}
  if(InstallFailure){if(PartialInstall)*H=(EFI_HANDLE)(UINTN)0x5678;return EFI_DEVICE_ERROR;}
  *H=(EFI_HANDLE)(UINTN)0x5678;Published=Candidate;return EFI_SUCCESS;
}
STATIC EFI_BOOT_SERVICES Bs={.LocateProtocol=Locate,.CreateEventEx=Event,.CloseEvent=Close,
  .InstallMultipleProtocolInterfaces=Install,.RaiseTPL=Raise,.RestoreTPL=Restore};
STATIC EFI_DXE_SERVICES Ds={.GetMemorySpaceDescriptor=Gcd};

/* Reuse the unchanged independently qualified Android DPU6 register seed. */
extern VOID BootReferenceSeed(UINT32,VOID (*)(UINT32,UINT32,UINT32),CR_MDSS_BOOT_DMA_OBSERVATION *);
STATIC VOID KonaSeed(UINT32 W,UINT32 O,UINT32 V){
  CONST UINT64 Bases[]={0xae00000,0xae01000,0xaeb0000,0xae94000,0xae94400,0xae96000,0xae96400,0xaeac000};
  assert(W<ARRAY_SIZE(Bases));*Reg(Bases[W]+O)=V;
}
STATIC VOID Seed8350(VOID){
  /* Literal public DPU7/HDK DT route; independent from the driver/parser. */
  UINT64 C=0xae16000,F=0xae25000,I=0xae36000;
  *Reg(0xae00000)=0x70000000;*Reg(C+0x14)=0xf0000000;*Reg(C+0xf4)=2;
  *Reg(C)=0x1040000;*Reg(C+0xfc)=1;*Reg(0xae45004)=0x04380780;
  *Reg(F)=*Reg(F+0xc)=0x04380780;*Reg(F+0x14)=(UINT32)FrameAddress;
  *Reg(F+0x24)=7680;*Reg(F+0x30)=0x236ff;*Reg(F+0x34)=0x03020001;*Reg(F+0x38)=0x80000000;
  for(UINT32 J=0;J<3;++J)*Reg(F+0x108+J*0x10)=0x04380780;
  *Reg(I)=1;*Reg(I+0x26c)=1;*Reg(I+8)=0x0898002c;*Reg(I+0xc)=2475000;
  *Reg(I+0x14)=11000;*Reg(I+0x1c)=90200;*Reg(I+0x24)=2466199;
  *Reg(I+0x3c)=*Reg(I+0x64)=0x083f00c0;*Reg(I+0x60)=0x10;*Reg(I+0x90)=0x213f;*Reg(I+0xa8)=3;
  *Reg(0xae94000)=0x20050000;*Reg(0xae94004)=0x1f3;*Reg(0xae94010)=0x130;
  *Reg(0xae94024)=0x084000c0;*Reg(0xae94028)=0x04610029;*Reg(0xae9402c)=0x04640897;
  *Reg(0xae940ac)=0x10000000;*Reg(0xae94424)=0x7f;*Reg(0xae9443c)=1;
  *Reg(0xae94ab0)=1;*Reg(0xae94414)=0x31;*Reg(0xae9441c)=1;*Reg(0xae944a0)=0x1f;
  *Reg(0xae94434)=0x21;*Reg(0xae94438)=0x84;*Reg(0xae944a4)=0x60;*Reg(0xae94540)=1;
  CONST UINT64 Gates[]={0x127004,0x12701c,0x12700c,0x127014,
    0xaf0207c,0xaf0200c,0xaf0201c,0xaf02024,0xaf02028,0xaf0202c,0xaf02004};
  for(UINTN J=0;J<ARRAY_SIZE(Gates);++J)*Reg(Gates[J])=1;
  *Reg(0xaf02110)=0x101;*Reg(0xaf02098)=0x101;*Reg(0xaf02124)=1;
}
STATIC VOID Seed(VOID){
  gBS=&Bs;gDS=&Ds;Progress=1;SmmuStatus=EFI_SUCCESS;
  Frame=mmap((VOID *)(UINTN)FrameAddress,FrameCapacity,PROT_READ|PROT_WRITE,
    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Frame==(VOID *)(UINTN)FrameAddress);
  memset(Frame,0xa5,FrameCapacity);
  if(HDK_TEST_BOARD==8245){CR_MDSS_BOOT_DMA_OBSERVATION Unused;
    BootReferenceSeed(4,KonaSeed,&Unused);*Reg(0xae6bab4)=0x80000001;*Reg(0xae25014)=(UINT32)FrameAddress;
    *Reg(0x10b00c)=1;*Reg(0xaf02080)=1;
  }else Seed8350();
  *Reg(0xaf03000)=0x80000000;
}
STATIC VOID CheckPixels(VOID){
  EFI_GRAPHICS_OUTPUT_PROTOCOL *G=Published;assert(G&&mDisplay.Published);
  UINT32 Width=HDK_TEST_BOARD==8245?1080:1920,Height=HDK_TEST_BOARD==8245?2340:1080;
  assert(G->Mode->MaxMode==1&&G->Mode->Info->HorizontalResolution==Width&&G->Mode->Info->VerticalResolution==Height);
  UINTN Pitch=Padding?Width+16:Width;
  assert(G->Mode->Info->PixelsPerScanLine==Pitch&&G->Mode->FrameBufferBase==FrameAddress);
  assert(G->Mode->Info->PixelFormat==PixelBitMask&&G->Mode->Info->PixelInformation.RedMask==0xff0000&&
    G->Mode->Info->PixelInformation.GreenMask==(AlternatePacking?0xff:0xff00)&&
    G->Mode->Info->PixelInformation.BlueMask==(AlternatePacking?0xff00:0xff));
  assert(G->Mode->FrameBufferSize==Pitch*Height*4);
  UINTN N;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=NULL;
  assert(G->QueryMode(G,0,&N,&Info)==EFI_SUCCESS&&N==sizeof(*Info));FreePool(Info);
  assert(G->QueryMode(G,1,&N,&Info)==EFI_UNSUPPORTED);
  assert(G->QueryMode(G,0,NULL,&Info)==EFI_INVALID_PARAMETER);
  assert(G->SetMode(G,1)==EFI_UNSUPPORTED&&((UINT32 *)Frame)[0]==0xa5a5a5a5);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel={0x11,0x22,0x33,0x44};
  assert(G->Blt(G,&Pixel,EfiBltVideoFill,0,0,1,1,2,2,0)==EFI_SUCCESS);
  assert(CachePublishes==1);
  UINT32 Encoded=AlternatePacking?0x00331122:0x00332211;
  assert(((UINT32 *)Frame)[Pitch+1]==Encoded&&((UINT32 *)Frame)[Pitch+2]==Encoded);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL ReadBack={0};
  assert(G->Blt(G,&ReadBack,EfiBltVideoToBltBuffer,1,1,0,0,1,1,0)==EFI_SUCCESS);
  assert(ReadBack.Blue==0x11&&ReadBack.Green==0x22&&ReadBack.Red==0x33);
  assert(CachePublishes==1);
  UINT32 Before=((UINT32 *)Frame)[0];
  assert(G->Blt(G,&Pixel,EfiBltVideoFill,0,0,Width,0,1,1,0)==EFI_INVALID_PARAMETER);
  assert(G->Blt(G,&Pixel,EfiGraphicsOutputBltOperationMax,0,0,0,0,1,1,0)==EFI_INVALID_PARAMETER);
  assert(((UINT32 *)Frame)[0]==Before);
  RuntimeChange=TRUE;assert(G->Blt(G,&Pixel,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_NOT_READY);
  assert(((UINT32 *)Frame)[0]==Before);RuntimeChange=FALSE;
  assert(G->SetMode(G,0)==EFI_SUCCESS);
  assert(CachePublishes==2);
  for(UINTN Y=0;Y<Height;++Y)for(UINTN X=0;X<Pitch;++X)
    assert(((UINT32 *)Frame)[Y*Pitch+X]==(X<Width?0:0xa5a5a5a5));
  assert(HdkBootDisplayUnload(NULL)==EFI_ACCESS_DENIED);
}
int main(int Argc,char **Argv){
  assert(Argc==2);Seed();CONST char *Case=Argv[1];
  if(!strcmp(Case,"no-progress"))ProgressStopped=TRUE;
  else if(!strcmp(Case,"gate"))GateOff=TRUE;
  else if(!strcmp(Case,"route"))RouteInvalid=TRUE;
  else if(!strcmp(Case,"smmu"))SmmuStatus=EFI_ACCESS_DENIED;
  else if(!strcmp(Case,"smmu-pending"))SmmuStatus=1;
  else if(!strcmp(Case,"translated"))Translated=TRUE;
  else if(!strcmp(Case,"physical"))WrongPhysical=TRUE;
  else if(!strcmp(Case,"reservation"))ReservedOff=TRUE;
  else if(!strcmp(Case,"writeback"))WriteBack=TRUE;
  else if(!strcmp(Case,"cpu-map"))CpuMapOff=TRUE;
  else if(!strcmp(Case,"readonly"))ReadOnly=TRUE;
  else if(!strcmp(Case,"partial-install"))InstallFailure=PartialInstall=TRUE;
  else if(!strcmp(Case,"allocation"))FailAllocation=1;
  else if(!strcmp(Case,"reentry"))Reentry=ReentryBlt=TRUE;
  else if(!strcmp(Case,"install"))InstallFailure=TRUE;
  else if(!strcmp(Case,"retained"))InstallFailure=CloseFailure=TRUE;
  else if(!strcmp(Case,"packing")){AlternatePacking=TRUE;*Reg(0xae25034)=0x03020100;}
  else if(!strcmp(Case,"padding")){Padding=TRUE;*Reg(0xae25024)=HDK_TEST_BOARD==8245?4384:7744;}
  EFI_STATUS S=HdkBootDisplayEntry(NULL,NULL);
  if(!strcmp(Case,"success")||!strcmp(Case,"reentry")||!strcmp(Case,"ebs")||
     !strcmp(Case,"packing")||!strcmp(Case,"padding")||!strcmp(Case,"bounds")||!strcmp(Case,"writeback")||!strcmp(Case,"move")){
    assert(S==EFI_SUCCESS&&Published&&Installs==1);CheckPixels();
    if(!strcmp(Case,"bounds")){
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel={1,2,3,0};UINT32 Before=((UINT32 *)Frame)[0];
      assert(Published->Blt(Published,&Pixel,EfiBltVideoFill,0,0,MAX_UINTN,0,1,1,0)==EFI_INVALID_PARAMETER);
      assert(Published->Blt(Published,NULL,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_INVALID_PARAMETER);
      assert(Published->Blt(Published,&Pixel,EfiBltBufferToVideo,0,0,0,0,2,1,4)==EFI_INVALID_PARAMETER);
      assert(Published->Blt(Published,&Pixel,EfiBltBufferToVideo,MAX_UINTN,0,0,0,1,1,0)==EFI_INVALID_PARAMETER);
      assert(((UINT32 *)Frame)[0]==Before);
    }
    if(!strcmp(Case,"move")){
      UINTN Pitch=mDisplay.Info.PixelsPerScanLine,Height=mDisplay.Info.VerticalResolution;
      UINT32 *Pixels=Frame;Pixels[0]=0x00112233;
      UINT32 After=Pixels[Pitch*Height];
      assert(Published->Blt(Published,NULL,EfiBltVideoToVideo,0,0,0,Height-1,1,1,0)==EFI_SUCCESS);
      assert(Pixels[(Height-1)*Pitch]==0x00112233&&Pixels[Pitch*Height]==After);
      Pixels[0]=1;Pixels[Pitch]=2;Pixels[2*Pitch]=3;
      assert(Published->Blt(Published,NULL,EfiBltVideoToVideo,0,0,0,1,1,2,0)==EFI_SUCCESS);
      assert(Pixels[Pitch]==1&&Pixels[2*Pitch]==2);
      assert(Published->Blt(Published,NULL,EfiBltVideoToVideo,0,1,0,0,1,2,0)==EFI_SUCCESS);
      assert(Pixels[0]==1&&Pixels[Pitch]==2);
    }
    if(!strcmp(Case,"ebs")){UINTN BeforeReads=Reads,BeforeCalls=BootCalls;
      ExitNotify((EFI_EVENT)(UINTN)0x1234,ExitContext);PostExit=TRUE;
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel={1,2,3,0};
      assert(Published->Blt(Published,&Pixel,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_ACCESS_DENIED);
      assert(Published->SetMode(Published,0)==EFI_ACCESS_DENIED);
      UINTN N;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *I;
      assert(Published->QueryMode(Published,0,&N,&I)==EFI_ACCESS_DENIED);
      assert(Reads==BeforeReads&&BootCalls==BeforeCalls&&HdkBootDisplayUnload(NULL)==EFI_ACCESS_DENIED);
    }
  }else if(!strcmp(Case,"retained")||!strcmp(Case,"partial-install")){
    assert(S==EFI_SUCCESS&&!Published&&mDisplay.ExitEvent&&mDisplay.Blt&&HdkBootDisplayUnload(NULL)==EFI_ACCESS_DENIED);
    FRAME_BUFFER_CONFIGURE *B=mDisplay.Blt;EFI_EVENT E=mDisplay.ExitEvent;UINTN A=Allocations;
    assert(HdkBootDisplayEntry(NULL,NULL)==EFI_ALREADY_STARTED);
    assert(mDisplay.Blt==B&&mDisplay.ExitEvent==E&&Allocations==A);
    free(B);mDisplay.Blt=NULL;
  }else{
    assert(EFI_ERROR(S)&&!Published&&!mDisplay.ExitEvent&&!mDisplay.Blt&&HdkBootDisplayUnload(NULL)==EFI_SUCCESS);
    if(!strcmp(Case,"no-progress"))assert(S==EFI_TIMEOUT&&Delays==12&&Installs==0);
    if(!strcmp(Case,"install"))assert(Installs==1&&Closes==1&&Allocations==Frees);
  }
  if(mDisplay.Blt)free(mDisplay.Blt);assert(munmap(Frame,FrameCapacity)==0);
  printf("PASS HDK%u actual GOP/route/BLT lifecycle: %s\n",HDK_TEST_BOARD,Case);return 0;
}
