/* Actual DXE/parser, with independent sparse memory ownership model. MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#undef NULL
#ifndef CR_RAM_DXE_SOURCE
#define CR_RAM_DXE_SOURCE "../Driver/RamPartitionCrDxe/RamPartitionCrDxe.c"
#endif
#include <Uefi.h>
#include <Library/DxeMemoryProtectionHobLib.h>
#include CR_RAM_DXE_SOURCE

DXE_MEMORY_PROTECTION_SETTINGS gDxeMps;
STATIC BOOLEAN TestNx;
STATIC UINT64 ExpectedAttributes;

EFI_STATUS CrRamTestDmaCapabilities(UINT64 Base,UINT64 Length,UINT64 Capabilities,UINT64 Attributes);

EFI_GUID gEfiSmemCrProtocolGuid=EFI_SMEM_CR_PROTOCOL_GUID;
EFI_GUID gEfiRamPartitionProtocolGuid=EFI_RAMPARTITION_PROTOCOL_GUID;
EFI_GUID gEfiCrRamRangeProtocolGuid=EFI_CR_RAM_RANGE_PROTOCOL_GUID;
STATIC EFI_CR_RAM_RANGE_PROTOCOL *RangePublished;
STATIC EFI_BOOT_SERVICES Boot;
STATIC EFI_DXE_SERVICES Dxe;
EFI_BOOT_SERVICES *gBS=&Boot;
EFI_DXE_SERVICES *gDS=&Dxe;
STATIC EFI_SMEM_CR_PROTOCOL Smem;
STATIC EFI_RAMPARTITION_PROTOCOL *Published;
STATIC UINT8 Wire[CR_RAM_PARTITION_MAX_WIRE_SIZE];
STATIC UINT32 WireSize,Step,FailAt,Adds,Maps,Attributes;
STATIC UINT64 MappedBase,MappedLength,AddedBytes;
STATIC EFI_TPL Tpl;
STATIC UINTN PhysicalBits;
STATIC UINT64 Tcr;
STATIC EFI_STATUS GetStatus;
STATIC BOOLEAN BadDescriptor,CommitPublication;
STATIC EFI_STATUS PublicationStatus;
typedef struct {UINT64 Base,Length;EFI_GCD_MEMORY_TYPE Type;UINT64 Capabilities,Attributes;} REGION;
STATIC REGION Owned[64];STATIC UINTN OwnedCount;
STATIC ARM_MEMORY_REGION_DESCRIPTOR_EX Platform[128];
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;(VOID)Format;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"%s:%zu:%s\n",F,(size_t)L,D);abort();}
ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap(VOID){return Platform;}
UINTN EFIAPI ArmGetPhysicalAddressBits(VOID){return PhysicalBits;}
UINT64 EFIAPI ArmGetTCR(VOID){return Tcr;}
STATIC EFI_STATUS Check(VOID){return ++Step==FailAt?EFI_DEVICE_ERROR:EFI_SUCCESS;}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){assert(New>=Tpl);EFI_TPL Old=Tpl;Tpl=New;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){assert(Old<=Tpl);Tpl=Old;}
STATIC EFI_STATUS EFIAPI GetItem(EFI_SMEM_CR_PROTOCOL *This,UINT32 Host,UINT32 Item,UINT32 *Size,VOID **Address){
  assert(This==&Smem&&Host==SMEM_HOST_ANY&&Item==402);*Address=NULL;*Size=0;
  if(GetStatus!=EFI_SUCCESS)return GetStatus;*Size=WireSize;*Address=Wire;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){
  assert(Guid==&gEfiSmemCrProtocolGuid&&!Registration);*Out=&Smem;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,...){
  EFI_STATUS S=Check();if(S==EFI_SUCCESS)S=PublicationStatus;
  if(S!=EFI_SUCCESS&&!CommitPublication)return S;
  va_list A;va_start(A,Handle);assert(va_arg(A,EFI_GUID *)==&gEfiRamPartitionProtocolGuid);
  Published=va_arg(A,EFI_RAMPARTITION_PROTOCOL *);assert(va_arg(A,EFI_GUID *)==&gEfiCrRamRangeProtocolGuid);
  RangePublished=va_arg(A,EFI_CR_RAM_RANGE_PROTOCOL *);assert(!va_arg(A,VOID *));va_end(A);*Handle=(VOID *)1;return S;
}
STATIC EFI_STATUS EFIAPI Describe(EFI_PHYSICAL_ADDRESS Base,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  EFI_STATUS S=Check();if(S!=EFI_SUCCESS)return S;memset(D,0,sizeof(*D));
  if(BadDescriptor)return EFI_SUCCESS;
  UINT64 Next=1ULL<<36;
  for(UINTN I=0;I<OwnedCount;++I){
    REGION *R=&Owned[I];if(Base>=R->Base&&Base-R->Base<R->Length){D->BaseAddress=R->Base;D->Length=R->Length;D->GcdMemoryType=R->Type;D->Capabilities=R->Capabilities;D->Attributes=R->Attributes;return EFI_SUCCESS;}
    if(R->Base>Base)Next=MIN(Next,R->Base);
  }
  if(Base>=Next)return EFI_NOT_FOUND;
  D->BaseAddress=Base;D->Length=Next-Base;D->GcdMemoryType=EfiGcdMemoryTypeNonExistent;return EFI_SUCCESS;
}
EFI_STATUS ArmSetMemoryAttributes(EFI_PHYSICAL_ADDRESS Base,UINT64 Length,UINT64 Attr,UINT64 Mask){
  assert(Tpl==TPL_NOTIFY&&Attr==ExpectedAttributes&&!Mask);
  EFI_STATUS S=Check();if(S!=EFI_SUCCESS)return S;
  ++Maps;MappedBase=Base;MappedLength=Length;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Add(EFI_GCD_MEMORY_TYPE Type,EFI_PHYSICAL_ADDRESS Base,UINT64 Length,UINT64 Caps){
  assert(Tpl==TPL_NOTIFY&&Type==EfiGcdMemoryTypeSystemMemory&&(Caps&(EFI_MEMORY_WB|EFI_MEMORY_XP))==(EFI_MEMORY_WB|EFI_MEMORY_XP));
  assert(Maps==Adds+1&&Base==MappedBase&&Length==MappedLength&&Length<=0x80000000ULL);
  for(UINTN I=0;I<OwnedCount;++I)assert(Base>=Owned[I].Base+Owned[I].Length||Base+Length<=Owned[I].Base);
  for(UINTN I=0;Platform[I].Length;++I)assert(Base>=Platform[I].Address+Platform[I].Length||Base+Length<=Platform[I].Address);
  /* The unavailable tail of both banks must never be made writable RAM. */
  assert((Base>=0x80000000ULL&&Base+Length<=0x81800000ULL)||
         (Base>=0x800000000ULL&&Base+Length<=0x8e0000000ULL));
  EFI_STATUS S=Check();if(S!=EFI_SUCCESS)return S;
  Owned[OwnedCount++]=(REGION){Base,Length,Type,Caps,0};++Adds;AddedBytes+=Length;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetAttributes(EFI_PHYSICAL_ADDRESS Base,UINT64 Length,UINT64 Attr){
  assert(Tpl==TPL_NOTIFY&&Base==MappedBase&&Length==MappedLength&&Attr==ExpectedAttributes);
  EFI_STATUS S=Check();if(S!=EFI_SUCCESS)return S;
  REGION *R=&Owned[OwnedCount-1];assert(R->Base==Base&&R->Length==Length&&(R->Capabilities&Attr)==Attr);
  R->Attributes=Attr;++Attributes;return EFI_SUCCESS;
}
STATIC VOID W32(UINTN Offset,UINT32 V){memcpy(Wire+Offset,&V,4);}
STATIC VOID W64(UINTN Offset,UINT64 V){memcpy(Wire+Offset,&V,8);}
STATIC VOID Bank(UINTN Index,UINT64 Base,UINT64 Length,UINT64 Available){
  UINTN O=24+Index*72;W64(O+16,Base);W64(O+24,Length);W32(O+32,1);W32(O+36,14);W32(O+40,1);W32(O+44,1);W64(O+64,Available);
}
STATIC VOID Setup(VOID){
  gDxeMps=(DXE_MEMORY_PROTECTION_SETTINGS)DXE_MEMORY_PROTECTION_SETTINGS_OFF;
  gDxeMps.NxProtectionPolicy.Fields.EfiConventionalMemory=TestNx;
  gDxeMps.NxProtectionPolicy.Fields.EfiBootServicesData=TestNx;
  ExpectedAttributes=EFI_MEMORY_WB|(TestNx?EFI_MEMORY_XP:0);
  mRamInventoryReady=FALSE;mRamProtocolHandle=NULL;RangePublished=NULL;
  memset(&mRam,0,sizeof(mRam));memset(Wire,0,sizeof(Wire));memset(Platform,0,sizeof(Platform));
  W32(0,CR_RAM_PARTITION_MAGIC1);W32(4,CR_RAM_PARTITION_MAGIC2);W32(8,2);W32(16,2);WireSize=24+144;
  Bank(0,0x80000000,0x02000000,0x01800000);Bank(1,0x800000000,0x100000000,0xe0000000);
  Platform[0]=(ARM_MEMORY_REGION_DESCRIPTOR_EX){.Address=0x80000000,.Length=0x100000,.HobOption=AddMem};
  Platform[1]=(ARM_MEMORY_REGION_DESCRIPTOR_EX){.Address=0x80400000,.Length=0x100000,.HobOption=NoHob};
  Platform[2]=(ARM_MEMORY_REGION_DESCRIPTOR_EX){.Address=0x80480000,.Length=0x180000,.HobOption=AllocOnly};
  Platform[3]=(ARM_MEMORY_REGION_DESCRIPTOR_EX){.Address=0x830000000,.Length=0x10000000,.HobOption=AddMem};
  Owned[0]=(REGION){0x80900000,0x200000,EfiGcdMemoryTypeReserved,0,0};
  Owned[1]=(REGION){0x81000000,0x100000,EfiGcdMemoryTypeMemoryMappedIo,0,0};OwnedCount=2;
  Boot=(EFI_BOOT_SERVICES){.LocateProtocol=Locate,.InstallMultipleProtocolInterfaces=Install,.RaiseTPL=Raise,.RestoreTPL=Restore};
  Dxe=(EFI_DXE_SERVICES){.GetMemorySpaceDescriptor=Describe,.AddMemorySpace=Add,.SetMemorySpaceAttributes=SetAttributes};
  Smem=(EFI_SMEM_CR_PROTOCOL){.Revision=EFI_SMEM_CR_PROTOCOL_REVISION,.GetItem=GetItem};
  Step=FailAt=Adds=Maps=Attributes=0;AddedBytes=0;Published=NULL;Tpl=TPL_APPLICATION;PhysicalBits=36;Tcr=28;
  GetStatus=PublicationStatus=EFI_SUCCESS;BadDescriptor=CommitPublication=FALSE;
}
STATIC VOID TestPublication(BOOLEAN Nx){
  TestNx=Nx;
  Setup();assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Published);
  assert(RangePublished&&RangePublished->Revision==EFI_CR_RAM_RANGE_PROTOCOL_REVISION);
  assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x80000000,0x01800000)==EFI_SUCCESS);
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x80000000,0x02000000)==EFI_NOT_FOUND);
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x817ff000,0x2000)==EFI_NOT_FOUND);
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x800000000,0xe0000000)==EFI_SUCCESS);
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x800000000,0xe0001000)==EFI_NOT_FOUND);
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x80000000,0)==EFI_INVALID_PARAMETER);
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x80000001,4096)==EFI_INVALID_PARAMETER);
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,MAX_UINT64-4095,4096)==EFI_INVALID_PARAMETER);
  assert(RangePublished->CheckAppsReadWriteRange(NULL,0x80000000,4096)==EFI_INVALID_PARAMETER);
  UINT64 Available=mRam.Banks[0].AvailableLength;mRam.Banks[0].AvailableLength=0;
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x80000000,4096)==EFI_NOT_FOUND);
  mRam.Banks[0].AvailableLength=Available;mRam.Banks[0].Attribute=0;
  assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x80000000,4096)==EFI_NOT_FOUND);
  mRam.Banks[0].Attribute=CR_RAM_PARTITION_READWRITE;
  assert(AddedBytes==0x1200000ULL+0xd0000000ULL&&Adds==Maps&&Adds==Attributes&&Tpl==TPL_APPLICATION);
  /* Feed every actual low/high RAM publication to the actual SMMU allocator.
   * Initial cacheability and execution permissions must stay unchanged. */
  EFI_STATUS PublishedDmaStatus=EFI_SUCCESS;UINTN HighChecks=0,LowChecks=0;
  for(UINTN I=2;I<OwnedCount;++I){
    assert(Owned[I].Attributes==ExpectedAttributes);
    EFI_STATUS DmaStatus=CrRamTestDmaCapabilities(Owned[I].Base,Owned[I].Length,
                                                 Owned[I].Capabilities,Owned[I].Attributes);
    if(DmaStatus!=EFI_SUCCESS)PublishedDmaStatus=DmaStatus;
    if(Owned[I].Base>MAX_UINT32)++HighChecks;else ++LowChecks;
  }
  assert(HighChecks&&LowChecks);
  assert(PublishedDmaStatus==EFI_SUCCESS);
  UINT32 Steps=Step,Count=0;assert(Published->GetRamPartitions(Published,NULL,&Count)==EFI_BUFFER_TOO_SMALL&&Count==2);
  RamPartitionEntry Entries[3];memset(Entries,0xa5,sizeof(Entries));Count=1;
  assert(Published->GetRamPartitions(Published,Entries,&Count)==EFI_BUFFER_TOO_SMALL&&Entries[0].Base==0xa5a5a5a5a5a5a5a5ULL);
  Count=3;assert(Published->GetRamPartitions(Published,Entries,&Count)==EFI_SUCCESS&&Count==2&&Entries[1].AvailableLength==0xe0000000);
  UINT32 Major,Minor,V;assert(Published->GetRamPartitionVersion(Published,&Major,&Minor)==EFI_SUCCESS&&Major==2&&!Minor);
  assert(Published->GetHighestBankBit(Published,&V)==EFI_UNSUPPORTED&&V==0);
  assert(Published->GetMinPasrSize(Published,&V)==EFI_UNSUPPORTED);
  for(UINT32 I=1;I<=Steps;++I){Setup();FailAt=I;assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR&&!Published&&Tpl==TPL_APPLICATION);}
  Setup();GetStatus=EFI_NOT_FOUND;assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_NOT_FOUND&&!Maps&&!Adds);
  Setup();W32(0,0);assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_COMPROMISED_DATA&&!Maps&&!Adds);
  Setup();Bank(1,0x900000000,0x100000000,0xe0000000);Tcr=29;
  assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_UNSUPPORTED&&!Maps&&!Adds);
  Setup();Platform[1].Length=3;assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_COMPROMISED_DATA&&!Maps&&!Adds);
  Setup();BadDescriptor=TRUE;assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_COMPROMISED_DATA&&!Maps&&!Adds);
  Setup();W64(24+64,0);W32(24+72+32,0);
  assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Published&&!Maps&&!Adds);
  Setup();W32(8,3);Wire[24+56]=Wire[24+72+56]=34;W32(24+60,256);W32(24+72+60,256);
  assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS);
  assert(Published->GetHighestBankBit(Published,&V)==EFI_SUCCESS&&V==34);
  assert(Published->GetMinPasrSize(Published,&V)==EFI_SUCCESS&&V==256);
  mRam.Banks[1].MinPasrSize=512;assert(Published->GetMinPasrSize(Published,&V)==EFI_UNSUPPORTED&&V==0);
  CONST EFI_STATUS PublicationFailures[]={EFI_WARN_UNKNOWN_GLYPH,EFI_DEVICE_ERROR};
  for(UINTN I=0;I<ARRAY_SIZE(PublicationFailures);++I){
    Setup();PublicationStatus=PublicationFailures[I];CommitPublication=TRUE;
    assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Published&&RangePublished&&mRamProtocolHandle);
    assert(RangePublished->CheckAppsReadWriteRange(RangePublished,0x80000000,4096)==EFI_SUCCESS);
    assert(RamPartitionCrDxeEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);
    Setup();PublicationStatus=PublicationFailures[I];
    assert(RamPartitionCrDxeEntryPoint(NULL,NULL)!=EFI_SUCCESS&&!Published&&!RangePublished&&!mRamProtocolHandle);
  }
  puts("RAM DXE: full SMEM parser, >4GiB sparse banks, unavailable tails, GCD/platform exclusions, MMU-before-allocation, actual SMMU WC allocation/restore and every publication failure PASS");
  printf("RAM DXE policy: NX=%u attributes=0x%llx PASS\n",Nx,(unsigned long long)ExpectedAttributes);
}
int main(void){
  TestPublication(FALSE);
  TestPublication(TRUE);
  return 0;
}
