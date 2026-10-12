/* Actual SEC capture -> immutable HOB -> DAL UFS overlay integration.
 * Firmware services alone are controlled; parser/libfdt, policy, registry,
 * target profiles and the capture/publication/lookup implementations are real.
 * SPDX-License-Identifier: MIT */
#include <PiPei.h>
#include <Pi/PiHob.h>
#include <Library/UfsPowerPolicyCrLib.h>
#include <Library/PlatformMemoryMapLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/AndroidBootCrLib.h>
#include <Guid/CrBootData.h>
#include <Library/MemoryAllocationLib.h>
#include <sys/mman.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>

#include "../Library/BootModeCaptureCrLib/BootModeCaptureCrLib.c"
#include "../Driver/CrDALDxe/CrDALDxe.c"

extern CONST CR_TARGET_UFS_CONTEXT *CrTargetGetUfsContext8450(VOID);
extern CONST CR_TARGET_UFS_CONTEXT *CrTargetGetUfsContext8250(VOID);
EFI_GUID gCrBootDataGuid=CR_BOOT_DATA_HOB_GUID;
EFI_GUID gEfiCrDalProtocolGuid;
EFI_GUID gEfiCrFdtProtocolGuid=EFI_CR_FDT_PROTOCOL_GUID;
STATIC EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
STATIC ARM_MEMORY_REGION_DESCRIPTOR_EX Regions[5];
STATIC BOOLEAN FailHob, InstallCommit;
STATIC EFI_STATUS InstallStatus;
STATIC UINT32 Installs, Checks, PoolCount;
STATIC CR_DAL_DEVICE_MANIFEST Manifest;
STATIC CR_DAL_DEVICE_INFO Devices[2];
STATIC UINT32 GpioMetadata=0x6372616e;
/* Explicit simulated physical ranges: use ASan low-memory-safe mappings rather
 * than 0x89000000, which is ASan shadow memory on this host. No parser receives
 * a host-stack pointer disguised as an Android physical initrd address. */
#define FDT_PHYSICAL 0x20000000U
#define INITRD_PHYSICAL 0x21000000U
STATIC UINT8 *Blob,*Initrd;
STATIC _Alignas(8) UINT8 Original[CR_UFS_POWER_FDT_MAX];
STATIC _Alignas(4096) UINT8 Heap[CR_BOOT_DATA_FDT_MAX+0x10000+0x40000+EFI_PAGE_SIZE];
STATIC EFI_HOB_HANDOFF_INFO_TABLE *Phit;
STATIC VOID *RelocatedHobs;
STATIC CR_BOOT_DATA_HOB *Record;
STATIC UINTN BlobBytes,InitrdBytes,HobBytes,HeapFreeTop;

VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) { return memmove(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D,UINTN N) { return memset(D,0,N); }
EFI_GUID *EFIAPI CopyGuid(EFI_GUID *D,CONST EFI_GUID *S) {return memcpy(D,S,sizeof(*D));}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) { return memcmp(A,B,N); }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) { return strcmp(A,B); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { (VOID)Level; (VOID)Format; }
VOID *EFIAPI AllocatePool(UINTN N) {VOID *P=calloc(1,N);assert(P);++PoolCount;return P;}
VOID EFIAPI FreePool(VOID *P) {assert(P&&PoolCount);--PoolCount;free(P);}
ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap(VOID) { return Regions; }
VOID *EFIAPI GetHobList(VOID) {return Phit;}
STATIC VOID EndHobs(VOID) {
  EFI_HOB_GENERIC_HEADER *H=(VOID *)(Heap+HobBytes);
  *H=(EFI_HOB_GENERIC_HEADER){EFI_HOB_TYPE_END_OF_HOB_LIST,sizeof(*H),0};
  Phit->EfiEndOfHobList=(UINTN)H;Phit->EfiFreeMemoryBottom=(UINTN)(H+1);
}
STATIC VOID InitHobs(VOID) {
  Phit=(VOID *)Heap;memset(Phit,0,sizeof(*Phit));
  Phit->Header=(EFI_HOB_GENERIC_HEADER){EFI_HOB_TYPE_HANDOFF,sizeof(*Phit),0};
  Phit->Version=EFI_HOB_HANDOFF_TABLE_VERSION;
  Phit->EfiMemoryBottom=(UINTN)Heap;Phit->EfiMemoryTop=(UINTN)Heap+sizeof(Heap);
  Phit->EfiFreeMemoryTop=HeapFreeTop;HobBytes=sizeof(*Phit);Record=NULL;EndHobs();
}
VOID *CreateHob(UINT16 Type,UINT16 Size) {
  if(FailHob)return NULL;
  Size=(UINT16)((Size+7)&~7U);assert(Size>=8&&HobBytes+Size+8<0x10000);
  assert((UINTN)Heap+HobBytes+Size+8<=Phit->EfiFreeMemoryTop);
  EFI_HOB_GENERIC_HEADER *H=(VOID *)(Heap+HobBytes);memset(H,0,Size);
  *H=(EFI_HOB_GENERIC_HEADER){Type,Size,0};HobBytes+=Size;EndHobs();return H;
}
VOID *EFIAPI BuildGuidDataHob(CONST EFI_GUID *Guid,VOID *Data,UINTN Size) {
  assert(!memcmp(Guid,&gCrBootDataGuid,sizeof(*Guid))&&Size==sizeof(CR_BOOT_DATA_HOB));
  EFI_HOB_GUID_TYPE *H=CreateHob(EFI_HOB_TYPE_GUID_EXTENSION,(UINT16)(sizeof(*H)+Size));
  if(!H)return NULL;H->Name=*Guid;Record=(VOID *)(H+1);memcpy(Record,Data,Size);return Record;
}
STATIC VOID MemoryHobs(VOID) {
  /* MemoryPeim publishes the underlying conventional RAM in addition to the
     SEC named suballocation. Both producers are modeled with real HOB bytes. */
  EFI_HOB_RESOURCE_DESCRIPTOR *R=CreateHob(EFI_HOB_TYPE_RESOURCE_DESCRIPTOR,sizeof(*R));
  assert(R);R->ResourceType=EFI_RESOURCE_SYSTEM_MEMORY;
  R->ResourceAttribute=EFI_RESOURCE_ATTRIBUTE_PRESENT|EFI_RESOURCE_ATTRIBUTE_INITIALIZED|EFI_RESOURCE_ATTRIBUTE_TESTED;
  R->PhysicalStart=(UINTN)Heap;R->ResourceLength=sizeof(Heap);
  EFI_HOB_MEMORY_ALLOCATION *M=CreateHob(EFI_HOB_TYPE_MEMORY_ALLOCATION,sizeof(*M));
  assert(M);M->AllocDescriptor.MemoryBaseAddress=(UINTN)Heap;
  M->AllocDescriptor.MemoryLength=sizeof(Heap);M->AllocDescriptor.MemoryType=EfiConventionalMemory;
}
STATIC VOID Capture(VOID) {
  BootModeCapture(FDT_PHYSICAL,(UINTN)Heap,&HeapFreeTop);
  InitHobs();BootModePublish();BOOLEAN WasFail=FailHob;FailHob=FALSE;MemoryHobs();FailHob=WasFail;
}
EFI_STATUS CrTargetGetDeviceManifest(CONST CR_DAL_DEVICE_MANIFEST **Out)
{ *Out=&Manifest; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,...)
{
  va_list Args; va_start(Args,Handle);
  assert(va_arg(Args,EFI_GUID *)==&gEfiCrDalProtocolGuid);
  assert(va_arg(Args,VOID *)==&mCrDalProtocol);
  assert(va_arg(Args,EFI_GUID *)==&gEfiCrFdtProtocolGuid);
  assert(va_arg(Args,VOID *)==CrBootFdtProtocol());assert(va_arg(Args,VOID *)==NULL);
  va_end(Args); ++Installs;
  if (InstallCommit) *Handle=(EFI_HANDLE)0x713;
  return InstallStatus;
}
STATIC VOID Reset(CONST CR_TARGET_UFS_CONTEXT *Target)
{
  CrBootFdtDiscard();assert(!PoolCount);FailHob=FALSE;InstallCommit=TRUE;
  free(RelocatedHobs);RelocatedHobs=NULL;
  /* Each Reset models a new firmware boot, not production recapture. */
  mCaptured=FALSE;mPublished=FALSE;
  InstallStatus=EFI_SUCCESS; Installs=0; mCrDalHandle=NULL; mManifest=NULL;
  memset(&mPublishedManifest,0,sizeof(mPublishedManifest));
  memset(&mPublishedUfs,0,sizeof(mPublishedUfs)); memset(&mUfsPower,0,sizeof(mUfsPower));
  memset(Heap,0,sizeof(Heap));HeapFreeTop=((UINTN)Heap+sizeof(Heap))&~(UINTN)EFI_PAGE_MASK;
  InitHobs();memset(Regions,0,sizeof(Regions));
  strcpy(Regions[0].Name,"DT BLOB"); Regions[0].ResourceType=MEM_RES;
  Regions[0].Address=FDT_PHYSICAL;Regions[0].Length=CR_UFS_POWER_FDT_MAX;Regions[0].ArmAttributes=WRITE_BACK;
  strcpy(Regions[1].Name,"RAM Partition");Regions[1].ResourceType=SYS_MEM;
  Regions[1].Address=INITRD_PHYSICAL;Regions[1].Length=CR_BOOT_DATA_TAIL_MAX+4096;Regions[1].ArmAttributes=WRITE_BACK;
  strcpy(Regions[2].Name,"DXE Heap");Regions[2].ResourceType=SYS_MEM;
  Regions[2].Address=(UINTN)Heap;Regions[2].Length=sizeof(Heap);Regions[2].ArmAttributes=WRITE_BACK;Regions[2].MemoryType=EfiConventionalMemory;
  Regions[2].HobOption=AddMem;Regions[2].ResourceAttribute=SYS_MEM_CAP;
  Regions[0].ResourceAttribute=Regions[1].ResourceAttribute=SYS_MEM_CAP;
  UINT32 Attributes=CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE|CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES|CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE;
  Devices[0]=(CR_DAL_DEVICE_INFO){.Size=sizeof(Devices[0]),.Revision=CR_DAL_DEVICE_INFO_REVISION,
    .Type=CrDalDeviceGpio,.DataRevision=CR_DAL_DATA_REVISION_1,.DataSize=sizeof(GpioMetadata),.Data=&GpioMetadata,.Attributes=Attributes};
  Devices[1]=(CR_DAL_DEVICE_INFO){.Size=sizeof(Devices[1]),.Revision=CR_DAL_DEVICE_INFO_REVISION,
    .Type=CrDalDeviceUfs,.DataRevision=CR_DAL_DATA_REVISION_1,.DataSize=sizeof(*Target),.Data=Target,.Attributes=Attributes};
  Manifest=(CR_DAL_DEVICE_MANIFEST){.Size=sizeof(Manifest),.Revision=CR_DAL_DEVICE_MANIFEST_REVISION,.DeviceCount=2,.Devices=Devices};
  Services.InstallMultipleProtocolInterfaces=Install;
}
STATIC VOID Load(CONST CHAR8 *Path)
{
  assert(!mprotect(Blob,CR_UFS_POWER_FDT_MAX,PROT_READ|PROT_WRITE));
  assert(!mprotect(Initrd,CR_BOOT_DATA_TAIL_MAX+4096,PROT_READ|PROT_WRITE));
  FILE *F=fopen(Path,"rb"); assert(F); BlobBytes=fread(Original,1,sizeof(Original),F); fclose(F);
  assert(BlobBytes>=40 && BlobBytes==(UINTN)fdt_totalsize(Original));
  assert(fdt_open_into(Original,Blob,CR_UFS_POWER_FDT_MAX)==0);
}
STATIC CR_UFS_POWER_FDT Parsed(VOID)
{
  CR_UFS_POWER_FDT Info; assert(CrUfsPowerFdtParse(Blob,fdt_totalsize(Blob),&Info)==CR_SUCCESS); return Info;
}
STATIC VOID Resolve(CONST CR_TARGET_UFS_CONTEXT *Target,CONST CR_UFS_POWER_FDT *Info,EFI_STATUS Status)
{
  CR_UFS_POWER_RESOLVED Result,Before; memset(&Result,0x5a,sizeof(Result)); Before=Result;
  EFI_STATUS Actual=CrUfsPowerResolve(Target,Info,&Result);
  if (Actual!=Status) fprintf(stderr,"resolve case%u status=%zx expected=%zx\n",Checks,(size_t)Actual,(size_t)Status);
  assert(Actual==Status); if (Status!=EFI_SUCCESS) assert(memcmp(&Result,&Before,sizeof(Result))==0); ++Checks;
}
STATIC VOID Published(BOOLEAN Ufs)
{
  fprintf(stderr,"publish check%u expected%u\n",Checks,Ufs);
  CONST CR_DAL_DEVICE_INFO *D=NULL; UINTN Count=0;
  assert(CrDalDxeEntryPoint(NULL,NULL)==EFI_SUCCESS && Installs==1);
  assert(mCrDalProtocol.GetDeviceCount(&mCrDalProtocol,&Count)==EFI_SUCCESS && Count==(Ufs?2U:1U));
  assert(mCrDalProtocol.GetDeviceInfo(&mCrDalProtocol,CrDalDeviceGpio,0,0,0,&D)==EFI_SUCCESS && D->Data==&GpioMetadata);
  assert(mCrDalProtocol.GetDeviceInfo(&mCrDalProtocol,CrDalDeviceUfs,0,0,0,&D)==(Ufs?EFI_SUCCESS:EFI_NOT_FOUND));
  if (Ufs) assert(D->Data==&mPublishedUfs && CrTargetUfsValid(D->Data));
  assert(CrDalDxeEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED && Installs==1); ++Checks;
}
STATIC VOID GoodHob(CONST CR_UFS_POWER_FDT *Info)
{
  CR_UFS_POWER_FDT Actual=Parsed();assert(!memcmp(&Actual,Info,sizeof(Actual)));
  Capture();assert(Record&&Record->FdtStatus==EFI_SUCCESS);
}
/* Match CoreInitializeGcdServices: AllocateCopyPool copies through the old
 * PHIT free bottom and leaves every physical PHIT field unchanged. The old
 * HOB storage becomes reusable immediately afterwards. */
STATIC VOID RelocateHobs(VOID)
{
  UINTN Bytes=(UINTN)Phit->EfiFreeMemoryBottom-(UINTN)Phit;
  UINTN RecordOffset=Record?(UINTN)Record-(UINTN)Phit:0;
  assert(!RelocatedHobs&&Bytes==HobBytes+sizeof(EFI_HOB_GENERIC_HEADER));
  RelocatedHobs=malloc(Bytes);assert(RelocatedHobs);
  memcpy(RelocatedHobs,Phit,Bytes);
  memset(Phit,0xa5,Bytes);
  Phit=RelocatedHobs;Record=RecordOffset?(VOID *)((UINT8 *)Phit+RecordOffset):NULL;
}
STATIC VOID RelocatedLifecycle(CONST CHAR8 *Path)
{
  Load(Path);Reset(CrTargetGetUfsContext8450());CR_UFS_POWER_FDT Info=Parsed();
  GoodHob(&Info);RelocateHobs();
  assert(!mprotect(Blob,CR_UFS_POWER_FDT_MAX,PROT_NONE));
  Published(TRUE);
  CONST VOID *Copy;UINTN Bytes;EFI_CR_FDT_PROTOCOL *P=CrBootFdtProtocol();
  assert(P->GetBlob(P,&Copy,&Bytes)==EFI_SUCCESS);
  CR_UFS_POWER_FDT Saved;assert(CrUfsPowerFdtParse(Copy,Bytes,&Saved)==CR_SUCCESS);
  assert(!memcmp(&Info,&Saved,sizeof(Info)));++Checks;
}
STATIC VOID Policy(CONST CHAR8 *Path)
{
  CONST CR_TARGET_UFS_CONTEXT *Target=CrTargetGetUfsContext8450(); Load(Path);
  CR_UFS_POWER_FDT Base=Parsed(),Info=Base;
  CR_UFS_POWER_RESOLVED Result;
  assert(CrUfsPowerResolve(Target,&Info,&Result)==EFI_SUCCESS && Result.Count==4);
  for(UINT32 I=0;I<4;++I) assert(strcmp(Result.Rails[I].Name,Target->Rails[I].Name)==0 && Result.Rails[I].VoltageMv==Target->Rails[I].VoltageMv);
  ++Checks;
  CR_UFS_POWER_SUPPLY *V=&Info.Supplies[CrUfsPowerVcc];
  V->MinUv=1704000;V->MaxUv=3544000;V->InitUv=1800000;V->Flags|=CR_UFS_POWER_RANGE_PRESENT|CR_UFS_POWER_INIT_PRESENT|CR_UFS_POWER_VCC_1P8;
  assert(CrUfsPowerResolve(Target,&Info,&Result)==EFI_SUCCESS && Result.Rails[2].VoltageMv==1800 && Result.Sources[2]==1);++Checks;
  Info=Base;V=&Info.Supplies[CrUfsPowerVcc];V->MinUv=1704000;V->MaxUv=3544000;V->InitUv=0;V->Flags&=~CR_UFS_POWER_INIT_PRESENT;
  assert(CrUfsPowerResolve(Target,&Info,&Result)==EFI_SUCCESS && Result.Rails[2].VoltageMv==2504 && Result.Sources[2]==2);++Checks;
  V->Flags|=CR_UFS_POWER_INIT_PRESENT|CR_UFS_POWER_CONSUMER_RANGE;V->InitUv=2496000;V->ConsumerMinUv=2504000;V->ConsumerMaxUv=2950000;
  assert(CrUfsPowerResolve(Target,&Info,&Result)==EFI_SUCCESS && Result.Rails[2].VoltageMv==2504 && Result.Sources[2]==2);++Checks;
  V->InitUv=2505000;Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base; Info.Supplies[CrUfsPowerVcc].Flags|=CR_UFS_POWER_ALWAYS_ON;Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base; Info.Supplies[CrUfsPowerVcc].VoteSet=1;Info.Supplies[CrUfsPowerVcc].Flags|=CR_UFS_POWER_VOTE_SET_PRESENT;Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base; strcpy(Info.Supplies[CrUfsPowerVcc].Resource,"smpa4");Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base; Info.Supplies[CrUfsPowerVcc]=(CR_UFS_POWER_SUPPLY){0};
  assert(CrUfsPowerResolve(Target,&Info,&Result)==EFI_SUCCESS && Result.Count==3 && (Result.InheritedRoles&(1U<<CrUfsPowerVcc)));++Checks;
  Info=Base;Info.Supplies[CrUfsPowerVddHba].ProviderBase++;Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;Info.Supplies[CrUfsPowerVddaPhy]=(CR_UFS_POWER_SUPPLY){0};Resolve(Target,&Info,EFI_COMPROMISED_DATA);
  Info=Base;Info.PhySize=0x1000;Resolve(Target,&Info,EFI_SUCCESS);
  Info=Base;Info.HostBase++;Resolve(Target,&Info,EFI_COMPROMISED_DATA);
  Info=Base;Info.Supplies[CrUfsPowerVddHba]=Info.Supplies[CrUfsPowerVccq];
  assert(CrUfsPowerResolve(Target,&Info,&Result)==EFI_SUCCESS && Result.Count==4);++Checks;
  Info.Supplies[CrUfsPowerVddHba].Flags|=CR_UFS_POWER_CONSUMER_RANGE;
  Info.Supplies[CrUfsPowerVddHba].ConsumerMinUv=1300000;Info.Supplies[CrUfsPowerVddHba].ConsumerMaxUv=1400000;
  Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;Info.VccqParent.MinUv=1400000;Info.VccqParent.MaxUv=1500000;
  Info.VccqParent.InitUv=1400000;Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;strcpy(Info.VccqParent.Resource,"smpb11");Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;Info.VccqParent.VoteSet=1;Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;Info.VccqParent.Flags|=CR_UFS_POWER_ALWAYS_ON;Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;Info.VccqParent=(CR_UFS_POWER_SUPPLY){0};Resolve(Target,&Info,EFI_SUCCESS);
}
STATIC VOID KonaPolicy(CONST CHAR8 *Path)
{
  CONST CR_TARGET_UFS_CONTEXT *Target=CrTargetGetUfsContext8250();Load(Path);
  CR_UFS_POWER_FDT Info=Parsed();CR_UFS_POWER_RESOLVED Result;
  assert(CrUfsPowerResolve(Target,&Info,&Result)==EFI_SUCCESS && Result.Count==4);
  assert(Info.Supplies[CrUfsPowerVccq2].Kind==CrUfsPowerFixedAlwaysOn);
  assert(Result.InheritedRoles&(1U<<CrUfsPowerVccq2));
  for(UINT32 I=0;I<4;++I)assert(strcmp(Result.Rails[I].Name,Target->Rails[I].Name)==0);
  ++Checks;
}
STATIC VOID Lifecycle(CONST CHAR8 *Path)
{
  CONST CR_TARGET_UFS_CONTEXT *Target=CrTargetGetUfsContext8450();Load(Path);CR_UFS_POWER_FDT Info=Parsed();
  Reset(Target);GoodHob(&Info);assert(Record->FdtOriginal==FDT_PHYSICAL&&Record->FdtCopy!=FDT_PHYSICAL);
  memset(Blob,0,CR_UFS_POWER_FDT_MAX);assert(!mprotect(Blob,CR_UFS_POWER_FDT_MAX,PROT_NONE));Published(TRUE);
  CONST VOID *Copy;UINTN CopySize;EFI_CR_FDT_PROTOCOL *P=CrBootFdtProtocol();
  assert(P->GetBlob(P,&Copy,&CopySize)==EFI_SUCCESS&&Copy!=(VOID *)(UINTN)Record->FdtCopy);
  CR_UFS_POWER_FDT Saved;assert(CrUfsPowerFdtParse(Copy,CopySize,&Saved)==CR_SUCCESS&&!memcmp(&Saved,&Info,sizeof(Info)));
  memset((VOID *)(UINTN)Record->FdtCopy,0,(UINTN)Record->FdtSize);
  assert(CrUfsPowerFdtParse(Copy,CopySize,&Saved)==CR_SUCCESS&&!memcmp(&Saved,&Info,sizeof(Info)));++Checks;
  Reset(Target);Published(FALSE); /* absent raw HOB preserves unrelated GPIO */
  Reset(CrTargetGetUfsContext8250());Published(FALSE);
  for(UINT32 I=0;I<7;++I){Load(Path);Reset(Target);GoodHob(&Info);
    if(I==0){CR_BOOT_DATA_HOB Duplicate=*Record;assert(BuildGuidDataHob(&gCrBootDataGuid,&Duplicate,sizeof(Duplicate)));}
    if(I==1)((EFI_HOB_GUID_TYPE *)Record-1)->Header.HobLength--;
    if(I==2)Record->Revision++;
    if(I==3)Record->Size--;
    if(I==4)Record->FdtStatus=EFI_WARN_UNKNOWN_GLYPH;
    if(I==5)Record->FdtStatus=EFI_NOT_READY;
    if(I==6){VOID *Fdt=(VOID *)(UINTN)Record->FdtCopy;
      INT32 N=fdt_node_offset_by_phandle(Fdt,Info.Supplies[0].Phandle);assert(N>=0);N=fdt_parent_offset(Fdt,N);assert(N>=0);
      assert(!fdt_setprop_string(Fdt,N,"qcom,resource-name","smpa4"));}
    Published(FALSE);
  }
  Load(Path);Reset(Target);strcpy(Regions[0].Name,"UEFI Stack");Capture();assert(Record&&Record->FdtStatus!=EFI_SUCCESS);Published(FALSE);
  Load(Path);Reset(Target);Regions[0].Length=40;Capture();assert(Record&&Record->FdtStatus!=EFI_SUCCESS);Published(FALSE);
  Load(Path);Reset(Target);FailHob=TRUE;Capture();assert(!Record);Published(FALSE);
  Load(Path);Reset(Target);GoodHob(&Info);InstallStatus=EFI_DEVICE_ERROR;InstallCommit=FALSE;
  assert(CrDalDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR&&!mCrDalHandle&&!PoolCount);
  InstallStatus=EFI_SUCCESS;InstallCommit=TRUE;Installs=0;Published(TRUE);
  Load(Path);Reset(Target);GoodHob(&Info);InstallStatus=EFI_WARN_UNKNOWN_GLYPH;
  assert(CrDalDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&mCrDalHandle&&Installs==1);
  assert(CrDalDxeEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);++Checks;
}
STATIC VOID Bootconfig(CONST CHAR8 *Path)
{
  Load(Path);Reset(CrTargetGetUfsContext8450());
  INT32 N=fdt_path_offset(Blob,"/chosen");if(N<0)N=fdt_add_subnode(Blob,0,"chosen");assert(N>=0);
  assert(!fdt_setprop_string(Blob,N,"bootargs","androidboot.mode=normal"));
  STATIC CONST CHAR8 Text[]="androidboot.mode = \"charger\"\n";
  UINT32 Size=sizeof(Text)-1,Sum=0;memset(Initrd,0x65,4096);memcpy(Initrd+4096,Text,Size);
  for(UINT32 I=0;I<Size;++I)Sum+=(UINT8)Text[I];
  for(UINT32 I=0;I<4;++I){Initrd[4096+Size+I]=(UINT8)(Size>>(I*8));Initrd[4100+Size+I]=(UINT8)(Sum>>(I*8));}
  memcpy(Initrd+4104+Size,"#BOOTCONFIG\n",12);InitrdBytes=4116+Size;
  N=fdt_path_offset(Blob,"/chosen");assert(!fdt_setprop_u64(Blob,N,"linux,initrd-start",INITRD_PHYSICAL));
  N=fdt_path_offset(Blob,"/chosen");assert(!fdt_setprop_u64(Blob,N,"linux,initrd-end",INITRD_PHYSICAL+InitrdBytes));
  Capture();assert(Record&&Record->FdtStatus==EFI_SUCCESS&&Record->TailStatus==EFI_SUCCESS&&Record->TailSize);
  memset(Blob,0,CR_UFS_POWER_FDT_MAX);memset(Initrd,0,InitrdBytes);
  assert(!mprotect(Blob,CR_UFS_POWER_FDT_MAX,PROT_NONE));
  assert(!mprotect(Initrd,CR_BOOT_DATA_TAIL_MAX+4096,PROT_NONE));RelocateHobs();Published(TRUE);
  EFI_CR_FDT_PROTOCOL *P=CrBootFdtProtocol();CR_ANDROID_BOOT_INFO I;
  assert(P->GetBootInfo(P,&I)==EFI_SUCCESS&&I.Mode==CR_ANDROID_BOOT_CHARGER&&I.Source==CR_ANDROID_BOOT_SOURCE_BOOTCONFIG&&I.FdtAddress==FDT_PHYSICAL);
  memset((VOID *)(UINTN)Record->AllocationBase,0,(UINTN)Record->AllocationSize);
  assert(P->GetBootInfo(P,&I)==EFI_SUCCESS&&I.Mode==CR_ANDROID_BOOT_CHARGER);++Checks;
}
STATIC VOID Hostile(CONST CHAR8 *Kona,CONST CHAR8 *Waipio,CONST CHAR8 *Mode)
{
  CR_UFS_POWER_FDT Info,Before; memset(&Info,0x5a,sizeof(Info));Before=Info;
  if(strcmp(Mode,"fixed-parent")==0){
    Load(Kona);CR_UFS_POWER_FDT R=Parsed();assert(R.Supplies[2].Kind==CrUfsPowerFixedAlwaysOn);
    INT32 Fixed=fdt_node_offset_by_phandle(Blob,R.Supplies[2].Phandle);INT32 Len;
    CONST fdt32_t *Link=fdt_getprop(Blob,Fixed,"vin-supply",&Len);assert(Link && Len==4);
    INT32 Parent=fdt_node_offset_by_phandle(Blob,fdt32_to_cpu(*Link));assert(Parent>=0);
    assert(fdt_setprop_u32(Blob,Parent,"gpio",0x3456)==0);
  }else{Load(Waipio);INT32 Host=fdt_node_offset_by_compatible(Blob,-1,"qcom,ufshc");assert(Host>=0);
    assert(fdt_setprop_u32(Blob,Host,"qcom,vddp-ref-clk-min-microvolt",1300000)==0);
    assert(fdt_setprop_u32(Blob,Host,"qcom,vddp-ref-clk-max-microvolt",1400000)==0);
  }
  CR_STATUS S=CrUfsPowerFdtParse(Blob,fdt_totalsize(Blob),&Info);
  fprintf(stderr,"hostile %s status=%zx\n",Mode,(size_t)S);
  assert(S!=CR_SUCCESS && memcmp(&Info,&Before,sizeof(Info))==0);++Checks;
  Reset(strcmp(Mode,"fixed-parent")==0?CrTargetGetUfsContext8250():CrTargetGetUfsContext8450());
  Capture();assert(Record&&Record->FdtStatus==EFI_SUCCESS);Published(FALSE);
}
STATIC VOID KonaAndroidPolicy(CONST CHAR8 *Path)
{
  CONST CR_TARGET_UFS_CONTEXT *Target=CrTargetGetUfsContext8250();
  Load(Path); CR_UFS_POWER_FDT Base=Parsed(), Info=Base;
  CR_UFS_POWER_RESOLVED R;
  assert(CrUfsPowerResolve(Target,&Info,&R)==EFI_SUCCESS && R.Count==5);
  assert(R.RetainedRailMask==0x11 && !strcmp(R.Rails[4].Name,"smpa4") && R.Rails[4].VoltageMv==1800);
  assert(R.Rails[0].VoltageMv==880 && R.Sources[4]==1);
  Reset(Target);GoodHob(&Info);Published(TRUE);
  assert(mPublishedUfs.RetainedRailMask==0x11 && mPublishedUfs.RailCount==5);
  Info=Base;Info.Supplies[CrUfsPowerVccq2].InitUv=1801000;
  Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;strcpy(Info.Supplies[CrUfsPowerVccq2].Resource,"smpa5");
  Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;Info.Supplies[CrUfsPowerVccq2].Flags|=CR_UFS_POWER_CONSUMER_ALWAYS_ON;
  assert(CrUfsPowerResolve(Target,&Info,&R)==EFI_SUCCESS && R.RetainedRailMask==0x11);
  Info=Base;Info.Supplies[CrUfsPowerVddaPll].Flags|=CR_UFS_POWER_CONSUMER_ALWAYS_ON;
  Resolve(Target,&Info,EFI_UNSUPPORTED);
  Info=Base;Info.Supplies[CrUfsPowerVccq2].VoteSet=1;
  Resolve(Target,&Info,EFI_UNSUPPORTED);
  CR_TARGET_UFS_CONTEXT Bad=*Target;Bad.RetainedRailMask=1U<<5;
  assert(!CrTargetUfsValid(&Bad));
  Reset(Target);Published(FALSE); /* New Kona native path requires copied FDT. */
  ++Checks;
}

int main(int argc,char **argv)
{
  assert(argc==5);assert(sizeof(CR_UFS_POWER_FDT)==544);
  Blob=mmap((VOID *)(UINTN)FDT_PHYSICAL,CR_UFS_POWER_FDT_MAX,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
  Initrd=mmap((VOID *)(UINTN)INITRD_PHYSICAL,CR_BOOT_DATA_TAIL_MAX+4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
  assert(Blob==(VOID *)(UINTN)FDT_PHYSICAL&&Initrd==(VOID *)(UINTN)INITRD_PHYSICAL);
  if(strcmp(argv[3],"normal")==0){RelocatedLifecycle(argv[2]);Policy(argv[2]);KonaPolicy(argv[1]);KonaAndroidPolicy(argv[4]);fprintf(stderr,"policy complete%u\n",Checks);Lifecycle(argv[2]);Bootconfig(argv[2]);}
  else Hostile(argv[1],argv[2],argv[3]);
  CrBootFdtDiscard();assert(!PoolCount);
  free(RelocatedHobs);
  assert(!munmap(Blob,CR_UFS_POWER_FDT_MAX));assert(!munmap(Initrd,CR_BOOT_DATA_TAIL_MAX+4096));
  printf("UFS actual SEC/HOB/import/policy/DAL PASS %u %s\n",Checks,argv[3]);return 0;
}
