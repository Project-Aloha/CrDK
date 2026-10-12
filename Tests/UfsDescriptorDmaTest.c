/** Exercise actual UFS requests through disjoint CPU/IOVA mappings.
 * The wire oracle uses public Android/Linux 512-byte UPIU regions. It is
 * not a hardware model of undocumented Waipio prefetch or the board fault.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "UfsPassThruHci.c"
#include "UfsRequestResources.inc"

EFI_BOOT_SERVICES *gBS;
EDKII_UFS_HC_PLATFORM_PROTOCOL *mUfsHcPlatform;
static UFS_PASS_THRU_PRIVATE_DATA Owner;
static UINT32 Regs[0x100 / 4];
static EFI_TPL Tpl = TPL_APPLICATION;
static UINT64 IovaBase;
static BOOLEAN PagesUsed[128], AllowPacked, PoisonSlots;
static unsigned Buffers, Pools, LiveMaps, Requests, GuardRejects, Allocations;
typedef struct {
  VOID *Host;
  UINTN Bytes, First, Pages;
  UINT64 Device;
  EDKII_UFS_HOST_CONTROLLER_OPERATION Operation;
  BOOLEAN Live;
} DMA_MAP;
static DMA_MAP Maps[16];

VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) {}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return FALSE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID EFIAPI DebugAssert (CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Text) { abort(); }
VOID *EFIAPI ZeroMem (VOID *P, UINTN N) { return memset(P, 0, N); }
VOID *EFIAPI CopyMem (VOID *P, CONST VOID *Q, UINTN N) { return memcpy(P, Q, N); }
VOID *EFIAPI AllocateZeroPool (UINTN N) { Pools++; return calloc(1, N); }
VOID EFIAPI FreePool (VOID *P) { assert(Pools); Pools--; free(P); }
UINT64 EFIAPI MultU64x32 (UINT64 A, UINT32 B) { return A * B; }
UINT64 EFIAPI DivU64x32 (UINT64 A, UINT32 B) { return A / B; }
UINT64 EFIAPI RShiftU64 (UINT64 A, UINTN B) { return A >> B; }
UINTN EFIAPI MicroSecondDelay (UINTN N) { return N; }
LIST_ENTRY *EFIAPI InitializeListHead (LIST_ENTRY *L) { L->ForwardLink=L->BackLink=L; return L; }
LIST_ENTRY *EFIAPI InsertTailList (LIST_ENTRY *H, LIST_ENTRY *E) {
  E->BackLink=H->BackLink; E->ForwardLink=H; H->BackLink->ForwardLink=E; H->BackLink=E; return H;
}
LIST_ENTRY *EFIAPI RemoveEntryList (CONST LIST_ENTRY *E) {
  E->BackLink->ForwardLink=E->ForwardLink; E->ForwardLink->BackLink=E->BackLink; return E->ForwardLink;
}
static EFI_TPL EFIAPI Raise (EFI_TPL T) { EFI_TPL Old=Tpl; assert(T>=Old); Tpl=T; return Old; }
static VOID EFIAPI Restore (EFI_TPL T) { assert(T<=Tpl); Tpl=T; }
static EFI_STATUS EFIAPI Signal (EFI_EVENT E) { return EFI_SUCCESS; }

static EFI_STATUS EFIAPI Allocate (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H,
  EFI_ALLOCATE_TYPE T, EFI_MEMORY_TYPE M, UINTN Pages, VOID **P, UINT64 Attr)
{
  *P=aligned_alloc(4096, Pages*4096); assert(*P);
  memset(*P, 0xa5, Pages*4096); Buffers++; Allocations++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, UINTN Pages, VOID *P)
{
  for(unsigned I=0; I<16; I++) assert(!Maps[I].Live || Maps[I].Host!=P);
  assert(Buffers); Buffers--; free(P); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Map (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H,
  EDKII_UFS_HOST_CONTROLLER_OPERATION Op, VOID *P, UINTN *Bytes,
  EFI_PHYSICAL_ADDRESS *Device, VOID **Token)
{
  UINTN Pages=(*Bytes+4095)/4096, First, I;
  DMA_MAP *M=NULL;
  for(I=0; I<16; I++) if(!Maps[I].Live) { M=&Maps[I]; break; }
  assert(M && Pages<=128);
  for(First=0; First+Pages<=128; First++) {
    for(I=0; I<Pages && !PagesUsed[First+I]; I++) {}
    if(I==Pages) break;
  }
  assert(First+Pages<=128);
  for(I=0; I<Pages; I++) PagesUsed[First+I]=TRUE;
  *M=(DMA_MAP){.Host=P, .Bytes=*Bytes, .First=First, .Pages=Pages,
    .Device=IovaBase+First*4096, .Operation=Op, .Live=TRUE};
  assert(M->Device!=(UINTN)P);
  *Device=M->Device; *Token=M; LiveMaps++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, VOID *Token)
{
  DMA_MAP *M=Token; assert(M->Live && LiveMaps);
  if(Regs[UFS_HC_IS_OFFSET/4]&0x20000) assert(Regs[UFS_HC_ENABLE_OFFSET/4]==0);
  for(UINTN I=0; I<M->Pages; I++) { assert(PagesUsed[M->First+I]); PagesUsed[M->First+I]=FALSE; }
  M->Live=FALSE; LiveMaps--; return EFI_SUCCESS;
}
static UINT8 *Dma (UINT64 Address, UINTN Bytes, BOOLEAN Write)
{
  for(unsigned I=0; I<16; I++) {
    DMA_MAP *M=&Maps[I];
    if(!M->Live || Address<M->Device || Address-M->Device>M->Bytes ||
       Bytes>M->Bytes-(Address-M->Device)) continue;
    assert(M->Operation==EdkiiUfsHcOperationBusMasterCommonBuffer ||
      M->Operation==(Write ? EdkiiUfsHcOperationBusMasterWrite : EdkiiUfsHcOperationBusMasterRead));
    return (UINT8 *)M->Host+(UINTN)(Address-M->Device);
  }
  GuardRejects++; return NULL;
}
static UINT32 Le32 (CONST UINT8 *P) { return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24); }
static UINT16 Le16 (CONST UINT8 *P) { return P[0]|((UINT16)P[1]<<8); }
static UINT64 Le64 (CONST UINT8 *P) { return Le32(P)|((UINT64)Le32(P+4)<<32); }
static UINT32 Be32 (CONST UINT8 *P) { return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3]; }
static UINT64 ListAddress (VOID) {
  return Regs[UFS_HC_UTRLBA_OFFSET/4]|((UINT64)Regs[UFS_HC_UTRLBAU_OFFSET/4]<<32);
}

static void Execute (UINT32 Mask)
{
  assert(Regs[UFS_HC_UTRLRSR_OFFSET/4]==1);
  for(UINTN Slot=0; Slot<32; Slot++) if(Mask&(1U<<Slot)) {
    UINT8 *Trd=Dma(ListAddress()+Slot*32, 32, FALSE); assert(Trd);
    UINT64 EncodedUcd=Le64(Trd+16), Ucd=EncodedUcd&~127ULL;
    unsigned RspLength=Le16(Trd+24)*4, RspOffset=Le16(Trd+26)*4;
    unsigned PrdtCount=Le16(Trd+28), PrdtOffset=Le16(Trd+30)*4;
    UINT8 *Command=Dma(Ucd, 512, FALSE); assert(Command);
    fprintf(stderr, "DMA oracle: slot=%lu UCD=%llx response=%u/%u PRDT=%u/%u\n",
      (unsigned long)Slot, (unsigned long long)Ucd, RspOffset, RspLength, PrdtOffset, PrdtCount);
    // Independent of production UTP bitfields/UPIU-size constants.
    assert(AllowPacked || (RspOffset==512 && RspLength==512));
    assert((Le32(Trd)&~0x06000000U)==0x11000000U);
    assert(Le32(Trd+4)==0 && Le32(Trd+8)==15 && Le32(Trd+12)==0 && !(EncodedUcd&127));
    assert(Command[3]==Slot && Command[8]==0);
    UINT8 *Response=Dma(Ucd+RspOffset, RspLength, TRUE); assert(Response);
    memset(Response, 0, RspLength); Response[2]=Command[2]; Response[3]=Command[3];
    if(Command[0]==0) {
      assert(PrdtCount==0 && PrdtOffset==1024 && ((Le32(Trd)>>25)&3)==0);
      assert(Ucd==IovaBase+4096+Slot*3072 && LiveMaps==3);
      // Every address belongs to allocated command memory, not a guard grant.
      assert(Dma(Ucd, 3072, FALSE));
      assert(Dma(IovaBase+26*4096, 1, FALSE)==NULL);
      Response[0]=0x20;
    } else if(Command[0]==0x16) {
      assert(PrdtCount==0 && PrdtOffset==1024 && ((Le32(Trd)>>25)&3)==0);
      unsigned Length=((unsigned)Command[18]<<8)|Command[19];
      Response[0]=0x36;
      if(Command[12]==1) {
        assert(Length==255 && Command[10]==0 && Command[11]==0);
        Response[11]=255; Response[19]=255;
        for(unsigned I=0; I<255; I++) Response[32+I]=(UINT8)I;
      } else if(Command[12]==2) {
        assert(Length==255 && Command[11]==255);
        for(unsigned I=0; I<255; I++) assert(Command[32+I]==(UINT8)(255-I));
        for(unsigned I=287; I<512; I++) assert(Command[I]==0);
        Response[19]=255;
      } else { assert(Command[12]==6); }
    } else {
      assert(Command[0]==1 && PrdtOffset==1024 && PrdtCount);
      UINT8 *Prdt=Dma(Ucd+PrdtOffset, PrdtCount*16, FALSE); assert(Prdt);
      UINTN Total=0; BOOLEAN Write=((Le32(Trd)>>25)&3)==2;
      for(unsigned I=0; I<PrdtCount; I++) {
        UINT64 Address=Le64(Prdt+I*16); UINT32 Bytes=Le32(Prdt+I*16+12)+1;
        assert(Le32(Prdt+I*16+8)==0 && Bytes<=262144 && !(Bytes&3));
        UINT8 *Data=Dma(Address, Bytes, Write); assert(Data);
        if(Write) memset(Data, 0x5a, Bytes);
        else for(UINTN J=0; J<Bytes; J++) assert(Data[J]==0xa6);
        Total+=Bytes;
      }
      assert(Total==Be32(Command+12)); Response[0]=0x21;
    }
    Trd=Dma(ListAddress()+Slot*32, 32, TRUE); assert(Trd); Trd[8]=0;
  }
}
static EFI_STATUS EFIAPI Read (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH W, UINT64 Offset, UINTN Count, VOID *P)
{
  assert(W==EfiUfsHcWidthUint32 && Count==1 && !(Offset&3) && Offset<0x100);
  *(UINT32 *)P=Regs[Offset/4]; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH W, UINT64 Offset, UINTN Count, VOID *P)
{
  UINT32 Value=*(UINT32 *)P;
  assert(W==EfiUfsHcWidthUint32 && Count==1 && !(Offset&3) && Offset<0x100);
#ifdef UFS_DMA_POOL_CONTRACT_ONLY
  if(Offset==UFS_HC_UTRLRSR_OFFSET && Value==1) {
    // Inspect at the publication write, not only after initialization returns.
    UINT8 *First=Dma(ListAddress(),32,FALSE); assert(First);
    UINT64 Base=Le64(First+16);
    assert(Base!=0 && "Every UTRD needs its UCD before the list starts");
    unsigned Slots=(Owner.UfsHcInfo.Capabilities&31)+1;
    for(unsigned Slot=0; Slot<Slots; Slot++) {
      UINT8 *T=Dma(ListAddress()+Slot*32,32,FALSE); assert(T);
      assert(Le64(T+16)==Base+Slot*3072 && Le16(T+24)==128 && Le16(T+26)==128);
      assert(Le16(T+28)==0 && Le16(T+30)==256 && Dma(Base+Slot*3072,3072,FALSE));
    }
  }
#endif
  Regs[Offset/4]=Value;
  if(Offset==UFS_HC_UTRLDBR_OFFSET) {
    Requests++;
    Execute(Value);
    Regs[Offset/4]=0;
  }
  if(Offset==UFS_HC_ENABLE_OFFSET && Value==0) {
    Regs[UFS_HC_UTRLDBR_OFFSET/4]=0;
    Regs[UFS_HC_UTRLRSR_OFFSET/4]=Regs[UFS_HC_UTMRLRSR_OFFSET/4]=0;
  }
  return EFI_SUCCESS;
}
#ifndef UFS_DMA_POOL_CONTRACT_ONLY
static void RequestClean (void) {
  assert(LiveMaps==3 && Buffers==3 && !Pools && !Owner.ReservedSlots && Tpl==TPL_APPLICATION);
  assert(Owner.Queue.ForwardLink==&Owner.Queue);
}
int main (void)
{
  EFI_BOOT_SERVICES Bs={.RaiseTPL=Raise, .RestoreTPL=Restore, .SignalEvent=Signal};
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL H={.Read=Read, .Write=Write, .Map=Map,
    .Unmap=Unmap, .AllocateBuffer=Allocate, .FreeBuffer=Free};
  gBS=&Bs; AllowPacked=getenv("UFS_DMA_ORACLE_ALLOW_PACKED")!=NULL;
  PoisonSlots=getenv("UFS_DMA_ORACLE_CLEAN_TRD")==NULL;
  for(unsigned Wide=0; Wide<2; Wide++) {
    unsigned AllocationsBefore=Allocations;
    memset(&Owner, 0, sizeof(Owner)); memset(Regs, 0, sizeof(Regs));
    Owner.UfsHostController=&H; Owner.UfsHcInfo.Capabilities=31|(7U<<16)|UFS_HC_CAP_64ADDR;
    IovaBase=Wide ? 0x110000000ULL : 0x10000000;
    InitializeListHead(&Owner.Queue); Regs[UFS_HC_ENABLE_OFFSET/4]=1;
    assert(UfsInitTaskManagementRequestList(&Owner)==EFI_SUCCESS);
    assert(UfsInitTransferRequestList(&Owner)==EFI_SUCCESS);
    assert(ListAddress()==IovaBase+25*4096 && Owner.Nutrs==32 && Owner.Nutmrs==8);
    assert(Allocations==AllocationsBefore+3);
    // Decode every descriptor before any request can replace its fields.
    for(UINTN Slot=0; Slot<32; Slot++) {
      UINT8 *T=Dma(ListAddress()+Slot*32, 32, FALSE); assert(T);
      assert(Le64(T+16)==IovaBase+4096+Slot*3072);
      assert(Le16(T+24)==128 && Le16(T+26)==128 && Le16(T+28)==0 && Le16(T+30)==256);
      UINT8 *C=Dma(Le64(T+16), 3072, FALSE); assert(C);
      for(UINTN I=0; I<3072; I++) assert(C[I]==0);
    }
    for(UINTN Slot=0; Slot<32; Slot++) {
      // Poison reused HCI crypto/EHS/reserved fields, not just PRDT fields.
      if(PoisonSlots) memset((UINT8 *)Owner.UtpTrlBase+Slot*32, 0xff, 32);
      UINT32 Held=~(1U<<Slot); Owner.ReservedSlots=Held;
      assert(UfsExecNopCmds(&Owner)==EFI_SUCCESS && Owner.ReservedSlots==Held);
      Owner.ReservedSlots=0; RequestClean();
    }
    assert(Allocations==AllocationsBefore+3); // NOPs never allocate or remap UCDs.
    UINT8 Descriptor[256]; UINT32 Size=255;
    assert(UfsRwDeviceDesc(&Owner, TRUE, UfsUnitDesc, 0, 0, Descriptor, &Size)==EFI_SUCCESS && Size==255);
    for(unsigned I=0; I<255; I++) assert(Descriptor[I]==I);
    RequestClean();
    for(unsigned I=0; I<255; I++) Descriptor[I]=(UINT8)(255-I);
    Size=255;
    assert(UfsRwDeviceDesc(&Owner, FALSE, UfsConfigDesc, 0, 0, Descriptor, &Size)==EFI_SUCCESS && Size==255);
    RequestClean();
    unsigned Before=Requests; Size=256;
    assert(UfsRwDeviceDesc(&Owner, TRUE, UfsUnitDesc, 0, 0, Descriptor, &Size)==EFI_INVALID_PARAMETER);
    assert(Requests==Before && Size==0); RequestClean();
    UINT8 Flag=1;
    UFS_DEVICE_MANAGEMENT_REQUEST_PACKET Query={.Opcode=UtpQueryFuncOpcodeSetFlag,
      .DescId=UfsFlagDevInit, .DataBuffer=&Flag, .TransferLength=1, .Timeout=1000};
    assert(UfsSendDmRequest(&Owner, &Query)==EFI_SUCCESS);
    RequestClean();
    UINT8 *Data=malloc(262656); assert(Data);
    UINT8 Cdb[10]={0x2a, 0, 0, 0, 0, 0, 0, 2, 1}; memset(Data, 0xa6, 262656);
    EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet={.Cdb=Cdb, .CdbLength=10,
      .DataDirection=EFI_EXT_SCSI_DATA_DIRECTION_WRITE, .OutDataBuffer=Data, .OutTransferLength=262656, .Timeout=1000};
    assert(UfsExecScsiCmds(&Owner, 0, &Packet, NULL)==EFI_SUCCESS); RequestClean();
    Cdb[0]=0x28; Packet.DataDirection=EFI_EXT_SCSI_DATA_DIRECTION_READ;
    Packet.InDataBuffer=Data; Packet.InTransferLength=262656;
    assert(UfsExecScsiCmds(&Owner, 0, &Packet, NULL)==EFI_SUCCESS);
    assert(Data[0]==0x5a && Data[262655]==0x5a); RequestClean(); free(Data);
    assert(UfsControllerStop(&Owner)==EFI_SUCCESS);
    assert(Unmap(&H, Owner.TrlMapping)==EFI_SUCCESS && Unmap(&H, Owner.TmrlMapping)==EFI_SUCCESS);
    assert(Free(&H, 1, Owner.UtpTrlBase)==EFI_SUCCESS && Free(&H, 1, Owner.UtpTmrlBase)==EFI_SUCCESS);
    assert(Unmap(&H, Owner.UcdMapping)==EFI_SUCCESS);
    assert(Free(&H, 24, Owner.UtpUcdBase)==EFI_SUCCESS);
    assert(!LiveMaps && !Buffers && !Pools);
  }
  assert(GuardRejects==64);
  puts("UFS DMA oracle: disjoint CPU/32-bit and 33-bit IOVA, 32 permanent initialized slots, allocation-free NOP, backing-end denial, query255/256 and multi-PRD read/write PASS");
  return 0;
}
#else
// This contract uses only the original private fields, so it also runs against
// the archived UFS102 production source. It asserts Android's initialization
// and backing contract, not an undocumented hardware read or the board fault.
int main (void)
{
  EFI_BOOT_SERVICES Bs={.RaiseTPL=Raise, .RestoreTPL=Restore, .SignalEvent=Signal};
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL H={.Read=Read, .Write=Write, .Map=Map,
    .Unmap=Unmap, .AllocateBuffer=Allocate, .FreeBuffer=Free};
  unsigned Slots[]={1, 16, 32};
  gBS=&Bs; IovaBase=0x10000000; AllowPacked=PoisonSlots=FALSE;
  for(unsigned Case=0; Case<3; Case++) {
    memset(&Owner,0,sizeof(Owner)); memset(Regs,0,sizeof(Regs));
    Owner.UfsHostController=&H; Owner.UfsHcInfo.Capabilities=(Slots[Case]-1)|UFS_HC_CAP_64ADDR;
    InitializeListHead(&Owner.Queue); Regs[UFS_HC_ENABLE_OFFSET/4]=1;
    assert(UfsInitTaskManagementRequestList(&Owner)==EFI_SUCCESS);
    assert(UfsInitTransferRequestList(&Owner)==EFI_SUCCESS);
    UINT8 *First=Dma(ListAddress(),32,FALSE); assert(First);
    UINT64 Base=Le64(First+16);
    assert(Base!=0 && "Every UTRD needs its UCD before the list starts");
    for(unsigned Slot=0; Slot<Slots[Case]; Slot++) {
      UINT8 *T=Dma(ListAddress()+Slot*32,32,FALSE); assert(T);
      assert(Le64(T+16)==Base+Slot*3072);
      assert(Le16(T+24)==128 && Le16(T+26)==128 && Le16(T+28)==0 && Le16(T+30)==256);
      UINT8 *C=Dma(Base+Slot*3072,3072,FALSE); assert(C);
      for(unsigned I=0; I<3072; I++) assert(C[I]==0);
    }
    assert(LiveMaps==3 && Buffers==3 && !Pools && !Requests);
    assert(UfsControllerStop(&Owner)==EFI_SUCCESS);
    for(unsigned I=0; I<16; I++) if(Maps[I].Live) {
      VOID *Host=Maps[I].Host; UINTN Pages=Maps[I].Pages;
      assert(Unmap(&H,&Maps[I])==EFI_SUCCESS && Free(&H,Pages,Host)==EFI_SUCCESS);
    }
    assert(!LiveMaps && !Buffers && !Pools);
  }
  puts("UFS Android persistent-UCD initialization contract: 1/16/32 slots and real zeroed backing PASS; physical fault not modeled");
  return 0;
}
#endif
