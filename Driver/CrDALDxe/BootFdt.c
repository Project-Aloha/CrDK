/* Bounded raw SEC copy -> private immutable DXE FDT.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/CrBootData.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include "BootFdt.h"

/* MU BaseFdtLib builds this libfdt validator; the public wrapper exposes only
 * header checking. Never call its typed reserve-map walker before alignment
 * and bounds admission. */
extern int fdt_check_full(CONST VOID *, UINTN);
#define HOB_BYTES_MAX (1024U * 1024U)
#define TEXT_MAX 4096U
STATIC struct {
  BOOLEAN Imported, Present;
  EFI_STATUS FdtStatus, BootStatus;
  UINT8 *Copy;
  UINTN CopySize, FdtSize, TailOffset, TailSize;
  UINT64 FdtOriginal, TailOriginal;
  CR_ANDROID_BOOT_INFO BootInfo;
} mBootFdt;
STATIC EFI_CR_FDT_PROTOCOL mFdtProtocol;

STATIC BOOLEAN Range(UINT64 Base, UINT64 Size)
{ return Base!=0 && Size!=0 && Base<=MAX_UINT64-Size; }
STATIC BOOLEAN Overlap(UINT64 A, UINT64 N, UINT64 B, UINT64 M)
{ return N!=0 && M!=0 && A<B+M && B<A+N; }
STATIC BOOLEAN Contains(UINT64 A, UINT64 N, UINT64 B, UINT64 M)
{ return Range(A,N) && Range(B,M) && B>=A && M<=N && B-A<=N-M; }
STATIC UINT32 Be32(CONST UINT8 *P)
{ return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3]; }
STATIC EFI_STATUS Failure(EFI_STATUS Status)
{ return EFI_ERROR(Status)?Status:EFI_COMPROMISED_DATA; }
STATIC BOOLEAN EqualGuid(CONST EFI_GUID *A, CONST EFI_GUID *B)
{ return CompareMem(A,B,sizeof(*A))==0; }

/* A named allocation HOB is the source-read authority. Neither an arbitrary
 * original DT address nor a generic surrounding RAM descriptor suffices. */
STATIC EFI_STATUS CaptureRecord(CR_BOOT_DATA_HOB *Out)
{
  CONST EFI_HOB_HANDOFF_INFO_TABLE *Phit=GetHobList();
  CR_BOOT_DATA_HOB Record={0};
  EFI_HOB_MEMORY_ALLOCATION_HEADER Allocation={0};
  UINTN Start,End,Bytes,At,AllocationAt=0;
  UINT32 Records=0,Allocations=0;
  if(!Phit) return EFI_NOT_FOUND;
  Start=(UINTN)Phit;
  if((Start&7) || Phit->Header.HobType!=EFI_HOB_TYPE_HANDOFF ||
     Phit->Header.HobLength!=sizeof(*Phit) || Phit->Header.Reserved ||
     Phit->Version!=EFI_HOB_HANDOFF_TABLE_VERSION)
    return EFI_COMPROMISED_DATA;
  /* Crane PrePi constructs the PHIT at EfiMemoryBottom. MU DXE GCD copies
   * [that base, EfiFreeMemoryBottom) into pool without rebasing any PHIT
   * fields. They describe historical PEI memory, not pointers into this
   * current list. Derive its exact copied length, then walk only the copy.
   * Never dereference the old list: GCD has already made it reusable. */
  if(!Phit->EfiMemoryBottom || (Phit->EfiMemoryBottom&7) ||
     (Phit->EfiEndOfHobList&7) ||
     Phit->EfiEndOfHobList<Phit->EfiMemoryBottom ||
     Phit->EfiEndOfHobList-Phit->EfiMemoryBottom<sizeof(*Phit) ||
     Phit->EfiEndOfHobList-Phit->EfiMemoryBottom>HOB_BYTES_MAX-sizeof(EFI_HOB_GENERIC_HEADER) ||
     Phit->EfiEndOfHobList>MAX_UINT64-sizeof(EFI_HOB_GENERIC_HEADER) ||
     Phit->EfiFreeMemoryBottom!=Phit->EfiEndOfHobList+sizeof(EFI_HOB_GENERIC_HEADER) ||
     Phit->EfiFreeMemoryBottom>Phit->EfiFreeMemoryTop ||
     Phit->EfiFreeMemoryTop>Phit->EfiMemoryTop)
    return EFI_COMPROMISED_DATA;
  Bytes=(UINTN)(Phit->EfiFreeMemoryBottom-Phit->EfiMemoryBottom);
  if(Start>MAX_UINTN-Bytes) return EFI_COMPROMISED_DATA;
  End=Start+Bytes-sizeof(EFI_HOB_GENERIC_HEADER);
  for(At=Start;At<=End;) {
    CONST EFI_HOB_GENERIC_HEADER *H=(CONST VOID *)At;
    if(H->Reserved || H->HobLength<sizeof(*H) || (H->HobLength&7) ||
       H->HobLength>End+sizeof(*H)-At) return EFI_COMPROMISED_DATA;
    if(H->HobType==EFI_HOB_TYPE_END_OF_HOB_LIST) {
      if(At!=End || H->HobLength!=sizeof(*H)) return EFI_COMPROMISED_DATA;
      break;
    }
    if(At==End || H->HobLength>End-At) return EFI_COMPROMISED_DATA;
    if(H->HobType==EFI_HOB_TYPE_GUID_EXTENSION) {
      CONST EFI_HOB_GUID_TYPE *G=(CONST VOID *)H;
      if(H->HobLength<sizeof(*G)) return EFI_COMPROMISED_DATA;
      if(EqualGuid(&G->Name,&gCrBootDataGuid)) {
        mBootFdt.Present=TRUE;
        if(++Records!=1 || H->HobLength!=sizeof(*G)+sizeof(Record))
          return EFI_COMPROMISED_DATA;
        CopyMem(&Record,G+1,sizeof(Record));
      }
    } else if(H->HobType==EFI_HOB_TYPE_MEMORY_ALLOCATION) {
      CONST EFI_HOB_MEMORY_ALLOCATION *M=(CONST VOID *)H;
      if(H->HobLength<sizeof(*M)) return EFI_COMPROMISED_DATA;
      if(EqualGuid(&M->AllocDescriptor.Name,&gCrBootDataGuid)) {
        mBootFdt.Present=TRUE;
        if(++Allocations!=1 || H->HobLength!=sizeof(*M)) return EFI_COMPROMISED_DATA;
        Allocation=M->AllocDescriptor;AllocationAt=At;
      }
    }
    if(H->HobType==EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && H->HobLength!=sizeof(EFI_HOB_RESOURCE_DESCRIPTOR))
      return EFI_COMPROMISED_DATA;
    At+=H->HobLength;
  }
  if(!Records) return Allocations?EFI_COMPROMISED_DATA:EFI_NOT_FOUND;
  if(Record.Revision!=CR_BOOT_DATA_HOB_REVISION || Record.Size!=sizeof(Record))
    return EFI_COMPROMISED_DATA;
  if(Record.FdtStatus!=EFI_SUCCESS) {
    if(Record.FdtCopy || Record.FdtSize || Record.TailCopy || Record.TailSize ||
       Record.AllocationBase || Record.AllocationSize) return EFI_COMPROMISED_DATA;
    return Failure(Record.FdtStatus);
  }
  if(Allocations!=1 || Allocation.MemoryType!=EfiBootServicesData ||
     Allocation.MemoryBaseAddress!=Record.AllocationBase ||
     Allocation.MemoryLength!=Record.AllocationSize ||
     CompareMem(Allocation.Reserved,(UINT8[4]){0},4)!=0 ||
     !Range(Record.AllocationBase,Record.AllocationSize) ||
     Record.AllocationBase>MAX_UINTN-Record.AllocationSize ||
     (Record.AllocationBase&EFI_PAGE_MASK) || (Record.AllocationSize&EFI_PAGE_MASK) ||
     Record.AllocationSize>EFI_SIZE_TO_PAGES(CR_BOOT_DATA_FDT_MAX+CR_BOOT_DATA_TAIL_MAX+7)*EFI_PAGE_SIZE ||
     !Contains(Record.AllocationBase,Record.AllocationSize,Record.FdtCopy,Record.FdtSize) ||
     (Record.FdtCopy&7) || (Record.FdtOriginal&7) ||
     !Range(Record.FdtOriginal,Record.FdtSize) || Record.FdtSize<40 ||
     Record.FdtSize>CR_BOOT_DATA_FDT_MAX || Record.TailSize>CR_BOOT_DATA_TAIL_MAX)
    return EFI_COMPROMISED_DATA;
  if(Record.TailStatus==EFI_SUCCESS && Record.TailSize) {
    if(!Contains(Record.AllocationBase,Record.AllocationSize,Record.TailCopy,Record.TailSize) ||
       !Range(Record.TailOriginal,Record.TailSize) ||
       Overlap(Record.FdtCopy,Record.FdtSize,Record.TailCopy,Record.TailSize) ||
       Overlap(Record.FdtOriginal,Record.FdtSize,Record.TailOriginal,Record.TailSize))
      return EFI_COMPROMISED_DATA;
  } else if(Record.TailCopy || Record.TailOriginal || Record.TailSize)
    return EFI_COMPROMISED_DATA;
  if(Overlap(Record.AllocationBase,Record.AllocationSize,Start,Bytes) ||
     Overlap(Record.AllocationBase,Record.AllocationSize,Phit->EfiMemoryBottom,Bytes) ||
     Overlap(Record.AllocationBase,Record.AllocationSize,Phit->EfiFreeMemoryBottom,
             Phit->EfiFreeMemoryTop-Phit->EfiFreeMemoryBottom)) return EFI_COMPROMISED_DATA;
  /* Reject a second owner for any byte of the supposedly dedicated copy. */
  for(At=Start;At<End;) {
    CONST EFI_HOB_GENERIC_HEADER *H=(CONST VOID *)At;
    if(H->HobType==EFI_HOB_TYPE_MEMORY_ALLOCATION) {
      CONST EFI_HOB_MEMORY_ALLOCATION *M=(CONST VOID *)H;
      if(!Range(M->AllocDescriptor.MemoryBaseAddress,M->AllocDescriptor.MemoryLength))
        return EFI_COMPROMISED_DATA;
      if(!EqualGuid(&M->AllocDescriptor.Name,&gCrBootDataGuid) &&
         Overlap(Record.AllocationBase,Record.AllocationSize,M->AllocDescriptor.MemoryBaseAddress,
                 M->AllocDescriptor.MemoryLength) &&
         (M->AllocDescriptor.MemoryType!=EfiConventionalMemory || At<AllocationAt ||
          !Contains(M->AllocDescriptor.MemoryBaseAddress,M->AllocDescriptor.MemoryLength,
                    Record.AllocationBase,Record.AllocationSize))) return EFI_COMPROMISED_DATA;
      /* The named arena is the first claim; later MemoryPeim conventional
       * descriptors cannot supersede it in DXE GCD initialization. The historical
       * PEI free interval was already checked disjoint; a partial overlap or any
       * competing allocated type is never an alternate read authority. */
    }
    At+=H->HobLength;
  }
  UINT64 Covered=Record.AllocationBase,Limit=Covered+Record.AllocationSize;
  while(Covered<Limit) {
    UINT64 Next=Covered;
    for(At=Start;At<End;) {
      CONST EFI_HOB_GENERIC_HEADER *H=(CONST VOID *)At;
      if(H->HobType==EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
        CONST EFI_HOB_RESOURCE_DESCRIPTOR *D=(CONST VOID *)H;
        if(!Range(D->PhysicalStart,D->ResourceLength))return EFI_COMPROMISED_DATA;
        if(Overlap(Record.AllocationBase,Record.AllocationSize,D->PhysicalStart,D->ResourceLength)) {
          UINT64 Need=EFI_RESOURCE_ATTRIBUTE_PRESENT|EFI_RESOURCE_ATTRIBUTE_INITIALIZED|EFI_RESOURCE_ATTRIBUTE_TESTED;
          if(D->ResourceType!=EFI_RESOURCE_SYSTEM_MEMORY || (D->ResourceAttribute&Need)!=Need ||
             (D->ResourceAttribute&EFI_RESOURCE_ATTRIBUTE_READ_PROTECTED))return EFI_COMPROMISED_DATA;
          if(D->PhysicalStart<=Covered && D->ResourceLength>Covered-D->PhysicalStart &&
             D->PhysicalStart+D->ResourceLength>Next)Next=D->PhysicalStart+D->ResourceLength;
        }
      }
      At+=H->HobLength;
    }
    if(Next==Covered)return EFI_COMPROMISED_DATA;
    Covered=Next>Limit?Limit:Next;
  }
  *Out=Record; return EFI_SUCCESS;
}

STATIC EFI_STATUS ValidateFdt(CONST UINT8 *Fdt,UINTN Size)
{
  UINT32 Struct,Strings,Rsv,Ss,Ts;INT32 Count;
  if(((UINTN)Fdt&7) || Size<40 || Size>CR_BOOT_DATA_FDT_MAX ||
     Be32(Fdt)!=0xd00dfeed || Be32(Fdt+4)!=Size || Be32(Fdt+20)!=17)
    return EFI_COMPROMISED_DATA;
  Struct=Be32(Fdt+8);Strings=Be32(Fdt+12);Rsv=Be32(Fdt+16);
  Ss=Be32(Fdt+36);Ts=Be32(Fdt+32);
  if((Struct&3) || (Rsv&7) || Rsv<40 || Rsv>Size-16 || Struct<40 || Struct>Size ||
     Ss>Size-Struct || Strings<40 || Strings>Size || Ts>Size-Strings ||
     FdtCheckHeader(Fdt)!=0 || fdt_check_full(Fdt,Size)!=0) return EFI_COMPROMISED_DATA;
  Count=FdtGetNumberOfReserveMapEntries(Fdt);
  if(Count<0 || (UINTN)Count+1>(Size-Rsv)/16 ||
     Overlap(Struct,Ss,Strings,Ts) || Overlap(Rsv,((UINTN)Count+1)*16,Struct,Ss) ||
     Overlap(Rsv,((UINTN)Count+1)*16,Strings,Ts)) return EFI_COMPROMISED_DATA;
  INT32 Length;CONST CHAR8 *Name=FdtGetName(Fdt,0,&Length);
  return Name && Length==0?EFI_SUCCESS:EFI_COMPROMISED_DATA;
}

/* Only these two saved ranges authorize AndroidBootInspect reads. Original
 * physical addresses are labels; they are never cast or dereferenced. */
STATIC CONST VOID *SnapshotRead(VOID *Context,UINT64 Address,UINTN Size)
{
  (VOID)Context;
  if(!mBootFdt.Copy) return NULL;
  if(Contains(mBootFdt.FdtOriginal,mBootFdt.FdtSize,Address,Size))
    return mBootFdt.Copy+(UINTN)(Address-mBootFdt.FdtOriginal);
  if(Contains(mBootFdt.TailOriginal,mBootFdt.TailSize,Address,Size))
    return mBootFdt.Copy+mBootFdt.TailOffset+(UINTN)(Address-mBootFdt.TailOriginal);
  return NULL;
}
EFI_STATUS CrBootFdtImport(VOID)
{
  CR_BOOT_DATA_HOB R;EFI_STATUS Status;
  if(mBootFdt.Imported) return mBootFdt.FdtStatus;
  mBootFdt.Imported=TRUE;mBootFdt.FdtStatus=mBootFdt.BootStatus=EFI_NOT_FOUND;
  Status=CaptureRecord(&R);
  if(Status!=EFI_SUCCESS) goto Done;
  mBootFdt.TailOffset=((UINTN)R.FdtSize+7)&~(UINTN)7;
  mBootFdt.CopySize=mBootFdt.TailOffset+(UINTN)R.TailSize;
  mBootFdt.Copy=AllocatePool(mBootFdt.CopySize);
  if(!mBootFdt.Copy) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  if(((UINTN)mBootFdt.Copy&7) ||
     Overlap((UINTN)mBootFdt.Copy,mBootFdt.CopySize,R.AllocationBase,R.AllocationSize)) {
    Status=EFI_COMPROMISED_DATA;goto Done;
  }
  mBootFdt.FdtSize=(UINTN)R.FdtSize;mBootFdt.TailSize=(UINTN)R.TailSize;
  mBootFdt.FdtOriginal=R.FdtOriginal;mBootFdt.TailOriginal=R.TailOriginal;
  CopyMem(mBootFdt.Copy,(CONST VOID *)(UINTN)R.FdtCopy,mBootFdt.FdtSize);
  if(mBootFdt.TailSize) CopyMem(mBootFdt.Copy+mBootFdt.TailOffset,(CONST VOID *)(UINTN)R.TailCopy,mBootFdt.TailSize);
  Status=ValidateFdt(mBootFdt.Copy,mBootFdt.FdtSize);
  if(Status!=EFI_SUCCESS) goto Done;
  mBootFdt.BootStatus=R.TailStatus==EFI_SUCCESS?
    CrAndroidBootInspect(R.FdtOriginal,SnapshotRead,NULL,&mBootFdt.BootInfo):Failure(R.TailStatus);
Done:
  mBootFdt.FdtStatus=Status;
  if(Status!=EFI_SUCCESS) {
    mBootFdt.BootStatus=Status;
    if(mBootFdt.Copy) {FreePool(mBootFdt.Copy);mBootFdt.Copy=NULL;}
  }
  return Status;
}
BOOLEAN CrBootFdtHobPresent(VOID)
{ return mBootFdt.Present || (mBootFdt.Imported && mBootFdt.FdtStatus!=EFI_NOT_FOUND); }
VOID CrBootFdtDiscard(VOID)
{ if(mBootFdt.Copy) FreePool(mBootFdt.Copy);ZeroMem(&mBootFdt,sizeof(mBootFdt)); }

STATIC BOOLEAN Output(CONST VOID *P,UINTN Size)
{
  UINTN A=(UINTN)P;
  return Range(A,Size) && !(A&(Size==sizeof(INT32)?3:7)) && !Overlap(A,Size,(UINTN)&mBootFdt,sizeof(mBootFdt)) &&
    !Overlap(A,Size,(UINTN)&mFdtProtocol,sizeof(mFdtProtocol)) &&
    (!mBootFdt.Copy || !Overlap(A,Size,(UINTN)mBootFdt.Copy,mBootFdt.CopySize));
}
STATIC BOOLEAN Pair(CONST VOID *A,UINTN N,CONST VOID *B,UINTN M)
{
  BOOLEAN First=Output(A,N),Second=Output(B,M);
  if(First)ZeroMem((VOID *)A,N);
  if(Second)ZeroMem((VOID *)B,M);
  return First&&Second&&!Overlap((UINTN)A,N,(UINTN)B,M);
}
STATIC BOOLEAN Text(CONST CHAR8 *S)
{
  if(!S) return FALSE;
  for(UINTN I=0;I<TEXT_MAX;++I) if(S[I]==0) return I!=0;
  return FALSE;
}
STATIC EFI_STATUS Ready(EFI_CR_FDT_PROTOCOL *This)
{ return This!=&mFdtProtocol?EFI_INVALID_PARAMETER:!mBootFdt.Imported?EFI_NOT_READY:mBootFdt.FdtStatus; }
STATIC EFI_STATUS EFIAPI GetBlob(EFI_CR_FDT_PROTOCOL *This,CONST VOID **Blob,UINTN *Size)
{
  if(!Pair(Blob,sizeof(*Blob),Size,sizeof(*Size))) return EFI_INVALID_PARAMETER;
  *Blob=NULL;*Size=0;EFI_STATUS S=Ready(This);if(S!=EFI_SUCCESS)return S;
  *Blob=mBootFdt.Copy;*Size=mBootFdt.FdtSize;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FindNode(EFI_CR_FDT_PROTOCOL *This,CONST CHAR8 *Path,INT32 *Node)
{
  if(!Output(Node,sizeof(*Node)))return EFI_INVALID_PARAMETER;*Node=-1;
  EFI_STATUS S=Ready(This);if(S!=EFI_SUCCESS)return S;
  if(!Text(Path)||Path[0]!='/')return EFI_INVALID_PARAMETER;
  INT32 N=FdtPathOffset(mBootFdt.Copy,Path);if(N<0)return N==-1?EFI_NOT_FOUND:EFI_INVALID_PARAMETER;
  *Node=N;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FindCompatible(EFI_CR_FDT_PROTOCOL *This,INT32 After,CONST CHAR8 *Compatible,INT32 *Node)
{
  if(!Output(Node,sizeof(*Node)))return EFI_INVALID_PARAMETER;*Node=-1;
  EFI_STATUS S=Ready(This);if(S!=EFI_SUCCESS)return S;
  if(!Text(Compatible)||After < -1 || (After>=0&&!FdtGetName(mBootFdt.Copy,After,NULL)))return EFI_INVALID_PARAMETER;
  INT32 N=FdtNodeOffsetByCompatible(mBootFdt.Copy,After,Compatible);
  if(N<0)return N==-1?EFI_NOT_FOUND:EFI_INVALID_PARAMETER;
  *Node=N;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetProperty(EFI_CR_FDT_PROTOCOL *This,INT32 Node,CONST CHAR8 *Name,CONST VOID **Data,UINTN *Size)
{
  if(!Pair(Data,sizeof(*Data),Size,sizeof(*Size)))return EFI_INVALID_PARAMETER;
  *Data=NULL;*Size=0;EFI_STATUS S=Ready(This);if(S!=EFI_SUCCESS)return S;
  if(!Text(Name)||Node<0||!FdtGetName(mBootFdt.Copy,Node,NULL))return EFI_INVALID_PARAMETER;
  INT32 Length;CONST VOID *Value=FdtGetProp(mBootFdt.Copy,Node,Name,&Length);
  if(!Value||Length<0)return Length==-1?EFI_NOT_FOUND:EFI_INVALID_PARAMETER;
  *Data=Value;*Size=(UINTN)Length;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetBootInfo(EFI_CR_FDT_PROTOCOL *This,CR_ANDROID_BOOT_INFO *Info)
{
  if(!Output(Info,sizeof(*Info)))return EFI_INVALID_PARAMETER;ZeroMem(Info,sizeof(*Info));
  if(This!=&mFdtProtocol)return EFI_INVALID_PARAMETER;
  if(!mBootFdt.Imported)return EFI_NOT_READY;
  if(mBootFdt.BootStatus!=EFI_SUCCESS)return mBootFdt.BootStatus;
  CopyMem(Info,&mBootFdt.BootInfo,sizeof(*Info));return EFI_SUCCESS;
}
STATIC EFI_CR_FDT_PROTOCOL mFdtProtocol={EFI_CR_FDT_PROTOCOL_REVISION,sizeof(EFI_CR_FDT_PROTOCOL),
  GetBlob,FindNode,FindCompatible,GetProperty,GetBootInfo};
EFI_CR_FDT_PROTOCOL *CrBootFdtProtocol(VOID) { return &mFdtProtocol; }
