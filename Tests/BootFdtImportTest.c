/* Actual DXE importer + MU libfdt + Android parser, controlled allocation/HOB.
 * SPDX-License-Identifier: MIT */
#include <PiDxe.h>
#include <Guid/CrBootData.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UfsPowerFdtCrLib.h>
#include "../Driver/CrDALDxe/BootFdt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
EFI_GUID gCrBootDataGuid=CR_BOOT_DATA_HOB_GUID;
EFI_GUID gEfiCrFdtProtocolGuid=EFI_CR_FDT_PROTOCOL_GUID;
static _Alignas(4096) UINT8 Source[0x211000];
static _Alignas(8) UINT8 Hobs[2048];
static UINTN HobBytes;static UINT32 Checks,Allocations,SourceReads;static BOOLEAN FailAllocation;
static EFI_HOB_HANDOFF_INFO_TABLE *Phit;
static EFI_HOB_MEMORY_ALLOCATION *Allocation;
static CR_BOOT_DATA_HOB *Record;
static EFI_HOB_RESOURCE_DESCRIPTOR *Resource;
static UINTN Total;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){
 UINTN A=(UINTN)S;if(A>=(UINTN)Source&&A<(UINTN)Source+sizeof(Source)){assert(N<=sizeof(Source)-(A-(UINTN)Source));++SourceReads;}
 return memmove(D,S,N);
}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI AllocatePool(UINTN N){if(FailAllocation)return NULL;void *P=calloc(1,N);assert(P);++Allocations;return P;}
VOID EFIAPI FreePool(VOID *P){assert(Allocations);--Allocations;free(P);}
VOID *EFIAPI GetHobList(VOID){return Hobs;}
static VOID EndHobs(VOID){EFI_HOB_GENERIC_HEADER *H=(VOID *)(Hobs+HobBytes);*H=(EFI_HOB_GENERIC_HEADER){EFI_HOB_TYPE_END_OF_HOB_LIST,sizeof(*H),0};Phit->EfiEndOfHobList=(UINTN)H;Phit->EfiFreeMemoryBottom=(UINTN)(H+1);}
static VOID *Add(UINT16 Type,UINTN Size){assert((Size&7)==0&&HobBytes+Size+8<sizeof(Hobs));EFI_HOB_GENERIC_HEADER *H=(VOID *)(Hobs+HobBytes);memset(H,0,Size);H->HobType=Type;H->HobLength=(UINT16)Size;HobBytes+=Size;EndHobs();return H;}
static VOID SetBlob(CONST VOID *Data,UINTN Size){assert(Size<=CR_BOOT_DATA_FDT_MAX);memcpy(Source,Data,Size);Record->FdtSize=Size;}
static VOID Reset(BOOLEAN Tail){
 CrBootFdtDiscard();assert(!Allocations);memset(Source,0,sizeof(Source));memset(Hobs,0,sizeof(Hobs));FailAllocation=FALSE;
 Phit=(VOID *)Hobs;Phit->Header=(EFI_HOB_GENERIC_HEADER){EFI_HOB_TYPE_HANDOFF,sizeof(*Phit),0};Phit->Version=EFI_HOB_HANDOFF_TABLE_VERSION;
 Phit->EfiMemoryBottom=(UINTN)Hobs;Phit->EfiMemoryTop=(UINTN)Hobs+sizeof(Hobs);Phit->EfiFreeMemoryTop=Phit->EfiMemoryTop;
 HobBytes=sizeof(*Phit);
 Resource=Add(EFI_HOB_TYPE_RESOURCE_DESCRIPTOR,sizeof(*Resource));Resource->ResourceType=EFI_RESOURCE_SYSTEM_MEMORY;
 Resource->ResourceAttribute=EFI_RESOURCE_ATTRIBUTE_PRESENT|EFI_RESOURCE_ATTRIBUTE_INITIALIZED|EFI_RESOURCE_ATTRIBUTE_TESTED|
 EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE|EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE|EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE|
 EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE|EFI_RESOURCE_ATTRIBUTE_EXECUTION_PROTECTABLE|EFI_RESOURCE_ATTRIBUTE_READ_PROTECTABLE|EFI_RESOURCE_ATTRIBUTE_WRITE_PROTECTABLE;
 Resource->PhysicalStart=(UINTN)Source;Resource->ResourceLength=sizeof(Source);
 Allocation=Add(EFI_HOB_TYPE_MEMORY_ALLOCATION,sizeof(*Allocation));Allocation->AllocDescriptor.Name=gCrBootDataGuid;
 Allocation->AllocDescriptor.MemoryBaseAddress=(UINTN)Source;Allocation->AllocDescriptor.MemoryLength=sizeof(Source);Allocation->AllocDescriptor.MemoryType=EfiBootServicesData;
 EFI_HOB_GUID_TYPE *G=Add(EFI_HOB_TYPE_GUID_EXTENSION,sizeof(*G)+sizeof(*Record));G->Name=gCrBootDataGuid;Record=(VOID *)(G+1);
 *Record=(CR_BOOT_DATA_HOB){.Revision=1,.Size=sizeof(*Record),.AllocationBase=(UINTN)Source,.AllocationSize=sizeof(Source),.FdtOriginal=0x89000000,.FdtCopy=(UINTN)Source};
 assert(!fdt_create_empty_tree(Source,4096));int Chosen=fdt_add_subnode(Source,0,"chosen");assert(Chosen>=0);
 assert(!fdt_setprop_string(Source,Chosen,"bootargs",Tail?"androidboot.mode=normal":"androidboot.mode=charger"));
 int Dev=fdt_add_subnode(Source,0,"test@0");assert(Dev>=0);assert(!fdt_setprop_string(Source,Dev,"compatible","crane,test"));assert(!fdt_setprop(Source,Dev,"boolean",NULL,0));
 if(Tail){
  static const char Data[]="androidboot.mode = \"charger\"\n";UINT32 N=sizeof(Data)-1,Sum=0;
  Record->TailCopy=(UINTN)Source+CR_BOOT_DATA_FDT_MAX;Record->TailOriginal=0x90000000;Record->TailSize=N+20;
  UINT8 *T=(VOID *)(UINTN)Record->TailCopy;memcpy(T,Data,N);for(UINT32 I=0;I<N;++I)Sum+=T[I];
  for(UINT32 I=0;I<4;++I){T[N+I]=(UINT8)(N>>(I*8));T[N+4+I]=(UINT8)(Sum>>(I*8));}memcpy(T+N+8,"#BOOTCONFIG\n",12);
  Chosen=fdt_path_offset(Source,"/chosen");assert(Chosen>=0);
  assert(!fdt_setprop_u64(Source,Chosen,"linux,initrd-start",0x8ff00000));assert(!fdt_setprop_u64(Source,Chosen,"linux,initrd-end",Record->TailOriginal+Record->TailSize));
 }
 assert(!fdt_pack(Source));Total=fdt_totalsize(Source);Record->FdtSize=Total;SourceReads=0;
}
static VOID Failed(EFI_STATUS Expected,BOOLEAN BeforeCopy){
 UINT32 Before=SourceReads;EFI_STATUS S=CrBootFdtImport();if(S!=Expected)fprintf(stderr,"case%u got%zx wanted%zx\n",Checks,(size_t)S,(size_t)Expected);assert(S==Expected);
 assert(CrBootFdtHobPresent());if(BeforeCopy)assert(SourceReads==Before);
 EFI_CR_FDT_PROTOCOL *P=CrBootFdtProtocol();const void *Blob=(void *)1;UINTN Size=99;CR_ANDROID_BOOT_INFO Info;
 memset(&Info,0xa5,sizeof(Info));assert(P->GetBlob(P,&Blob,&Size)==S&&!Blob&&!Size);
 assert(P->GetBootInfo(P,&Info)==S);for(UINTN I=0;I<sizeof(Info);++I)assert(!((UINT8 *)&Info)[I]);assert(!Allocations);++Checks;
}
static VOID Nominal(VOID){
 for(UINT32 T=0;T<2;++T){Reset(T);assert(CrBootFdtImport()==EFI_SUCCESS&&SourceReads==(T?2:1)&&Allocations==1);
  EFI_CR_FDT_PROTOCOL *P=CrBootFdtProtocol();const void *Blob;UINTN Size;assert(P->GetBlob(P,&Blob,&Size)==EFI_SUCCESS&&Size==Total&&Blob!=Source);
  memset(Source,0xcc,sizeof(Source));memset(Record,0xdd,sizeof(*Record)); /* all source data reclaimed */
  CR_ANDROID_BOOT_INFO I;assert(P->GetBootInfo(P,&I)==EFI_SUCCESS&&I.Mode==CR_ANDROID_BOOT_CHARGER&&I.FdtAddress==0x89000000);
  assert(I.Source==(T?CR_ANDROID_BOOT_SOURCE_BOOTCONFIG:CR_ANDROID_BOOT_SOURCE_CMDLINE));
  INT32 N;const void *Data;UINTN Length;
  assert(P->FindNode(P,"/chosen",&N)==EFI_SUCCESS&&N>=0);
  assert(P->GetProperty(P,N,"bootargs",&Data,&Length)==EFI_SUCCESS&&Length>0);
  assert(P->FindCompatible(P,-1,"crane,test",&N)==EFI_SUCCESS);
  assert(P->GetProperty(P,N,"boolean",&Data,&Length)==EFI_SUCCESS&&Length==0);
  assert(P->FindCompatible(P,N,"crane,test",&N)==EFI_NOT_FOUND&&N==-1);
  assert(P->FindNode(P,"/missing",&N)==EFI_NOT_FOUND&&N==-1);
  assert(P->GetProperty(P,1,"invalid",&Data,&Length)==EFI_INVALID_PARAMETER&&!Data&&!Length);
  assert(P->GetBlob(NULL,&Blob,&Size)==EFI_INVALID_PARAMETER&&!Blob&&!Size);
  Size=99;assert(P->GetBlob(P,NULL,&Size)==EFI_INVALID_PARAMETER&&!Size);
  Blob=(VOID *)1;assert(P->GetBlob(P,&Blob,NULL)==EFI_INVALID_PARAMETER&&!Blob);
  Data=(VOID *)1;assert(P->GetProperty(P,0,"bad",&Data,NULL)==EFI_INVALID_PARAMETER&&!Data);
  assert(P->GetBootInfo(NULL,&I)==EFI_INVALID_PARAMETER&&I.Mode==0);
  assert(P->FindNode(P,"relative",&N)==EFI_INVALID_PARAMETER&&N==-1);
  assert(P->FindCompatible(P,-2,"crane,test",&N)==EFI_INVALID_PARAMETER&&N==-1);
  assert(P->GetBlob(P,&Blob,&Size)==EFI_SUCCESS);UINT8 Saved[8];memcpy(Saved,Blob,8);
  assert(P->GetBlob(P,(VOID *)Blob,&Size)==EFI_INVALID_PARAMETER&&!memcmp(Saved,Blob,8));
  assert(P->GetBootInfo(P,(VOID *)Blob)==EFI_INVALID_PARAMETER&&!memcmp(Saved,Blob,8));
  assert(CrBootFdtImport()==EFI_SUCCESS);++Checks;
 }
}
static VOID Hostile(VOID){for(UINT32 Mode=0;Mode<33;++Mode){Reset(TRUE);
 if(Mode==0)Record->Revision++;
 else if(Mode==1)Record->Size--;
 else if(Mode==2){EFI_HOB_GUID_TYPE *G=Add(EFI_HOB_TYPE_GUID_EXTENSION,sizeof(*G)+sizeof(*Record));G->Name=gCrBootDataGuid;memcpy(G+1,Record,sizeof(*Record));}
 else if(Mode==3)Allocation->AllocDescriptor.MemoryType=EfiConventionalMemory;
 else if(Mode==4)Allocation->AllocDescriptor.MemoryLength-=4096;
 else if(Mode==5)Record->FdtCopy=1;
 else if(Mode==6)Record->FdtCopy++;
 else if(Mode==7)Record->FdtSize=39;
 else if(Mode==8)Record->FdtSize=CR_BOOT_DATA_FDT_MAX+1;
 else if(Mode==9)Record->FdtCopy=Record->AllocationBase+Record->AllocationSize-8;
 else if(Mode==10)Record->AllocationBase=MAX_UINT64-4095;
 else if(Mode==11)Record->TailCopy=Record->FdtCopy;
 else if(Mode==12)Record->TailOriginal=Record->FdtOriginal;
 else if(Mode==13)Record->TailSize=CR_BOOT_DATA_TAIL_MAX+1;
 else if(Mode==14){EFI_HOB_MEMORY_ALLOCATION *M=Add(EFI_HOB_TYPE_MEMORY_ALLOCATION,sizeof(*M));M->AllocDescriptor=Allocation->AllocDescriptor;}
 else if(Mode==15)Phit->EfiFreeMemoryBottom=(UINTN)Source,Phit->EfiFreeMemoryTop=(UINTN)Source+sizeof(Source);
 else if(Mode==16)Allocation->AllocDescriptor.Reserved[0]=1;
 else if(Mode==17)Record->FdtOriginal=0;
 else if(Mode==18)Record->TailCopy=MAX_UINT64-10;
 else if(Mode==19){EFI_HOB_MEMORY_ALLOCATION *M=Add(EFI_HOB_TYPE_MEMORY_ALLOCATION,sizeof(*M));M->AllocDescriptor=Allocation->AllocDescriptor;M->AllocDescriptor.Name.Data1++;}
 else if(Mode==20)Allocation->AllocDescriptor.Name.Data1++;
 else if(Mode==21)((EFI_HOB_GUID_TYPE *)Record-1)->Header.HobLength-=8;
 else if(Mode==22)((EFI_HOB_GENERIC_HEADER *)(UINTN)Phit->EfiEndOfHobList)->HobType=EFI_HOB_TYPE_UNUSED;
 else if(Mode==23)Record->TailStatus=EFI_NOT_READY;
 else if(Mode==24)Record->FdtStatus=EFI_WARN_UNKNOWN_GLYPH;
 else if(Mode==25)Resource->ResourceType=EFI_RESOURCE_MEMORY_MAPPED_IO;
 else if(Mode==26)Resource->ResourceLength--;
 else if(Mode==27)Resource->PhysicalStart++,Resource->ResourceLength--;
 else if(Mode==28)Resource->PhysicalStart=MAX_UINT64-4095;
 else if(Mode==29)Resource->ResourceAttribute&=~EFI_RESOURCE_ATTRIBUTE_PRESENT;
 else if(Mode==30)Resource->ResourceAttribute&=~EFI_RESOURCE_ATTRIBUTE_INITIALIZED;
 else if(Mode==31)Resource->ResourceAttribute&=~EFI_RESOURCE_ATTRIBUTE_TESTED;
 else Resource->ResourceAttribute|=EFI_RESOURCE_ATTRIBUTE_READ_PROTECTED;
 Failed(EFI_COMPROMISED_DATA,TRUE);
 }}
static VOID EnclosingConventional(VOID){Reset(FALSE);
 EFI_HOB_MEMORY_ALLOCATION *M=Add(EFI_HOB_TYPE_MEMORY_ALLOCATION,sizeof(*M));
 M->AllocDescriptor.MemoryBaseAddress=(UINTN)Source-4096;M->AllocDescriptor.MemoryLength=sizeof(Source)+8192;M->AllocDescriptor.MemoryType=EfiConventionalMemory;
 assert(CrBootFdtImport()==EFI_SUCCESS);++Checks;
 Reset(FALSE);M=Add(EFI_HOB_TYPE_MEMORY_ALLOCATION,sizeof(*M));
 M->AllocDescriptor.MemoryBaseAddress=(UINTN)Source-4096;M->AllocDescriptor.MemoryLength=sizeof(Source)+8192;M->AllocDescriptor.MemoryType=EfiConventionalMemory;
 EFI_HOB_MEMORY_ALLOCATION Swap=*M;*M=*Allocation;*Allocation=Swap;Failed(EFI_COMPROMISED_DATA,TRUE);
 Reset(FALSE);M=Add(EFI_HOB_TYPE_MEMORY_ALLOCATION,sizeof(*M));
 M->AllocDescriptor.MemoryBaseAddress=(UINTN)Source+4096;M->AllocDescriptor.MemoryLength=4096;M->AllocDescriptor.MemoryType=EfiConventionalMemory;Failed(EFI_COMPROMISED_DATA,TRUE);
}
static VOID InvalidFdt(VOID){for(UINT32 Mode=0;Mode<8;++Mode){Reset(FALSE);UINT32 *Words=(VOID *)Source;
 if(Mode==0)Words[0]=0;else if(Mode==1)Words[1]=cpu_to_fdt32((UINT32)Total-1);
 else if(Mode==2)Words[2]=cpu_to_fdt32(fdt_off_dt_struct(Source)+1);
 else if(Mode==3)Words[4]=cpu_to_fdt32(41);else if(Mode==4)Words[4]=cpu_to_fdt32((UINT32)Total-8);
 else if(Mode==5)Words[9]=cpu_to_fdt32(0x7fffffff);else if(Mode==6)Words[5]=cpu_to_fdt32(16);
 else Source[fdt_off_dt_struct(Source)]=0xff;
 Failed(EFI_COMPROMISED_DATA,FALSE);
 }}
static VOID Statuses(VOID){
 Reset(FALSE);FailAllocation=TRUE;Failed(EFI_OUT_OF_RESOURCES,TRUE);
 Reset(FALSE);Record->FdtStatus=EFI_NOT_READY;Record->FdtCopy=Record->FdtSize=Record->AllocationBase=Record->AllocationSize=0;Failed(EFI_NOT_READY,TRUE);
 Reset(FALSE);Record->TailStatus=EFI_NOT_READY;assert(CrBootFdtImport()==EFI_SUCCESS);EFI_CR_FDT_PROTOCOL *P=CrBootFdtProtocol();CR_ANDROID_BOOT_INFO I;const void *B;UINTN S;
 assert(P->GetBlob(P,&B,&S)==EFI_SUCCESS);assert(P->GetBootInfo(P,&I)==EFI_NOT_READY&&I.Mode==0);++Checks;
 Reset(FALSE);HobBytes=sizeof(*Phit);EndHobs();assert(CrBootFdtImport()==EFI_NOT_FOUND&&!CrBootFdtHobPresent()&&SourceReads==0);++Checks;
 Reset(FALSE);HobBytes=sizeof(*Phit)+sizeof(*Resource)+sizeof(*Allocation);EndHobs();Failed(EFI_COMPROMISED_DATA,TRUE);
 Reset(TRUE);((UINT8 *)(UINTN)Record->TailCopy)[0]^=1;assert(CrBootFdtImport()==EFI_SUCCESS);assert(P->GetBootInfo(P,&I)==EFI_INVALID_PARAMETER&&I.Mode==0);++Checks;
 Reset(TRUE);Record->TailCopy=Record->TailOriginal=Record->TailSize=0;assert(CrBootFdtImport()==EFI_SUCCESS);assert(P->GetBootInfo(P,&I)==EFI_NOT_READY&&I.Mode==0);++Checks;
}
static VOID Real(const char *Path){Reset(FALSE);FILE *F=fopen(Path,"rb");assert(F);UINTN N=fread(Source,1,CR_BOOT_DATA_FDT_MAX,F);fclose(F);assert(N>=40&&N==(UINTN)fdt_totalsize(Source));Record->FdtSize=N;
 assert(CrBootFdtImport()==EFI_SUCCESS);EFI_CR_FDT_PROTOCOL *P=CrBootFdtProtocol();const void *B;UINTN S;assert(P->GetBlob(P,&B,&S)==EFI_SUCCESS);CR_UFS_POWER_FDT U;
 assert(CrUfsPowerFdtParse(B,S,&U)==EFI_SUCCESS);memset(Source,0,sizeof(Source));assert(CrUfsPowerFdtParse(B,S,&U)==EFI_SUCCESS);++Checks;
}
int main(int argc,char **argv){Nominal();Hostile();EnclosingConventional();InvalidFdt();Statuses();for(int I=1;I<argc;++I)Real(argv[I]);CrBootFdtDiscard();assert(!Allocations);printf("DXE raw FDT importer: %u scenario groups PASS\n",Checks);return 0;}
