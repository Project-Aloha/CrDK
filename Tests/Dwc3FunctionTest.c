/** Independent DWC3 register, USB wire and DMA lifetime fixtures.
    SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Driver/Dwc3CrDxe/Dwc3Function.c"
#include "../Driver/Dwc3CrDxe/Dwc3Core.c"

static UINT32 Registers[0x100000 / 4];
static UINTN Allocations, Mappings, EndCommands, RunWrites, CacheWrites, MmioReads;
static UINT32 EventHead, FailCommand;
static BOOLEAN FailHalt, DropEndEvent;
static EFI_STATUS MapStatus, UnmapStatus;
static UINT64 TimeNs;
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;

VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D,0,N); }
VOID EFIAPI MemoryFence(VOID) {}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
VOID EFIAPI EfiAcquireLock(IN OUT EFI_LOCK *L) { (VOID)L; }
VOID EFIAPI EfiReleaseLock(IN OUT EFI_LOCK *L) { (VOID)L; }
VOID *EFIAPI WriteBackDataCacheRange(VOID *A, UINTN N) { assert(A && N); ++CacheWrites; return A; }
VOID *EFIAPI InvalidateDataCacheRange(VOID *A, UINTN N) { assert(A && N); return A; }
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *A, UINTN N) { assert(A && N); ++CacheWrites; return A; }
UINTN EFIAPI MicroSecondDelay(UINTN N) { TimeNs += N * 1000; return N; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return TimeNs; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 T) { return T; }

static VOID Event(UINT32 Value)
{
  UINT64 Address = Registers[0xC400 / 4] | ((UINT64)Registers[0xC404 / 4] << 32);
  assert(Address && Registers[0xC40C / 4] < 4096);
  ((UINT32 *)(UINTN)Address)[EventHead / 4] = Value;
  EventHead = (EventHead + 4) % 4096;
  Registers[0xC40C / 4] += 4;
}
UINT32 EFIAPI MmioRead32(UINTN Address)
{
  ++MmioReads;
  assert(Address >= 0xA600000 && Address < 0xA700000 && (Address & 3) == 0);
  UINT32 Offset = (UINT32)Address - 0xA600000;
  if (Offset == 0xC70C) return (Registers[Offset / 4] & 7) |
      ((FailHalt || (Registers[0xC704 / 4] & BIT31)) ? 0 : BIT22);
  return Registers[Offset / 4];
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  assert(Address >= 0xA600000 && Address < 0xA700000 && (Address & 3) == 0);
  UINT32 Offset = (UINT32)Address - 0xA600000;
  if (Offset == 0xC40C) { assert(Value <= Registers[Offset / 4]); Registers[Offset / 4] -= Value; return Value; }
  if (Offset == 0xC704) {
    if ((Value ^ Registers[Offset / 4]) & BIT31) ++RunWrites;
    Value &= ~(UINT32)BIT30;
  }
  if (Offset >= 0xC80C && Offset <= 0xC9FC && (Offset & 15) == 12) {
    UINT8 Ep = (UINT8)((Offset - 0xC80C) / 16);
    UINT32 Cmd = Value & 15;
    Value &= ~(UINT32)BIT10;
    if (Cmd == FailCommand) Value |= BIT12;
    else if (Cmd == 6) Value |= Ep >= 2 ? 4U << 16 : 0; /* EP0 resource zero is legal */
    else if (Cmd == 8) {
      ++EndCommands;
      assert((Value & (BIT8 | BIT11)) == (BIT8 | BIT11));
      if (!DropEndEvent) Event((8U << 24) | (7U << 6) | ((UINT32)Ep << 1));
    }
  }
  Registers[Offset / 4] = Value;
  return Value;
}
UINT32 EFIAPI MmioOr32(UINTN A, UINT32 V) { return MmioWrite32(A, MmioRead32(A) | V); }
UINT32 EFIAPI MmioAnd32(UINTN A, UINT32 V) { return MmioWrite32(A, MmioRead32(A) & V); }
UINT32 EFIAPI MmioAndThenOr32(UINTN A, UINT32 M, UINT32 V) { return MmioWrite32(A, (MmioRead32(A) & M) | V); }

static EFI_STATUS EFIAPI Allocate(EDKII_IOMMU_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
  EFI_MEMORY_TYPE Memory, UINTN Pages, VOID **Cpu, UINT64 Attributes)
{
  assert(This == mIommu && Type == AllocateAnyPages && Memory == EfiBootServicesData && Attributes == 0);
  *Cpu = aligned_alloc(4096, Pages * 4096); assert(*Cpu); ++Allocations; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free(EDKII_IOMMU_PROTOCOL *This, UINTN Pages, VOID *Cpu)
{ assert(This == mIommu && Pages && Cpu && Allocations); free(Cpu); --Allocations; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Map(EDKII_IOMMU_PROTOCOL *This, EDKII_IOMMU_OPERATION Op,
  VOID *Cpu, UINTN *Bytes, EFI_PHYSICAL_ADDRESS *Address, VOID **Token)
{
  assert(This == mIommu && Op == EdkiiIoMmuOperationBusMasterCommonBuffer64 && *Bytes % 4096 == 0);
  if (EFI_ERROR(MapStatus)) return MapStatus;
  *Address = (UINTN)Cpu; *Token = Cpu; ++Mappings; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap(EDKII_IOMMU_PROTOCOL *This, VOID *Token)
{
  assert(This == mIommu && Token && Mappings);
  if (!EFI_ERROR(UnmapStatus)) --Mappings;
  return UnmapStatus;
}
static EFI_STATUS EFIAPI Attribute(EDKII_IOMMU_PROTOCOL *This, UINTN Base, UINT32 Sid, VOID *Token, UINT64 Access)
{
  assert(This == mIommu && Base == 0x15000000 && Sid == 0 && Token && Access == 3);
  return EFI_SUCCESS;
}
static EDKII_IOMMU_PROTOCOL Iommu = {.Revision=EDKII_IOMMU_PROTOCOL_REVISION,
  .Map=Map, .Unmap=Unmap, .AllocateBuffer=Allocate, .FreeBuffer=Free, .SetAttributeById=Attribute};

static EFI_USB_DEVICE_DESCRIPTOR Device = {.NumConfigurations=1, .IdVendor=0x1234, .IdProduct=0x5678};
static EFI_USB_CONFIG_DESCRIPTOR Config = {.NumInterfaces=1, .ConfigurationValue=1};
static EFI_USB_INTERFACE_DESCRIPTOR Interface = {.NumEndpoints=2};
static EFI_USB_ENDPOINT_DESCRIPTOR Rx = {.EndpointAddress=1, .Attributes=2};
static EFI_USB_ENDPOINT_DESCRIPTOR Tx = {.EndpointAddress=0x81, .Attributes=2};
static EFI_USB_ENDPOINT_DESCRIPTOR *Endpoints[] = {&Rx,&Tx};
static EFI_USB_INTERFACE_INFO InterfaceInfo = {&Interface,Endpoints};
static EFI_USB_INTERFACE_INFO *Interfaces[] = {&InterfaceInfo};
static EFI_USB_CONFIG_INFO ConfigInfo = {&Config,Interfaces};
static EFI_USB_CONFIG_INFO *Configs[] = {&ConfigInfo};
static EFI_USB_DEVICE_INFO Info = {&Device,Configs};

static VOID Fixture(VOID)
{
  assert(Allocations == 0 && Mappings == 0);
  ZeroMem(Registers,sizeof(Registers)); ZeroMem(mEndpoints,sizeof(mEndpoints));
  ZeroMem(mMessages,sizeof(mMessages)); mMessageHead=mMessageCount=0;
  EventHead=EndCommands=RunWrites=CacheWrites=TimeNs=FailCommand=MmioReads=0;
  FailHalt=DropEndEvent=FALSE; MapStatus=UnmapStatus=EFI_SUCCESS;
  Registers[0xC120 / 4]=0x33310000; Registers[0xC140 / 4]=2;
  Registers[0xC144 / 4]=1U<<15; Registers[0xC14C / 4]=(8U<<12)|(4U<<18);
  Registers[0]=0x40; Registers[0x44 / 4]=1;
  CrUsbFnInit(&Iommu);
}
static VOID Drain(VOID)
{
  EFI_USBFN_MESSAGE Msg;
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  do {
    UINTN Size=sizeof(Payload);
    assert(gCrUsbFunctionIo.EventHandler(&gCrUsbFunctionIo,&Msg,&Size,&Payload)==EFI_SUCCESS);
  } while(Msg != EfiUsbMsgNone);
}
static VOID Finish(UINT8 Ep, UINT32 Residual)
{
  USB_TRB *T=Trbs(Ep);
  T[0].Control &= ~(UINT32)BIT0; T[0].Size=Residual;
  T[1].Control &= ~(UINT32)BIT0;
  Event(((UINT32)Ep<<1)|(1U<<6));
  assert(CrUsbFnPoll()==EFI_SUCCESS);
}
static VOID Request(UINT8 Type, UINT8 Code, UINT16 Value, UINT16 Length)
{
  EFI_USB_DEVICE_REQUEST R={Type,Code,Value,0,Length};
  assert(mPhase==0 && mEndpoints[0].Active);
  memcpy(mDescriptors.Cpu,&R,8); Finish(0,0);
}
static VOID Status(BOOLEAN Internal)
{
  UINTN Size=0;
  UINT8 Ep=mStatusDirection;
  if (!Internal) assert(gCrUsbFunctionIo.Transfer(&gCrUsbFunctionIo,0,Ep,&Size,NULL)==EFI_SUCCESS);
  Event(((UINT32)Ep<<1)|(3U<<6)|(2U<<12));
  assert(CrUsbFnPoll()==EFI_SUCCESS && mEndpoints[Ep].Active);
  Finish(Ep,0); assert(mPhase==0 && mEndpoints[0].Active);
}
static VOID DeviceStart(VOID)
{
  assert(CrUsbCoreInit(UsbTypeCDevice,FALSE)==EFI_SUCCESS);
  CrUsbFnSetAvailable(TRUE);
  assert(gCrUsbFunctionIo.StartController(&gCrUsbFunctionIo)==EFI_SUCCESS);
  assert(gCrUsbFunctionIo.ConfigureEnableEndpoints(&gCrUsbFunctionIo,&Info)==EFI_SUCCESS);
  assert((Registers[0xF8808 / 4]&9)==9 && (Registers[0xC704 / 4]&BIT31));
  assert(Registers[0xC720 / 4]==3);
  Event(1U|(2U<<8)); assert(CrUsbFnPoll()==EFI_SUCCESS);
  Request(0,5,7,0); assert((Registers[0xC700 / 4]&(127U<<3))==(7U<<3)); Status(TRUE);
  Request(0,9,1,0); assert(Registers[0xC720 / 4]==15);
  assert((Registers[0xC80C / 4]&(127U<<16))==(2U<<16)); Status(FALSE); Drain();
}
int main(void)
{
  UINT8 Data[1024]={1,2,3}; UINTN Size;
  EFI_USBFN_MESSAGE Message; EFI_USBFN_MESSAGE_PAYLOAD Payload;
  Fixture(); assert(CrUsbCoreInit(UsbTypeCHost,TRUE)==EFI_SUCCESS);
  assert((Registers[0xC110 / 4]&(3U<<12))==(1U<<12) && (Registers[0xF8808 / 4]&9)==0);
  Fixture(); CrUsbFnSetAvailable(FALSE);
  UINTN Before=MmioReads;
  assert(Configure(&gCrUsbFunctionIo,&Info)==EFI_NOT_READY && MmioReads==Before);
  CrUsbFnSetAvailable(TRUE);
  assert(Configure(&gCrUsbFunctionIo,&Info)==EFI_NOT_READY && MmioReads==Before);
  Fixture(); MapStatus=EFI_DEVICE_ERROR; CrUsbFnSetAvailable(TRUE);
  assert(StartController(&gCrUsbFunctionIo)==EFI_DEVICE_ERROR && Allocations==0 && Mappings==0);
  Fixture(); DeviceStart();
  assert((UINTN)Trbs(3)-(UINTN)Trbs(2)==64);
  Size=8; assert(Transfer(&gCrUsbFunctionIo,1,0,&Size,Data)==EFI_SUCCESS);
  memset(mEndpoints[2].Data.Cpu,0x5A,8); Finish(2,504);
  Size=0; assert(EventHandler(&gCrUsbFunctionIo,&Message,&Size,NULL)==EFI_BUFFER_TOO_SMALL && Size==sizeof(Payload.utr));
  Size=sizeof(Payload); assert(EventHandler(&gCrUsbFunctionIo,&Message,&Size,&Payload)==EFI_SUCCESS);
  assert(Payload.utr.BytesTransferred==8 && Payload.utr.TransferStatus==UsbTransferStatusComplete && Data[7]==0x5A);
  mEndpoints[3].Zlp=TRUE; Size=512; assert(Transfer(&gCrUsbFunctionIo,1,1,&Size,Data)==EFI_SUCCESS);
  assert((Trbs(3)[0].Control&4) && (Trbs(3)[1].Control&2)); Finish(3,0); Drain();
  Size=32; assert(Transfer(&gCrUsbFunctionIo,1,1,&Size,Data)==EFI_SUCCESS);
  assert(Abort(&gCrUsbFunctionIo,1,1)==EFI_SUCCESS && EndCommands==1 && !mEndpoints[3].Active); Drain();
  Size=32; assert(Transfer(&gCrUsbFunctionIo,1,0,&Size,Data)==EFI_SUCCESS);
  Before=RunWrites; Event(1U|(1U<<8));
  assert(CrUsbFnPoll()==EFI_SUCCESS && mEndpoints[2].Ending && RunWrites==Before);
  assert(CrUsbFnPoll()==EFI_SUCCESS && !mEndpoints[2].Active && mEndpoints[0].Active && RunWrites==Before);
  Event(1U|(2U<<8)); assert(CrUsbFnPoll()==EFI_SUCCESS); Drain();
  Request(0x80,6,0x100,18); Drain(); Size=18;
  assert(Transfer(&gCrUsbFunctionIo,0,1,&Size,Data)==EFI_SUCCESS); Finish(1,0); Drain(); Status(FALSE); Drain();
  Request(0x80,6,0x100,18); Drain(); Size=18;
  assert(Transfer(&gCrUsbFunctionIo,0,1,&Size,Data)==EFI_SUCCESS);
  Before=RunWrites;
  Trbs(1)->Control &= ~(UINT32)BIT0; Trbs(1)->Size=(2U<<28)|18;
  Event((1U<<1)|(1U<<6)); assert(CrUsbFnPoll()==EFI_SUCCESS);
  Size=sizeof(Payload);
  assert(EventHandler(&gCrUsbFunctionIo,&Message,&Size,&Payload)==EFI_SUCCESS &&
         Payload.utr.TransferStatus==UsbTransferStatusAborted && Payload.utr.BytesTransferred==0);
  Event((3U<<6)|(2U<<12));
  assert(CrUsbFnPoll()==EFI_SUCCESS && mPhase==0 && mEndpoints[0].Active && RunWrites==Before);
  Request(0x40,0xFE,0,32); Drain(); Size=32;
  assert(Transfer(&gCrUsbFunctionIo,0,0,&Size,Data)==EFI_SUCCESS);
  Trbs(0)->Control &= ~(UINT32)BIT0; Trbs(0)->Size=(2U<<28)|32;
  Event(1U<<6); assert(CrUsbFnPoll()==EFI_SUCCESS);
  Size=0; assert(Transfer(&gCrUsbFunctionIo,0,1,&Size,NULL)==EFI_SUCCESS);
  assert(mEndpoints[1].Pending && !mEndpoints[1].Active);
  Event((1U<<1)|(3U<<6)|(2U<<12));
  assert(CrUsbFnPoll()==EFI_SUCCESS && mPhase==0 && mEndpoints[0].Active && RunWrites==Before);
  assert(mMessageCount==2);
  for (UINTN I=0;I<2;++I) {
    Size=sizeof(Payload);
    assert(EventHandler(&gCrUsbFunctionIo,&Message,&Size,&Payload)==EFI_SUCCESS &&
           Payload.utr.TransferStatus==UsbTransferStatusAborted && Payload.utr.BytesTransferred==0);
  }
  Request(0x40,0xFF,0,0); Drain(); BOOLEAN Stall=TRUE;
  assert(SetStall(&gCrUsbFunctionIo,0,1,&Stall)==EFI_SUCCESS && mPhase==0 && mEndpoints[0].Active);
  Request(0,9,1,0); Status(FALSE); Drain();
  Size=16; assert(Transfer(&gCrUsbFunctionIo,1,0,&Size,Data)==EFI_SUCCESS);
  FailHalt=TRUE; Before=Allocations;
  assert(CrUsbFnStop()==EFI_TIMEOUT && Allocations==Before && Mappings==Before);
  FailHalt=FALSE; UnmapStatus=EFI_DEVICE_ERROR;
  assert(CrUsbFnStop()==EFI_DEVICE_ERROR && Allocations==Before && !mDmaArmed);
  UnmapStatus=EFI_SUCCESS; assert(CrUsbFnStop()==EFI_SUCCESS && Allocations==0 && Mappings==0);
  Fixture(); DeviceStart(); Size=16; assert(Transfer(&gCrUsbFunctionIo,1,1,&Size,Data)==EFI_SUCCESS);
  DropEndEvent=TRUE; assert(Abort(&gCrUsbFunctionIo,1,1)==EFI_TIMEOUT && mEndpoints[3].Active);
  assert(CrUsbFnStop()==EFI_SUCCESS);
  Fixture(); DeviceStart(); Registers[0xC40C / 4]=4097;
  assert(CrUsbFnPoll()==EFI_PROTOCOL_ERROR); Registers[0xC40C / 4]=0; assert(CrUsbFnStop()==EFI_SUCCESS);
  Fixture(); DeviceStart(); MapStatus=EFI_DEVICE_ERROR; Size=16;
  assert(Transfer(&gCrUsbFunctionIo,1,1,&Size,Data)==EFI_DEVICE_ERROR);
  assert(mEndpoints[3].Data.Cpu!=NULL && mEndpoints[3].Data.Mapping==NULL && !mEndpoints[3].Active);
  assert(Transfer(&gCrUsbFunctionIo,1,1,&Size,Data)==EFI_DEVICE_ERROR);
  assert(CrUsbFnPoll()==EFI_DEVICE_ERROR && CrUsbFnStop()==EFI_SUCCESS);
  Fixture();
  EFI_USB_INTERFACE_DESCRIPTOR SecondInterface={.InterfaceNumber=1,.NumEndpoints=2};
  EFI_USB_ENDPOINT_DESCRIPTOR SecondRx={.EndpointAddress=2,.Attributes=2};
  EFI_USB_ENDPOINT_DESCRIPTOR SecondTx={.EndpointAddress=0x82,.Attributes=2};
  EFI_USB_ENDPOINT_DESCRIPTOR *SecondEndpoints[]={&SecondRx,&SecondTx};
  EFI_USB_INTERFACE_INFO SecondInfo={&SecondInterface,SecondEndpoints};
  EFI_USB_INTERFACE_INFO *CompositeInterfaces[]={&InterfaceInfo,&SecondInfo};
  EFI_USB_CONFIG_DESCRIPTOR CompositeConfig={.NumInterfaces=2,.ConfigurationValue=1};
  EFI_USB_CONFIG_INFO Composite={&CompositeConfig,CompositeInterfaces};
  EFI_USB_CONFIG_INFO *CompositeConfigs[]={&Composite};
  EFI_USB_DEVICE_INFO CompositeInfo={&Device,CompositeConfigs};
  assert(CrUsbCoreInit(UsbTypeCDevice,FALSE)==EFI_SUCCESS);
  CrUsbFnSetAvailable(TRUE); assert(StartController(&gCrUsbFunctionIo)==EFI_SUCCESS);
  SecondTx.EndpointAddress=0x81;
  assert(Configure(&gCrUsbFunctionIo,&CompositeInfo)==EFI_INVALID_PARAMETER && !mConfigured);
  SecondTx.EndpointAddress=0x82; SecondInterface.InterfaceNumber=0;
  assert(Configure(&gCrUsbFunctionIo,&CompositeInfo)==EFI_INVALID_PARAMETER && !mConfigured);
  SecondInterface.InterfaceNumber=1;
  Registers[0xC14C / 4]=(4U<<12)|(2U<<18);
  assert(Configure(&gCrUsbFunctionIo,&CompositeInfo)==EFI_UNSUPPORTED && !mConfigured);
  assert(!mEndpoints[0].Enabled && Registers[0xC720 / 4]==0);
  Registers[0xC14C / 4]=(8U<<12)|(4U<<18);
  assert(Configure(&gCrUsbFunctionIo,&CompositeInfo)==EFI_SUCCESS);
  Event(1U|(2U<<8)); assert(CrUsbFnPoll()==EFI_SUCCESS);
  Request(0,9,1,0); Status(FALSE); Drain();
  assert(Registers[0xC720 / 4]==63);
  Size=16; assert(Transfer(&gCrUsbFunctionIo,2,1,&Size,Data)==EFI_SUCCESS); Finish(5,0); Drain();
  assert(CrUsbFnStop()==EFI_SUCCESS);
  Fixture(); DeviceStart();
  UINT32 ActiveCalls=99;
  UINT32 Saved[sizeof(Registers)/sizeof(Registers[0])];
  memcpy(Saved,Registers,sizeof(Saved));
  Before=Allocations; UINTN BeforeMaps=Mappings, BeforeReads=MmioReads;
  assert(CrUsbFnFreeze(NULL)==EFI_INVALID_PARAMETER);
  assert(CrUsbFnFreeze(&ActiveCalls)==EFI_SUCCESS && ActiveCalls==0);
  assert(CrUsbFnFreeze(&ActiveCalls)==EFI_SUCCESS && ActiveCalls==0);
  CrUsbFnSetAvailable(TRUE); assert(!mAvailable);
  assert(CrUsbFnPoll()==EFI_NOT_READY && CrUsbFnStop()==EFI_NOT_READY);
  assert(StartController(&gCrUsbFunctionIo)==EFI_NOT_READY);
  assert(Configure(&gCrUsbFunctionIo,&Info)==EFI_NOT_READY);
  Size=16; assert(Transfer(&gCrUsbFunctionIo,1,1,&Size,Data)==EFI_NOT_READY);
  assert(Abort(&gCrUsbFunctionIo,1,1)==EFI_NOT_READY);
  assert(SetStall(&gCrUsbFunctionIo,1,1,&Stall)==EFI_NOT_READY);
  assert(SetPolicy(&gCrUsbFunctionIo,1,1,EfiUsbPolicyZeroLengthTermination,sizeof(Stall),&Stall)==EFI_NOT_READY);
  EFI_USBFN_MESSAGE FrozenMsg; EFI_USBFN_MESSAGE_PAYLOAD FrozenPayload;
  Size=sizeof(FrozenPayload); assert(EventHandler(&gCrUsbFunctionIo,&FrozenMsg,&Size,&FrozenPayload)==EFI_NOT_READY);
  VOID *Client=NULL; assert(AllocateTransfer(&gCrUsbFunctionIo,32,&Client)==EFI_NOT_READY && Client==NULL);
  assert(FreeTransfer(&gCrUsbFunctionIo,Data)==EFI_NOT_READY);
  CR_USB_DMA Dma={0}; assert(CrUsbDmaAllocate(&Dma,64)==EFI_NOT_READY && CrUsbDmaFree(&Dma)==EFI_NOT_READY);
  assert(!CrUsbFnCanUnload());
  assert(Allocations==Before && Mappings==BeforeMaps && MmioReads==BeforeReads);
  assert(memcmp(Saved,Registers,sizeof(Saved))==0);
  // Test teardown only: production has no reopen entry point.
  mFrozen=FALSE; assert(CrUsbFnStop()==EFI_SUCCESS);
  puts("DWC3: core roles, EP0, bulk/ZLP, reset, event layout and fenced DMA lifetime passed");
  return 0;
}
