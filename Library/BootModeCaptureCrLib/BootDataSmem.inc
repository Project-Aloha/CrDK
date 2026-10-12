/* SEC-only read qualification from the existing Linux/U-Boot-derived
 * SMEM item 402 decoder. No DDR mapping, allocation or RAM publication.
 * Private board reservations always win in Readable(). An explicitly named
 * Boot Data reservation additionally requires SMEM read qualification and
 * retains reserved ownership. SPDX-License-Identifier: MIT */
#include <Library/smem.h>
#include <Library/RamPartitionCrLib.h>

STATIC CR_RAM_PARTITION_TABLE mBootSmemRam;

STATIC VOID CaptureSmemRam(CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map, UINTN Count)
{
  SmemContext Context;
  CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Region=NULL;
  UINT8 First[CR_RAM_PARTITION_MAX_WIRE_SIZE], Second[CR_RAM_PARTITION_MAX_WIRE_SIZE];
  VOID *Item=NULL;
  UINT32 Size=0, Copied;
  CR_STATUS Status;
  ZeroMem(&mBootSmemRam,sizeof(mBootSmemRam));
  for(UINTN I=0;I<Count;++I) {
    if(!NameIs(&Map[I],"SMEM")) continue;
    if(Region) {
      DEBUG((DEBUG_ERROR,"BootDataCr: duplicate SMEM region\n"));
      return;
    }
    Region=&Map[I];
  }
  if(!Region || Region->Address>MAX_UINTN || Region->Length>MAX_UINTN ||
     Region->Address>MAX_UINTN-Region->Length || Region->Length<12496 ||
     Region->ResourceType!=MEM_RES || Region->MemoryType!=Reserv ||
     Region->ArmAttributes!=UNCACHED_UNBUFFERED_XN) {
    DEBUG((DEBUG_ERROR,"BootDataCr: missing or invalid SMEM region\n"));
    return;
  }
  Status=SmemInitialize(&Context,(VOID *)(UINTN)Region->Address,
                        (UINTN)Region->Length,Region->Address);
  DEBUG((DEBUG_ERROR,"BootDataCr: SMEM init=%r\n",Status));
  if(Status!=CR_SUCCESS) return;
  Status=SmemGetItem(&Context,SMEM_HOST_ANY,CR_RAM_PARTITION_SMEM_ITEM,&Size,&Item);
  DEBUG((DEBUG_ERROR,"BootDataCr: SMEM402 get=%r address=0x%Lx bytes=0x%x\n",
         Status,(UINT64)(UINTN)Item,Size));
  if(Status!=CR_SUCCESS || !Item || Size<20) return;
  /* SMEM may pad the allocation beyond the bounded RAM table wire format. */
  Copied=MIN(Size,(UINT32)sizeof(First));
  /* Use two bounded volatile snapshots; do not parse a changing remote item. */
  for(UINT32 I=0;I<Copied;++I) First[I]=((volatile UINT8 *)Item)[I];
  MemoryFence();
  for(UINT32 I=0;I<Copied;++I) Second[I]=((volatile UINT8 *)Item)[I];
  if(CompareMem(First,Second,Copied)!=0) {
    DEBUG((DEBUG_ERROR,"BootDataCr: SMEM402 changed during snapshot\n"));
    return;
  }
  /* Report only the stable, bounded local copy; never scan an item remotely. */
  DEBUG((DEBUG_ERROR,"BootDataCr: SMEM402 magic=0x%x:0x%x version=%u entries=%u copied=0x%x\n",
         ReadUnaligned32((CONST UINT32 *)First),ReadUnaligned32((CONST UINT32 *)(First+4)),
         ReadUnaligned32((CONST UINT32 *)(First+8)),ReadUnaligned32((CONST UINT32 *)(First+16)),Copied));
  Status=CrRamPartitionParse(First,Copied,&mBootSmemRam);
  if(Status!=CR_SUCCESS)
    ZeroMem(&mBootSmemRam,sizeof(mBootSmemRam));
  DEBUG((DEBUG_ERROR,"BootDataCr: SMEM402 parse=%r banks=%u available=0x%Lx\n",
         Status,mBootSmemRam.BankCount,mBootSmemRam.AvailableBytes));
  for(UINT32 I=0;I<mBootSmemRam.BankCount;++I) {
    CONST CR_RAM_PARTITION_ENTRY *B=&mBootSmemRam.Banks[I];
    DEBUG((DEBUG_ERROR,"BootDataCr: RAM%u base=0x%Lx length=0x%Lx available=0x%Lx attr=%u domain=%u\n",
           I,B->Base,B->Length,B->AvailableLength,B->Attribute,B->Domain));
  }
}

STATIC UINT64 SmemReadableEnd(UINT64 At,UINT64 End)
{
  for(UINT32 I=0;I<mBootSmemRam.BankCount;++I) {
    CONST CR_RAM_PARTITION_ENTRY *B=&mBootSmemRam.Banks[I];
    if(B->Attribute==CR_RAM_PARTITION_READWRITE && B->Base<=At &&
       At-B->Base<B->AvailableLength)
      return MIN(End,B->Base+B->AvailableLength);
  }
  return At;
}

STATIC BOOLEAN SmemBootDataRegion(
  CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *R,UINT64 Address,UINT64 End)
{
  /* This is an explicit board opt-in for ABL-owned normal DDR, not a way
     to read arbitrary firmware reservations. Never publish it as free RAM. */
  if(!NameIs(R,"Boot Data") || !NormalRam(R) || R->ResourceType!=SYS_MEM ||
     R->MemoryType!=Reserv || R->HobOption!=AddMem) return FALSE;
  UINT64 At=MAX(Address,R->Address),Limit=MIN(End,R->Address+R->Length);
  while(At<Limit) {
    UINT64 Next=SmemReadableEnd(At,Limit);
    if(Next==At) return FALSE;
    At=Next;
  }
  return TRUE;
}
