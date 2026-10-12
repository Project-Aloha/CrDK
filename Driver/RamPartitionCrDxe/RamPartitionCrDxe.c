/** SMEM DDR discovery and reservation-aware native memory publication. MIT */
#include <PiDxe.h>
#include <AArch64.h>
#include <Library/ArmLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeMemoryProtectionHobLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PlatformMemoryMapLib.h>
#include <Library/RamPartitionCrLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/smem.h>
#include <Protocol/EFIRamPartition.h>
#include <Protocol/EFICrRamRange.h>
#include <Protocol/EFISmemCrProtocol.h>

STATIC CR_RAM_PARTITION_TABLE mRam;
STATIC EFI_RAMPARTITION_PROTOCOL mProtocol;
STATIC EFI_CR_RAM_RANGE_PROTOCOL mRangeProtocol;
STATIC BOOLEAN mRamInventoryReady;
STATIC EFI_HANDLE mRamProtocolHandle;

STATIC EFI_STATUS EFIAPI CheckAppsReadWriteRange(EFI_CR_RAM_RANGE_PROTOCOL *This,UINT64 Base,UINT64 Bytes){
  if(This!=&mRangeProtocol||!Bytes||((Base|Bytes)&EFI_PAGE_MASK)||Base>MAX_UINT64-Bytes)return EFI_INVALID_PARAMETER;
  if(!mRamInventoryReady)return EFI_NOT_READY;
  for(UINT32 I=0;I<mRam.BankCount;++I){
    CONST CR_RAM_PARTITION_ENTRY *B=&mRam.Banks[I];
    if(B->Attribute==CR_RAM_PARTITION_READWRITE&&B->AvailableLength&&
      B->Base<=Base&&B->AvailableLength<=MAX_UINT64-B->Base&&
      Bytes<=B->AvailableLength&&Base-B->Base<=B->AvailableLength-Bytes)return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

/* Diagnose rejected boot tables without trusting their count, alignment or
 * string termination. These are metadata reads from the private snapshot. */
STATIC UINT32 Wire32(CONST UINT8 *P){
  return (UINT32)P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);
}
STATIC UINT64 Wire64(CONST UINT8 *P){return Wire32(P)|((UINT64)Wire32(P+4)<<32);}
STATIC VOID DescribeWire(CONST UINT8 *P,UINTN Size){
  if(Size<20)return;
  UINT32 Version=Wire32(P+8),Count=Wire32(P+16);
  DEBUG((DEBUG_ERROR,"RamPartitionCr: wire magic=%08x/%08x version=%u reserved=%08x count=%u\n",
    Wire32(P),Wire32(P+4),Version,Wire32(P+12),Count));
  if(Version>3)return;
  UINTN Header=Version?24:20,Stride=Version>=2?72:Version?64:56;
  if(Size<Header)return;
  Count=MIN(Count,CR_RAM_PARTITION_MAX_ENTRIES);Count=MIN(Count,(Size-Header)/Stride);
  for(UINT32 I=0;I<Count;++I){
    CONST UINT8 *E=P+Header+I*Stride;UINTN A=Version?32:24;
    UINT64 Base=Version?Wire64(E+16):Wire32(E+16),Length=Version?Wire64(E+24):Wire32(E+20);
    DEBUG((DEBUG_ERROR,"RamPartitionCr: wire%u base=%016lx length=%016lx available=%016lx attr=%u category=%u domain=%u type=%u\n",
      I,Base,Length,Version>=2?Wire64(E+64):Length,Wire32(E+A),Wire32(E+A+4),Wire32(E+A+8),Wire32(E+A+12)));
  }
}

STATIC EFI_STATUS EFIAPI GetVersion(EFI_RAMPARTITION_PROTOCOL *This,UINT32 *Major,UINT32 *Minor){
  if(This!=&mProtocol||!Major||!Minor)return EFI_INVALID_PARAMETER;
  *Major=mRam.Version;*Minor=0;return EFI_SUCCESS;
}
STATIC EFI_STATUS Metadata(EFI_RAMPARTITION_PROTOCOL *This,UINT32 *Value,BOOLEAN Pasr){
  if(This!=&mProtocol||!Value)return EFI_INVALID_PARAMETER;
  *Value=0;if(mRam.Version!=3||!mRam.BankCount)return EFI_UNSUPPORTED;
  UINT32 First=Pasr?mRam.Banks[0].MinPasrSize:mRam.Banks[0].HighestBankBit;
  for(UINT32 I=1;I<mRam.BankCount;++I)
    if((Pasr?mRam.Banks[I].MinPasrSize:mRam.Banks[I].HighestBankBit)!=First)return EFI_UNSUPPORTED;
  *Value=First;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetBankBit(EFI_RAMPARTITION_PROTOCOL *This,UINT32 *Value){return Metadata(This,Value,FALSE);}
STATIC EFI_STATUS EFIAPI GetPasr(EFI_RAMPARTITION_PROTOCOL *This,UINT32 *Value){return Metadata(This,Value,TRUE);}
STATIC EFI_STATUS EFIAPI GetPartitions(EFI_RAMPARTITION_PROTOCOL *This,RamPartitionEntry *Entries,UINT32 *Count){
  if(This!=&mProtocol||!Count)return EFI_INVALID_PARAMETER;
  UINT32 Capacity=*Count;*Count=mRam.BankCount;
  if(!Entries||Capacity<mRam.BankCount)return EFI_BUFFER_TOO_SMALL;
  for(UINT32 I=0;I<mRam.BankCount;++I){Entries[I].Base=mRam.Banks[I].Base;Entries[I].AvailableLength=mRam.Banks[I].AvailableLength;}
  return EFI_SUCCESS;
}

/* Platform entries can reserve memory without a GCD HOB (NoHob/AllocOnly).
 * Exclude every entry, not only those called Reserved or present in GCD. */
STATIC EFI_STATUS PlatformCount(CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map,UINTN *Count){
  if(!Map||!Count)return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT;++I){
    if(!Map[I].Length){*Count=I;return EFI_SUCCESS;}
    if(Map[I].Address>MAX_UINT64-Map[I].Length||
       ((Map[I].Address|Map[I].Length)&EFI_PAGE_MASK))return EFI_COMPROMISED_DATA;
  }
  return EFI_COMPROMISED_DATA;
}
STATIC VOID PlatformSpan(CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map,UINTN Count,
                         UINT64 Cursor,UINT64 Limit,UINT64 *Next,BOOLEAN *Excluded){
  *Next=Limit;*Excluded=FALSE;
  for(UINTN I=0;I<Count;++I){
    UINT64 Base=Map[I].Address,End=Base+Map[I].Length;
    if(Cursor>=End)continue;
    if(Cursor>=Base){*Excluded=TRUE;*Next=MIN(*Next,End);}
    else *Next=MIN(*Next,Base);
  }
}
STATIC EFI_STATUS AddBank(CONST CR_RAM_PARTITION_ENTRY *Bank,
                          CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map,UINTN Count,UINT64 *Added){
  if(Bank->Attribute!=CR_RAM_PARTITION_READWRITE||!Bank->AvailableLength)return EFI_SUCCESS;
  /* Match the policy used by DXE for newly available conventional memory.
   * When NX policy is disabled, AllocatePages does not change permissions;
   * forcing XP here would leave a later EfiLoaderCode allocation unusable. */
  UINT64 InitialAttributes=EFI_MEMORY_WB;
  if(gDxeMps.NxProtectionPolicy.Fields.EfiConventionalMemory)
    InitialAttributes|=EFI_MEMORY_XP;
  UINT64 Cursor=Bank->Base,Limit=Cursor+Bank->AvailableLength;
  while(Cursor<Limit){
    UINT64 Next;BOOLEAN Excluded;EFI_GCD_MEMORY_SPACE_DESCRIPTOR Gcd;
    PlatformSpan(Map,Count,Cursor,Limit,&Next,&Excluded);
    if(Excluded){Cursor=Next;continue;}
    EFI_STATUS S=gDS->GetMemorySpaceDescriptor(Cursor,&Gcd);if(S!=EFI_SUCCESS)return S;
    if(!Gcd.Length||Gcd.BaseAddress>Cursor||Gcd.BaseAddress>MAX_UINT64-Gcd.Length||
       Gcd.BaseAddress+Gcd.Length<=Cursor)return EFI_COMPROMISED_DATA;
    Next=MIN(Next,Gcd.BaseAddress+Gcd.Length);
    if(Next<=Cursor||((Cursor|Next)&EFI_PAGE_MASK))return EFI_COMPROMISED_DATA;
    if(Gcd.GcdMemoryType!=EfiGcdMemoryTypeNonExistent){Cursor=Next;continue;}
    /* Keep bounded publication chunks, and obtain a fresh GCD descriptor
     * after each mutation. Never convert reserved/MMIO/already added RAM. */
    UINT64 Length=MIN(Next-Cursor,0x80000000ULL);
    /* CoreAddMemorySpace(SystemMemory) immediately makes pages allocatable.
     * Translation must be valid before it can allocate from these pages. */
    /* Defer memory-map-change notifications until GCD attributes agree.
     * A callback must not load code into a newly published range before a
     * whole-range attribute update. No resource RPC or hardware wait occurs here. */
    EFI_TPL OldTpl=gBS->RaiseTPL(TPL_NOTIFY);
    S=ArmSetMemoryAttributes(Cursor,Length,InitialAttributes,0);
    /* Capabilities describe supported future mappings, not the active one.
     * CpuDxe/ArmMmuLib support WC as Normal non-cacheable memory; the SMMU
     * common-buffer allocator requires it for CPU-owned DMA rings. Keep
     * discovered RAM initially WB with the configured NX policy, while
     * allowing that checked per-allocation transition. UC is Device memory
     * on Arm and is not a substitute for this normal-memory capability. */
    if(S==EFI_SUCCESS)S=gDS->AddMemorySpace(EfiGcdMemoryTypeSystemMemory,Cursor,Length,
                                          EFI_MEMORY_WB|EFI_MEMORY_WC|EFI_MEMORY_XP);
    if(S==EFI_SUCCESS){
      *Added+=Length;
      S=gDS->SetMemorySpaceAttributes(Cursor,Length,InitialAttributes);
    }
    gBS->RestoreTPL(OldTpl);
    DEBUG((S==EFI_SUCCESS?DEBUG_INIT:DEBUG_ERROR,
      "RamPartitionCr: add %016lx +%016lx attributes=%lx: %r\n",Cursor,Length,InitialAttributes,S));
    if(S!=EFI_SUCCESS)return S;
    Cursor+=Length;
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI RamPartitionCrDxeEntryPoint(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable){
  (VOID)ImageHandle;(VOID)SystemTable;
  if(mRamInventoryReady||mRamProtocolHandle)return EFI_ALREADY_STARTED;
  EFI_SMEM_CR_PROTOCOL *Smem=NULL;VOID *Remote=NULL;UINT32 Size=0;
  EFI_STATUS S=gBS->LocateProtocol(&gEfiSmemCrProtocolGuid,NULL,(VOID **)&Smem);
  if(S!=EFI_SUCCESS)return S;
  if(!Smem||Smem->Revision!=EFI_SMEM_CR_PROTOCOL_REVISION||!Smem->GetItem)return EFI_UNSUPPORTED;
  S=Smem->GetItem(Smem,SMEM_HOST_ANY,CR_RAM_PARTITION_SMEM_ITEM,&Size,&Remote);
  if(S!=EFI_SUCCESS){DEBUG((DEBUG_ERROR,"RamPartitionCr: SMEM402: %r\n",S));return S;}
  if(!Remote||!Size)return EFI_COMPROMISED_DATA;
  /* XBL publishes this immutable boot table. Parse private bounded bytes;
   * never modify SMEM or synthesize a density when the item is absent. */
  UINT8 Snapshot[CR_RAM_PARTITION_MAX_WIRE_SIZE];
  UINTN Copied=MIN((UINTN)Size,sizeof(Snapshot));CopyMem(Snapshot,Remote,Copied);
  S=CrRamPartitionParse(Snapshot,Copied,&mRam);
  if(S!=EFI_SUCCESS){DEBUG((DEBUG_ERROR,"RamPartitionCr: SMEM402 size=%u invalid: %r\n",Size,S));DescribeWire(Snapshot,Copied);return S;}
  CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map=GetPlatformMemoryMap();UINTN Count=0;
  S=PlatformCount(Map,&Count);if(S!=EFI_SUCCESS)return S;
  UINTN Bits=MIN(ArmGetPhysicalAddressBits(),64U-(ArmGetTCR()&0x3fU));
  if(Bits<32||Bits>=64)return EFI_UNSUPPORTED;
  UINT64 AddressLimit=1ULL<<Bits;
  /* Validate all banks before changing page tables/GCD. AvailableLength is
   * the APPS grant; Length also includes unavailable hypervisor carveouts. */
  for(UINT32 I=0;I<mRam.BankCount;++I){
    CONST CR_RAM_PARTITION_ENTRY *B=&mRam.Banks[I];
    if(B->Base>=AddressLimit||B->AvailableLength>AddressLimit-B->Base)return EFI_UNSUPPORTED;
    DEBUG((DEBUG_INIT,"RamPartitionCr: bank%u base=%016lx physical=%016lx available=%016lx attr=%u domain=%u\n",
      I,B->Base,B->Length,B->AvailableLength,B->Attribute,B->Domain));
  }
  UINT64 Added=0;
  for(UINT32 I=0;I<mRam.BankCount;++I){
    S=AddBank(&mRam.Banks[I],Map,Count,&Added);
    if(S!=EFI_SUCCESS){
      /* Earlier valid additions remain usable. They do not point into this
       * image, and cannot be withdrawn after allocations become possible. */
      DEBUG((DEBUG_ERROR,"RamPartitionCr: bank%u stopped: %r; added=%016lx\n",I,S,Added));return S;
    }
  }
  mProtocol=(EFI_RAMPARTITION_PROTOCOL){EFI_RAMPARTITION_PROTOCOL_REVISION,GetVersion,GetBankBit,GetPartitions,GetPasr};
  mRangeProtocol=(EFI_CR_RAM_RANGE_PROTOCOL){EFI_CR_RAM_RANGE_PROTOCOL_REVISION,CheckAppsReadWriteRange};
  /* No reparsing once either protocol can be observed. Both methods point to
   * this immutable, fully validated private boot snapshot. */
  mRamInventoryReady=TRUE;
  S=gBS->InstallMultipleProtocolInterfaces(&mRamProtocolHandle,&gEfiRamPartitionProtocolGuid,&mProtocol,
    &gEfiCrRamRangeProtocolGuid,&mRangeProtocol,NULL);
  DEBUG((S==EFI_SUCCESS?DEBUG_INIT:DEBUG_ERROR,
    "RamPartitionCr: table-v%u banks=%u physical=%016lx available=%016lx added=%016lx protocol=%r\n",
    mRam.Version,mRam.BankCount,mRam.PhysicalBytes,mRam.AvailableBytes,Added,S));
  if(S!=EFI_SUCCESS&&mRamProtocolHandle){
    DEBUG((DEBUG_ERROR,"RamPartitionCr: publication uncertain; retain image handle=%p status=%r\n",mRamProtocolHandle,S));
    return EFI_SUCCESS; /* Any published method pointers must stay resident. */
  }
  return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;
}
