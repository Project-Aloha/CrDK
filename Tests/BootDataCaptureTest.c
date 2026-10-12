/* Actual raw SEC producer; only memory-map/HOB firmware boundaries are controlled.
 * SPDX-License-Identifier: MIT */
#include <PiPei.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Library/BootModeCaptureCrLib/BootModeCaptureCrLib.c"

EFI_GUID gCrBootDataGuid = CR_BOOT_DATA_HOB_GUID;
STATIC ARM_MEMORY_REGION_DESCRIPTOR_EX Regions[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
STATIC _Alignas(4096) UINT8 HeapBytes[4 * 1024 * 1024];
STATIC _Alignas(8) UINT8 Source[4096], TailSource[CR_BOOT_DATA_TAIL_MAX + 4096];
STATIC EFI_HOB_MEMORY_ALLOCATION AllocationHob;
STATIC CR_BOOT_DATA_HOB Published;
STATIC UINTN Sequence, AllocSequence, GuidSequence, Copies, Checks;
STATIC BOOLEAN FailAllocation, FailDescriptor;
#define CHECK(X) do { ++Checks; assert(X); } while (0)
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { ++Copies; return memmove(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D,0,N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A,B,N); }
EFI_GUID *EFIAPI CopyGuid(EFI_GUID *D, CONST EFI_GUID *S) { memcpy(D,S,sizeof(*D)); return D; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap(VOID) { return Regions; }
VOID *CreateHob(UINT16 Type, UINT16 Size) {
  CHECK(Type == EFI_HOB_TYPE_MEMORY_ALLOCATION && Size == sizeof(AllocationHob));
  if (FailAllocation) return NULL;
  memset(&AllocationHob,0,sizeof(AllocationHob));AllocSequence=++Sequence;
  return &AllocationHob;
}
VOID *EFIAPI BuildGuidDataHob(CONST EFI_GUID *Guid, VOID *Data, UINTN Size) {
  CHECK(!memcmp(Guid,&gCrBootDataGuid,sizeof(*Guid)) && Size==sizeof(Published));
  if (FailDescriptor) return NULL;
  memcpy(&Published,Data,Size);GuidSequence=++Sequence;return &Published;
}
STATIC VOID W(UINT8 *P, UINT32 V) { P[0]=V>>24;P[1]=V>>16;P[2]=V>>8;P[3]=V; }
STATIC VOID Word(UINT8 *P, UINTN *At, UINT32 V) { W(P+*At,V);*At+=4; }
STATIC VOID Node(UINT8 *P, UINTN *At, CONST char *Name) {
  Word(P,At,1);size_t N=strlen(Name)+1;memcpy(P+*At,Name,N);*At=(*At+N+3)&~(UINTN)3;
}
STATIC VOID Property(UINT8 *P, UINTN *At, UINT32 Name, UINT64 Value) {
  Word(P,At,3);Word(P,At,8);Word(P,At,Name);
  Word(P,At,(UINT32)(Value>>32));Word(P,At,(UINT32)Value);
}
STATIC UINTN Make(UINT8 *P, UINT64 Begin, UINT64 End, UINT32 Flags) {
  STATIC CONST char Names[]="linux,initrd-start\0linux,initrd-end\0";
  UINTN At=56;memset(P,0,512);Node(P,&At,"");
  if (Flags) {
    Node(P,&At,"chosen");
    if (Flags&1) Property(P,&At,0,Begin);
    if (Flags&2) Property(P,&At,sizeof("linux,initrd-start"),End);
    if (Flags&4) Property(P,&At,0,Begin);
    Word(P,&At,2);
    if (Flags&8) { Node(P,&At,"chosen");Word(P,&At,2); }
  }
  Word(P,&At,2);Word(P,&At,9);
  W(P,0xd00dfeed);W(P+8,56);W(P+12,(UINT32)At);W(P+16,40);
  W(P+20,17);W(P+24,16);W(P+32,sizeof(Names));W(P+36,(UINT32)At-56);
  memcpy(P+At,Names,sizeof(Names));At+=sizeof(Names);W(P+4,(UINT32)At);return At;
}
STATIC VOID Region(UINTN I, CONST char *Name, UINT64 Base, UINT64 Size) {
  memset(&Regions[I],0,sizeof(Regions[I]));strcpy(Regions[I].Name,Name);
  Regions[I].Address=Base;Regions[I].Length=Size;Regions[I].HobOption=AddMem;
  Regions[I].ResourceType=SYS_MEM;Regions[I].ResourceAttribute=SYS_MEM_CAP;
  Regions[I].MemoryType=Conv;Regions[I].ArmAttributes=WRITE_BACK;
}
STATIC VOID Reset(VOID) {
  /* A fresh simulated boot; no production reset API is introduced. */
  mCaptured=mPublished=FALSE;memset(&mBootData,0,sizeof(mBootData));
  memset(Regions,0,sizeof(Regions));memset(&Published,0,sizeof(Published));
  memset(HeapBytes,0xa5,sizeof(HeapBytes));memset(TailSource,0x6b,sizeof(TailSource));
  Sequence=AllocSequence=GuidSequence=Copies=0;FailAllocation=FailDescriptor=FALSE;
  Region(0,"DXE Heap",(UINTN)HeapBytes,sizeof(HeapBytes));
  Region(1,"DT BLOB",(UINTN)Source,sizeof(Source));
  Region(2,"RAM Partition",(UINTN)TailSource,sizeof(TailSource));
}
STATIC UINTN Capture(UINT8 *Fdt) {
  UINTN Top=(UINTN)HeapBytes+sizeof(HeapBytes);
  BootModeCapture((UINTN)Fdt,(UINTN)HeapBytes,&Top);return Top;
}
STATIC VOID Good(UINT8 *Fdt, UINTN Size, UINTN Top) {
  CHECK(mBootData.FdtStatus==EFI_SUCCESS);
  CHECK(mBootData.FdtCopy==Top && Top>=(UINTN)HeapBytes+BOOT_HEAP_SLACK);
  CHECK(mBootData.AllocationBase==Top && !(Top&EFI_PAGE_MASK));
  CHECK(!(mBootData.AllocationSize&EFI_PAGE_MASK));
  CHECK(mBootData.FdtSize==Size && !memcmp((VOID *)Top,Fdt,Size));
}
int main(void) {
  UINTN Size, Top, Before;UINT64 Base;
  Reset();Size=Make(Source,0,0,0);Top=Capture(Source);Good(Source,Size,Top);
  CHECK(mBootData.TailStatus==EFI_SUCCESS && !mBootData.TailSize);
  BootModePublish();CHECK(AllocSequence==1 && GuidSequence==2);
  CHECK(!memcmp(&AllocationHob.AllocDescriptor.Name,&gCrBootDataGuid,sizeof(EFI_GUID)));
  CHECK(AllocationHob.AllocDescriptor.MemoryType==EfiBootServicesData);
  CHECK(AllocationHob.AllocDescriptor.MemoryBaseAddress==Top && AllocationHob.AllocDescriptor.MemoryLength==mBootData.AllocationSize);
  Before=Copies;BootModeCapture(0,0,&Top);BootModePublish();CHECK(Sequence==2 && Copies==Before);
  for (UINTN Length=20;Length<=23;++Length) {
    Reset();Size=Make(Source,(UINTN)TailSource,(UINTN)TailSource+Length,3);
    Top=Capture(Source);Good(Source,Size,Top);
    CHECK(mBootData.TailStatus==EFI_SUCCESS && mBootData.TailSize==Length);
    CHECK(!memcmp((VOID *)(UINTN)mBootData.TailCopy,TailSource,Length));
  }
  Reset();Size=Make(Source,(UINTN)TailSource,(UINTN)TailSource+sizeof(TailSource),3);
  Top=Capture(Source);Good(Source,Size,Top);CHECK(mBootData.TailSize==CR_BOOT_DATA_TAIL_MAX);
  CHECK(mBootData.TailOriginal==(UINTN)TailSource+sizeof(TailSource)-CR_BOOT_DATA_TAIL_MAX);
  CHECK(!memcmp((VOID *)(UINTN)mBootData.TailCopy,(VOID *)(UINTN)mBootData.TailOriginal,(size_t)mBootData.TailSize));
  for (UINTN Length=0;Length<20;++Length) {
    Reset();Make(Source,(UINTN)TailSource,(UINTN)TailSource+Length,3);Capture(Source);
    CHECK(mBootData.FdtStatus==EFI_SUCCESS && mBootData.TailStatus==EFI_SUCCESS && !mBootData.TailSize);
  }
  for (UINT32 Flags=1;Flags<=15;++Flags) {
    if (Flags==3 || Flags==6) continue;
    Reset();Make(Source,(UINTN)TailSource,(UINTN)TailSource+64,Flags);Capture(Source);
    CHECK(mBootData.FdtStatus==EFI_SUCCESS && EFI_ERROR(mBootData.TailStatus) && !mBootData.TailSize);
  }
  Reset();Make(Source,0,0,3);Capture(Source);CHECK(mBootData.TailStatus==EFI_SUCCESS);
  Reset();Make(Source,0,32,3);Capture(Source);CHECK(EFI_ERROR(mBootData.TailStatus));
  Reset();Make(Source,100,99,3);Capture(Source);CHECK(EFI_ERROR(mBootData.TailStatus));
  Reset();Make(Source,(UINTN)Source,(UINTN)Source+256,3);Capture(Source);CHECK(EFI_ERROR(mBootData.TailStatus));
  Reset();Make(Source,MAX_UINT64-23,MAX_UINT64,3);Capture(Source);CHECK(EFI_ERROR(mBootData.TailStatus));
  /* Source DTB itself occupies the first candidate; the tail occupies the
   * second. Both must survive before either snapshot destination is written. */
  Reset();UINT8 *Fdt=HeapBytes+sizeof(HeapBytes)-8192;
  UINT8 *Tail=HeapBytes+sizeof(HeapBytes)-4096;memset(Tail,0x73,1024);
  Size=Make(Fdt,(UINTN)Tail,(UINTN)Tail+1024,3);Top=Capture(Fdt);Good(Fdt,Size,Top);
  CHECK(Top+mBootData.AllocationSize<=(UINTN)Fdt);
  CHECK(!memcmp((VOID *)(UINTN)mBootData.TailCopy,Tail,1024));
  /* Physical arithmetic above4GiB, source-collision retries and no-room path
   * exercise the actual allocator helper without dereferencing invented RAM. */
  Reset();Region(0,"DXE Heap",0x100000000ULL,0x400000);
  CHECK(Arena(Regions,1,0,0x100000000ULL,0x100400000ULL,0x1003ff000ULL,4096,
        0x1003fe000ULL,4096,8192,&Base)==EFI_SUCCESS && Base==0x1003fc000ULL);
  CHECK(Arena(Regions,1,0,0x100000000ULL,0x100010000ULL,0,0,0,0,4096,&Base)==EFI_OUT_OF_RESOURCES);
  CHECK(Arena(Regions,1,0,0x100000001ULL,0x100400000ULL,0,0,0,0,4096,&Base)==EFI_INVALID_PARAMETER);
  /* Fail closed on untrusted map names, authority, gaps, overlaps and bounds. */
  for (UINTN Case=0;Case<13;++Case) {
    Reset();Make(Source,0,0,0);
    if (Case==0) Regions[0].HobOption=NoHob;
    if (Case==1) Regions[0].MemoryType=BsData;
    if (Case==2) Regions[0].ResourceAttribute=0;
    if (Case==3) Regions[0].ResourceAttribute|=EFI_RESOURCE_ATTRIBUTE_WRITE_PROTECTED;
    if (Case==4) Regions[0].ArmAttributes=NS_DEVICE;
    if (Case==5) Regions[3]=Regions[0];
    if (Case==6) memset(Regions[1].Name,'x',sizeof(Regions[1].Name));
    if (Case==7) Regions[1].Length=40;
    if (Case==8) Region(3,"PIL Reserved",(UINTN)Source+40,8);
    if (Case==9) { Regions[1].Address=MAX_UINT64-1;Regions[1].Length=16; }
    if (Case==10) for (UINTN I=3;I<MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT;++I) Regions[I]=Regions[1];
    if (Case==11) Regions[1].ResourceAttribute|=EFI_RESOURCE_ATTRIBUTE_READ_PROTECTED;
    if (Case==12) Regions[1].ResourceAttribute=0;
    Top=Capture(Source);CHECK(EFI_ERROR(mBootData.FdtStatus));
    CHECK(Top==(UINTN)HeapBytes+sizeof(HeapBytes) && !mBootData.AllocationSize);
  }
  for (UINTN Case=0;Case<4;++Case) {
    Reset();Make(Source,0,0,0);
    if (Case==0) W(Source,0);
    if (Case==1) W(Source+4,39);
    if (Case==2) W(Source+4,CR_BOOT_DATA_FDT_MAX+1);
    if (Case==3) W(Source+20,16);
    Capture(Source);CHECK(mBootData.FdtStatus==EFI_COMPROMISED_DATA);
  }
  Reset();Make(Source,0,0,0);W(Source+8,0xfffffffc);Capture(Source);
  CHECK(mBootData.FdtStatus==EFI_SUCCESS && EFI_ERROR(mBootData.TailStatus));
  Reset();Make(Source,0,0,0);Capture(Source);FailAllocation=TRUE;BootModePublish();CHECK(Sequence==0);
  Reset();Make(Source,0,0,0);Top=Capture(Source);FailDescriptor=TRUE;BootModePublish();
  CHECK(AllocSequence==1 && !GuidSequence && Top==mBootData.AllocationBase);
  printf("PASS raw SEC capture %zu checks\n",(size_t)Checks);return 0;
}
