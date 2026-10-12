/* Real xHCI functions and layouts against an independent command/state model.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Xhci.h"

STATIC USB_XHCI_INSTANCE Hc;
STATIC DEVICE_CONTEXT C32;
STATIC DEVICE_CONTEXT_64 C64;
STATIC INPUT_CONTEXT I32;
STATIC INPUT_CONTEXT_64 I64;
STATIC EFI_PCI_IO_PROTOCOL Pci;
STATIC EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
STATIC URB Transfer;
STATIC TRANSFER_RING Ring;
STATIC TRB_TEMPLATE Trbs[4];
STATIC EVT_TRB_COMMAND_COMPLETION Completion;
STATIC EVT_TRB_TRANSFER Event;
STATIC UINT8 Buffer[8];
STATIC UINT64 Dcbaa[256];
STATIC UINTN DisableCommands, FailDisableSlot, RingFrees, Initializations;
STATIC UINTN Commands, Stops, Resets, Dequeues, Bells, Frees, BufferFrees, Polls, Allocations;
STATIC UINTN Delays, Tpl, RestartAfter;
STATIC BOOLEAN Halted, RaceHalt, StuckStopped, FailReset, FailDequeue, NoEvent, HaveEvent;
STATIC BOOLEAN FailHalt, FailUninstall;
STATIC VOID *PoolTracked[8];
STATIC UINTN PoolOutstanding;
STATIC UINTN ScheduleFrees, Uninstalls, Closes, TimerStops;
STATIC UINTN SoftResets, Callbacks, PortReads;
STATIC UINT32 CallbackResult, PortState;
STATIC BOOLEAN CancelInCallback, CommitResetFailure, WrongResetCompletion, FailBell, DisconnectAfterReset;
STATIC UINT8 ControllerToggle;
STATIC UINTN AutoEvents;
STATIC UINTN ChangeIdentity;
STATIC UINT64 Tick;
STATIC VOID TransferEvent(UINT8 Code);
EFI_GUID gEfiUsb2HcProtocolGuid, gEfiPciIoProtocolGuid, gEdkiiUsb2HcFreezeProtocolGuid;

VOID *EFIAPI ZeroMem(VOID *P, UINTN N) { return memset(P, 0, N); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
VOID EFIAPI MemoryFence(VOID) {}
UINTN EFIAPI MicroSecondDelay(UINTN Us) { Delays++; return Us; }
UINT64 EFIAPI LShiftU64(UINT64 V, UINTN N) { return V << N; }
UINT64 EFIAPI RShiftU64(UINT64 V, UINTN N) { return V >> N; }
UINT64 EFIAPI MultU64x32(UINT64 V, UINT32 N) { return V*N; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { Tick+=10000; return Tick; }
UINT64 XhcConvertTimeToTicks(UINT64 Nanoseconds) { return Nanoseconds; }
UINT64 XhcGetElapsedTicks(UINT64 *Previous) { UINT64 Now=GetPerformanceCounter(), Delta=Now-*Previous; *Previous=Now; return Delta; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) {}
VOID *EFIAPI AllocateZeroPool(UINTN N) { return calloc(1, N); }
VOID EFIAPI FreePool(VOID *P) {
  for (UINTN I=0; I<ARRAY_SIZE(PoolTracked); ++I) if(PoolTracked[I]==P && P!=NULL) {
    PoolTracked[I]=NULL; assert(PoolOutstanding); --PoolOutstanding; break;
  }
  if (P != &Hc && P != &Ring) free(P);
}
STATIC VOID *TrackPool(UINTN Size) {
  for (UINTN I=0; I<ARRAY_SIZE(PoolTracked); ++I) if(PoolTracked[I]==NULL) {
    PoolTracked[I]=calloc(1,Size); assert(PoolTracked[I]); ++PoolOutstanding; return PoolTracked[I];
  }
  assert(0); return NULL;
}
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *P) { P->ForwardLink = P->BackLink = P; return P; }
LIST_ENTRY *EFIAPI InsertHeadList(LIST_ENTRY *H, LIST_ENTRY *P) {
  P->ForwardLink = H->ForwardLink; P->BackLink = H;
  H->ForwardLink->BackLink = P; H->ForwardLink = P; return H;
}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *P) {
  P->BackLink->ForwardLink = P->ForwardLink; P->ForwardLink->BackLink = P->BackLink;
  return P->ForwardLink;
}
BOOLEAN EFIAPI IsListEmpty(CONST LIST_ENTRY *P) { return P->ForwardLink == P; }
STATIC EFI_TPL EFIAPI Raise(EFI_TPL N) { UINTN Old = Tpl; assert(N >= Old); Tpl=N; return Old; }
STATIC VOID EFIAPI Restore(EFI_TPL N) { assert(N <= Tpl); Tpl=N; }
STATIC EFI_STATUS EFIAPI ReleasePool(VOID *P) { free(P); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Stall(UINTN Us) { return EFI_SUCCESS; }
STATIC UINT8 State(VOID) { return Hc.HcCParams.Data.Csz ? C64.EP[2].EPState : C32.EP[2].EPState; }
STATIC VOID SetState(UINT8 V) { C32.EP[2].EPState=V; C64.EP[2].EPState=V; }
BOOLEAN XhcIsHalt(USB_XHCI_INSTANCE *X) { assert(X == &Hc); return Halted; }
BOOLEAN XhcIsSysError(USB_XHCI_INSTANCE *X) { return FALSE; }
EFI_PHYSICAL_ADDRESS UsbHcGetPciAddrForHostAddr(USBHC_MEM_POOL *P, VOID *M, UINTN N, BOOLEAN A) { return (UINTN)M; }
EFI_PHYSICAL_ADDRESS UsbHcGetHostAddrForPciAddr(USBHC_MEM_POOL *P, VOID *M, UINTN N, BOOLEAN A) { return (UINTN)M; }
UINT32 XhcReadRuntimeReg(USB_XHCI_INSTANCE *X, UINT32 O) { return 0; }
UINT32 XhcReadOpReg(USB_XHCI_INSTANCE *X, UINT32 O) {
  if (O == XHC_USBSTS_OFFSET) return 0;
  assert(X == &Hc && O == XHC_PORTSC_OFFSET); PortReads++; return PortState;
}
VOID XhcWriteRuntimeReg(USB_XHCI_INSTANCE *X, UINT32 O, UINT32 V) {}
EFI_STATUS EFIAPI XhcSyncEventRing(USB_XHCI_INSTANCE *X, EVENT_RING *R) { return EFI_SUCCESS; }
EFI_STATUS XhcCheckNewEvent(USB_XHCI_INSTANCE *X, EVENT_RING *R, TRB_TEMPLATE **T) {
  if (!HaveEvent) return EFI_NOT_READY;
  if (Event.Type==TRB_TYPE_TRANS_EVENT && Event.Completecode==TRB_COMPLETION_USB_TRANSACTION_ERROR) SetState(2);
  HaveEvent=FALSE; *T=(TRB_TEMPLATE *)&Event; return EFI_SUCCESS;
}
EFI_STATUS EFIAPI XhcRingDoorBell(USB_XHCI_INSTANCE *X, UINT8 Slot, UINT8 Dci) {
  assert(X == &Hc && Slot == 1 && Dci == 3); Bells++; SetState(1);
  if (AutoEvents==1) TransferEvent(Bells==1 ? TRB_COMPLETION_USB_TRANSACTION_ERROR : TRB_COMPLETION_SUCCESS);
  if (AutoEvents==2 || (AutoEvents==3 && Bells==1)) TransferEvent(TRB_COMPLETION_USB_TRANSACTION_ERROR);
  return FailBell ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
EFI_STATUS EFIAPI XhcCmdTransfer(USB_XHCI_INSTANCE *X, TRB_TEMPLATE *T, UINTN Timeout, TRB_TEMPLATE **E) {
  assert(X == &Hc); Commands++;
  if (Halted) return EFI_DEVICE_ERROR;
  Completion.Completecode=TRB_COMPLETION_SUCCESS; *E=(TRB_TEMPLATE *)&Completion;
  if (NoEvent) { *E=NULL; return EFI_SUCCESS; }
  if (T->Type == TRB_TYPE_DIS_SLOT) {
    CMD_TRB_DISABLE_SLOT *D=(CMD_TRB_DISABLE_SLOT *)T;
    DisableCommands++;
    return D->SlotId==FailDisableSlot ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  }
  if (T->Type == TRB_TYPE_STOP_ENDPOINT) {
    Stops++;
    if (RestartAfter != 0 && Stops == RestartAfter) SetState(1);
    if (RaceHalt) { RaceHalt=FALSE; SetState(2); Transfer.Finished=TRUE; Transfer.Result=EDKII_USB_ERR_TRANSACTION; }
    if (StuckStopped || State() != 1) {
      Completion.Completecode=TRB_COMPLETION_CONTEXT_STATE_ERROR; return EFI_DEVICE_ERROR;
    }
    SetState(3); return EFI_SUCCESS;
  }
  if (T->Type == TRB_TYPE_RESET_ENDPOINT) {
    Resets++; assert(State() == 2);
    CMD_TRB_RESET_ENDPOINT *Reset=(CMD_TRB_RESET_ENDPOINT *)T;
    if (Reset->TSP) SoftResets++; else ControllerToggle=0;
    if (CommitResetFailure) { SetState(3); return EFI_TIMEOUT; }
    if (FailReset) return EFI_DEVICE_ERROR;
    if (WrongResetCompletion) { Completion.Completecode=TRB_COMPLETION_CONTEXT_STATE_ERROR; return EFI_SUCCESS; }
    if (DisconnectAfterReset) PortState=0;
    if (ChangeIdentity==1) Hc.UsbDevContext[1].BusDevAddr=8;
    if (ChangeIdentity==2) Hc.UsbDevContext[1].EndpointTransferRing[2]=NULL;
    if (ChangeIdentity==3) Hc.UsbDevContext[1].OutputContext=NULL;
    if (ChangeIdentity==4) Transfer.Cancelled=TRUE;
    SetState(3); return EFI_SUCCESS;
  }
  assert(T->Type == TRB_TYPE_SET_TR_DEQUE); Dequeues++; assert(State() == 3);
  return FailDequeue ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
VOID XhcFreeUrb(USB_XHCI_INSTANCE *X, URB *U) { assert(U == &Transfer); Frees++; }
STATIC EFI_STATUS EFIAPI FreeBuffer(EFI_PCI_IO_PROTOCOL *P, UINTN Pages, VOID *Data) {
  assert(P == &Pci && Data == Buffer && Pages == 1); BufferFrees++; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AllocateBuffer(EFI_PCI_IO_PROTOCOL *P, EFI_ALLOCATE_TYPE T,
 EFI_MEMORY_TYPE M, UINTN Pages, VOID **Data, UINT64 Attributes) {
  Allocations++; *Data=Buffer; return EFI_SUCCESS;
}
URB *XhcCreateUrb(USB_XHCI_INSTANCE *X, UINT8 A, UINT8 E, UINT8 S, UINTN P, UINTN T,
 EFI_USB_DEVICE_REQUEST *R, VOID *D, UINTN N, EFI_ASYNC_USB_TRANSFER_CALLBACK C, VOID *Ctx) {
  assert(0); return NULL;
}
EFI_STATUS XhcFlushAsyncIntMap(USB_XHCI_INSTANCE *X, URB *U) { Polls++; return EFI_SUCCESS; }
VOID XhcUpdateAsyncRequest(USB_XHCI_INSTANCE *X, URB *U) { Polls++; }
BOOLEAN XhcCheckUrbResult(USB_XHCI_INSTANCE *X, URB *U);
EFI_STATUS XhcHaltHC(USB_XHCI_INSTANCE *X, UINT32 Timeout) {
  if (FailHalt) return EFI_TIMEOUT;
  Halted=TRUE; return EFI_SUCCESS;
}
VOID XhcClearBiosOwnership(USB_XHCI_INSTANCE *X) { assert(Halted); }
VOID XhcFreeSched(USB_XHCI_INSTANCE *X) { assert(Halted); ScheduleFrees++; }
VOID UsbHcFreeMem(USBHC_MEM_POOL *P, VOID *M, UINTN N) {
  if(M==&C32) assert(N==sizeof(C32));
  if(M==&C64) assert(N==sizeof(C64));
  if(M==&I32) assert(N==sizeof(I32));
  if(M==&I64) assert(N==sizeof(I64));
  RingFrees++;
}
EFI_STATUS EFIAPI XhcInitializeDeviceSlot(USB_XHCI_INSTANCE *X, USB_DEV_ROUTE P,
 UINT16 Port, USB_DEV_ROUTE R, UINT8 Speed) { Initializations++; return EFI_SUCCESS; }
EFI_STATUS EFIAPI XhcInitializeDeviceSlot64(USB_XHCI_INSTANCE *X, USB_DEV_ROUTE P,
 UINT16 Port, USB_DEV_ROUTE R, UINT8 Speed) { Initializations++; return EFI_SUCCESS; }
EFI_STATUS RingIntTransferDoorBell(USB_XHCI_INSTANCE *X, URB *U) { Bells++; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Flush(EFI_PCI_IO_PROTOCOL *P) { return EFI_SUCCESS; }
EFI_STATUS EFIAPI FreeUnicodeStringTable(EFI_UNICODE_STRING_TABLE *T) { return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Open(EFI_HANDLE H, EFI_GUID *G, VOID **P, EFI_HANDLE A, EFI_HANDLE C, UINT32 F) {
  *P=&Hc.Usb2Hc; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H, ...) {
  assert(Halted); Uninstalls++; return FailUninstall ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CloseProtocol(EFI_HANDLE H, EFI_GUID *G, EFI_HANDLE A, EFI_HANDLE C) { return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E) { assert(Halted); Closes++; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI SetTimer(EFI_EVENT E, EFI_TIMER_DELAY T, UINT64 I) {
  assert(Halted); TimerStops++; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION O,
 UINT64 A, UINT64 *R) { assert(Halted); return EFI_SUCCESS; }
#include "XhciEndpointProduction.c"

STATIC EFI_STATUS EFIAPI Callback(VOID *Data, UINTN Length, VOID *Context, UINT32 Result) {
  assert(Context == &Hc); Callbacks++; CallbackResult=Result;
  if (CancelInCallback) assert(XhciDelAsyncIntTransfer(&Hc,7,0x81)==EFI_SUCCESS);
  return EFI_SUCCESS;
}
STATIC VOID Setup(BOOLEAN Wide, UINT8 EndpointState) {
  memset(&Hc,0,sizeof(Hc)); memset(&C32,0,sizeof(C32)); memset(&C64,0,sizeof(C64));
  memset(&Transfer,0,sizeof(Transfer)); memset(&Ring,0,sizeof(Ring)); memset(&Completion,0,sizeof(Completion));
  memset(&Event,0,sizeof(Event)); memset(Trbs,0,sizeof(Trbs));
  DisableCommands=FailDisableSlot=RingFrees=Initializations=0;
  memset(Dcbaa,0,sizeof(Dcbaa)); Hc.DCBAA=Dcbaa;
  Commands=Stops=Resets=Dequeues=Bells=Frees=BufferFrees=Polls=Allocations=Delays=0;
  Halted=RaceHalt=StuckStopped=FailReset=FailDequeue=NoEvent=HaveEvent=FailHalt=FailUninstall=FALSE;
  assert(PoolOutstanding==0);
  ScheduleFrees=Uninstalls=Closes=TimerStops=RestartAfter=0;
  SoftResets=Callbacks=PortReads=CallbackResult=0; ControllerToggle=1;
  AutoEvents=ChangeIdentity=0; Tick=0;
  CancelInCallback=CommitResetFailure=WrongResetCompletion=FailBell=DisconnectAfterReset=FALSE;
  PortState=XHC_PORTSC_CCS|XHC_PORTSC_PED;
  Hc.Signature=XHCI_INSTANCE_SIG;
  Hc.HcCParams.Data.Csz=Wide; Hc.UsbDevContext[1].OutputContext=Wide ? (VOID *)&C64 : (VOID *)&C32;
  Hc.HcSParams1.Data.MaxPorts=1;
  Hc.MaxSlotsEn=2;
  C32.Slot.RootHubPortNum=C64.Slot.RootHubPortNum=1;
  C32.Slot.Speed=C64.Slot.Speed=EFI_USB_SPEED_FULL;
  C32.EP[2].EPType=C64.EP[2].EPType=ED_INTERRUPT_IN;
  Hc.UsbDevContext[1].Enabled=TRUE; Hc.UsbDevContext[1].SlotId=1; Hc.UsbDevContext[1].BusDevAddr=7; Hc.UsbDevContext[1].RouteString.Dword=0x10000;
  Hc.PciIo=&Pci; Pci.Flush=Flush; Pci.FreeBuffer=FreeBuffer; Pci.AllocateBuffer=AllocateBuffer;
  Services.RaiseTPL=Raise; Services.RestoreTPL=Restore; Tpl=TPL_APPLICATION;
  Services.FreePool=ReleasePool;
  Services.Stall=Stall;
  Services.OpenProtocol=Open; Services.UninstallMultipleProtocolInterfaces=Uninstall;
  Services.CloseProtocol=CloseProtocol; Services.CloseEvent=CloseEvent; Services.SetTimer=SetTimer;
  Pci.Attributes=Attributes;
  Hc.PollTimer=(VOID *)(UINTN)1; Hc.ExitBootServiceEvent=(VOID *)(UINTN)2;
  InitializeListHead(&Hc.AsyncIntTransfers); InitializeListHead(&Transfer.UrbList);
  Transfer.Ep.BusAddr=7; Transfer.Ep.EpAddr=1; Transfer.Ep.Direction=EfiUsbDataIn;
  Transfer.Callback=Callback; Transfer.Context=&Hc; Transfer.Data=Buffer; Transfer.DataLen=8;
  Transfer.Ring=&Ring; Transfer.TrbStart=Transfer.TrbEnd=Trbs; Transfer.TrbNum=1;
  Ring.RingSeg0=Trbs; Ring.RingEnqueue=Trbs+1; Ring.RingDequeue=Trbs; Ring.RingPCS=1;
  Hc.UsbDevContext[1].EndpointTransferRing[2]=&Ring;
  SetState(EndpointState);
}
STATIC VOID TransferEvent(UINT8 Code) {
  Event.Type=TRB_TYPE_TRANS_EVENT; Event.Completecode=Code; Event.SlotId=1; Event.EndpointId=3;
  Event.TRBPtrLo=(UINT32)(UINTN)Trbs; Event.TRBPtrHi=(UINT32)((UINTN)Trbs>>32);
  Hc.EventRing.TrbNumber=1; Hc.EventRing.EventRingDequeue=(TRB_TEMPLATE *)&Event;
  Trbs[0].Type=TRB_TYPE_NORMAL; ((TRANSFER_TRB_NORMAL *)Trbs)->Length=8; HaveEvent=TRUE;
}
STATIC VOID TestSoftRetry(BOOLEAN Wide) {
  Setup(Wide,2); CancelInCallback=TRUE;
  InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
  TransferEvent(TRB_COMPLETION_USB_TRANSACTION_ERROR);
  XhcMonitorAsyncRequests(NULL,&Hc);
#ifdef TEST_BEFORE_SOFT_RETRY
  // Actual old event/monitor/cancel path discards the TD and clears only the
  // controller toggle. The device's expected DATA1 cannot be synchronized by it.
  assert(Callbacks==1 && CallbackResult==EDKII_USB_ERR_TRANSACTION);
  assert(Resets==1 && SoftResets==0 && Dequeues==1 && ControllerToggle==0 && Frees==1);
#else
  assert(Callbacks==0 && Polls==0 && Frees==0 && BufferFrees==0);
  assert(SoftResets==1 && Dequeues==0 && Bells==1 && ControllerToggle==1);
  assert(!Transfer.Finished && Transfer.Result==0 && Transfer.SoftRetryCount==1);
  assert(Transfer.Ring==&Ring && Transfer.TrbStart==Trbs && Transfer.TrbEnd==Trbs);
  assert(Ring.RingEnqueue==Trbs+1 && Ring.RingDequeue==Trbs && Ring.RingPCS==1);
  CancelInCallback=FALSE;
  TransferEvent(TRB_COMPLETION_SUCCESS);
  XhcMonitorAsyncRequests(NULL,&Hc);
  assert(Callbacks==1 && CallbackResult==0 && Transfer.Completed==8 && Transfer.SoftRetryCount==0);
  assert(SoftResets==1 && Dequeues==0 && Polls==2 && Frees==0);
  // A multi-TRB TD resumes at the failed TRB. Preserve bytes already completed
  // without adding them again when the retried last TRB finally succeeds.
  Setup(Wide,1); InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
  Transfer.TrbEnd=Trbs+1; Transfer.TrbNum=2; Ring.RingEnqueue=Trbs+2;
  TransferEvent(TRB_COMPLETION_SUCCESS); ((TRANSFER_TRB_NORMAL *)Trbs)->Length=4;
  XhcMonitorAsyncRequests(NULL,&Hc);
  assert(Transfer.Completed==4 && Transfer.StartDone && !Transfer.Finished && Callbacks==0);
  TransferEvent(TRB_COMPLETION_USB_TRANSACTION_ERROR);
  Event.TRBPtrLo=(UINT32)(UINTN)(Trbs+1); Event.TRBPtrHi=(UINT32)((UINTN)(Trbs+1)>>32);
  Trbs[1].Type=TRB_TYPE_NORMAL; ((TRANSFER_TRB_NORMAL *)(Trbs+1))->Length=4;
  XhcMonitorAsyncRequests(NULL,&Hc);
  assert(Transfer.Completed==4 && Transfer.StartDone && !Transfer.Finished && Callbacks==0);
  assert(Ring.RingDequeue==Trbs+1 && Ring.RingEnqueue==Trbs+2 && SoftResets==1);
  TransferEvent(TRB_COMPLETION_SUCCESS);
  Event.TRBPtrLo=(UINT32)(UINTN)(Trbs+1); Event.TRBPtrHi=(UINT32)((UINTN)(Trbs+1)>>32);
  XhcMonitorAsyncRequests(NULL,&Hc);
  assert(Transfer.Completed==8 && Callbacks==1 && CallbackResult==0 && Dequeues==0);
  // Four retries match Linux err_count++ > MAX_SOFT_RETRY (MAX=3).
  Setup(Wide,2);
  for (UINTN Attempt=0; Attempt<4; Attempt++) {
    SetState(2); Transfer.Result=EDKII_USB_ERR_TRANSACTION; Transfer.Finished=TRUE;
    Transfer.Completed=4; Transfer.StartDone=TRUE;
    assert(XhcRetryTransaction(&Hc,&Transfer));
    assert(Transfer.Completed==4 && Transfer.StartDone && !Transfer.EndDone);
  }
  SetState(2); Transfer.Result=EDKII_USB_ERR_TRANSACTION; Transfer.Finished=TRUE;
  assert(!XhcRetryTransaction(&Hc,&Transfer) && SoftResets==4 && Dequeues==0 && Bells==4);
  assert(Transfer.Finished && Transfer.Result==EDKII_USB_ERR_TRANSACTION);
  // TT, control/isochronous endpoints, disconnected/inactive root links and
  // stale/cancelled TDs must never issue the preservation command.
  for (UINTN Case=0; Case<10; Case++) {
    Setup(Wide,2); Transfer.Result=EDKII_USB_ERR_TRANSACTION; Transfer.Finished=TRUE;
    if (Case==0) C32.Slot.TTHubSlotId=C64.Slot.TTHubSlotId=2;
    if (Case==1) C32.EP[2].EPType=C64.EP[2].EPType=ED_CONTROL_BIDIR;
    if (Case==2) C32.EP[2].EPType=C64.EP[2].EPType=ED_ISOCH_IN;
    if (Case==3) PortState=0;
    if (Case==4) PortState=XHC_PORTSC_CCS|(6U<<5);
    if (Case==5) Transfer.Cancelled=TRUE;
    if (Case==6) Transfer.Retired=TRUE;
    if (Case==7) Hc.UsbDevContext[1].EndpointTransferRing[2]=NULL;
    if (Case==8) SetState(1);
    if (Case==9) Halted=TRUE;
    assert(!XhcRetryTransaction(&Hc,&Transfer) && Commands==0 && Bells==0 && Frees==0);
  }
  for (UINTN Case=0; Case<6; Case++) {
    Setup(Wide,2); Transfer.Result=EDKII_USB_ERR_TRANSACTION; Transfer.Finished=TRUE;
    if (Case==0) FailReset=TRUE;
    if (Case==1) NoEvent=TRUE;
    if (Case==2) WrongResetCompletion=TRUE;
    if (Case==3) CommitResetFailure=TRUE;
    if (Case==4) DisconnectAfterReset=TRUE;
    if (Case==5) FailBell=TRUE;
    assert(!XhcRetryTransaction(&Hc,&Transfer));
    assert(Transfer.Finished && Transfer.Result==EDKII_USB_ERR_TRANSACTION);
    assert(Transfer.Ring==&Ring && Frees==0 && Dequeues==0 && SoftResets<=1);
    assert(Bells==(Case==5 ? 1U : 0U));
  }
  for (UINTN Case=1; Case<=4; Case++) {
    Setup(Wide,2); Transfer.Result=EDKII_USB_ERR_TRANSACTION; Transfer.Finished=TRUE;
    ChangeIdentity=Case;
    assert(!XhcRetryTransaction(&Hc,&Transfer));
    assert(SoftResets==1 && Bells==0 && PortReads==1 && Dequeues==0 && Frees==0);
  }
  // A failed preserving reset leaves the exact original TD and DMA owned.
  Setup(Wide,2); FailReset=TRUE; Transfer.Result=EDKII_USB_ERR_TRANSACTION; Transfer.Finished=TRUE;
  InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
  assert(!XhcRetryTransaction(&Hc,&Transfer));
  assert(XhciDelAsyncIntTransfer(&Hc,7,0x81)==EFI_DEVICE_ERROR);
  assert(Transfer.Cancelled && Transfer.Ring==&Ring && Frees==0 && BufferFrees==0);
  FailReset=FALSE;
  assert(XhciDelAsyncIntTransfer(&Hc,7,0x81)==EFI_SUCCESS && Frees==1 && BufferFrees==1);
  // The synchronous polling path retries without extending its deadline or
  // confusing command completion with transfer completion.
  Setup(Wide,1); AutoEvents=1;
  assert(XhcExecTransfer(&Hc,FALSE,&Transfer,1)==EFI_SUCCESS);
  assert(Transfer.Completed==8 && Transfer.Result==0 && SoftResets==1 && Bells==2 && Dequeues==0);
  Setup(Wide,1); AutoEvents=2;
  assert(XhcExecTransfer(&Hc,FALSE,&Transfer,1)==EFI_DEVICE_ERROR);
  assert(Transfer.Result==EDKII_USB_ERR_TRANSACTION && SoftResets==4 && Bells==5 && Dequeues==0);
  Setup(Wide,1); AutoEvents=3;
  assert(XhcExecTransfer(&Hc,FALSE,&Transfer,1)==EFI_TIMEOUT);
  assert(Transfer.Result==EFI_USB_ERR_TIMEOUT && SoftResets==1 && Tick>=1000000 && Tick<1100000);
#endif
}
STATIC EFI_STATUS Disable(UINT8 Slot) {
  return Hc.HcCParams.Data.Csz ? XhcDisableSlotCmd64(&Hc,Slot) : XhcDisableSlotCmd(&Hc,Slot);
}
STATIC VOID TestSlotRetirement(BOOLEAN Wide) {
  URB *Found=NULL;
  UINT8 Toggle=0;
  Setup(Wide,1);
  Hc.UsbDevContext[1].EndpointTransferRing[2]=&Ring;
  Hc.UsbDevContext[1].RouteString.Dword=0x10000;
  Dcbaa[1]=0x12340000;
  InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
  FailDisableSlot=1;
  assert(Disable(1)==EFI_DEVICE_ERROR);
  assert(!Transfer.Retired && !Transfer.Cancelled && Transfer.Ring==&Ring);
  assert(RingFrees==0 && Dcbaa[1]!=0 && Hc.UsbDevContext[1].Enabled);
  FailDisableSlot=0;
  assert(Disable(1)==EFI_SUCCESS);
  assert(Transfer.Retired && Transfer.Cancelled && Transfer.Callback==NULL);
  assert(Transfer.Ring==NULL && Transfer.TrbStart==NULL && Transfer.TrbNum==0);
  assert(Dcbaa[1]==0 && !Hc.UsbDevContext[1].Enabled && RingFrees==2);
  // Port-status retirement happens before class Stop. Reuse both the context
  // and ring address for another device, and deliver an unrelated command event.
  Hc.UsbDevContext[1].Enabled=TRUE; Hc.UsbDevContext[1].SlotId=1;
  Hc.UsbDevContext[1].BusDevAddr=8;
  assert(!IsAsyncIntTrb(&Hc,Trbs,&Found));
  XhcMonitorAsyncRequests(NULL,&Hc); assert(Polls==0 && Bells==0);
  UINTN Before=Commands;
  assert(XhcAsyncInterruptTransfer(&Hc.Usb2Hc,7,0x81,0,8,FALSE,&Toggle,0,0,NULL,NULL,NULL)==EFI_SUCCESS);
  assert(Commands==Before && Frees==1 && BufferFrees==1 && IsListEmpty(&Hc.AsyncIntTransfers));
  assert(XhcAsyncInterruptTransfer(&Hc.Usb2Hc,7,0x81,0,8,TRUE,&Toggle,10,8,NULL,Callback,NULL)==EFI_DEVICE_ERROR);
  // Missing command completion cannot certify retirement.
  Setup(Wide,1); NoEvent=TRUE;
  assert(Disable(1)==EFI_DEVICE_ERROR && !Transfer.Retired && RingFrees==0);
  // A failed child disable must retain its slot and prevent parent reuse.
  Setup(Wide,1);
  Hc.UsbDevContext[1].RouteString.Dword=0x10000;
  Hc.UsbDevContext[2].Enabled=TRUE; Hc.UsbDevContext[2].SlotId=2;
  Hc.UsbDevContext[2].ParentRouteString.Dword=0x10000;
  Hc.UsbDevContext[2].RouteString.Dword=0x10001;
  FailDisableSlot=2;
  assert(Disable(1)==EFI_DEVICE_ERROR && DisableCommands==1);
  assert(Hc.UsbDevContext[2].SlotId==2 && Hc.UsbDevContext[1].Enabled && RingFrees==0);
  // A port reset cannot initialize a replacement when the old slot did not retire.
  Setup(Wide,1);
  USB_DEV_ROUTE Route={0}, Parent={0};
  Route.Route.RootPortNum=1; Route.Route.TierNum=1;
  Hc.UsbDevContext[1].RouteString=Route;
  EFI_USB_PORT_STATUS Port={USB_PORT_STAT_ENABLE|USB_PORT_STAT_CONNECTION,USB_PORT_STAT_C_RESET};
  FailDisableSlot=1;
  assert(XhcPollPortStatusChange(&Hc,Parent,0,&Port)==EFI_DEVICE_ERROR);
  assert(DisableCommands==1 && Initializations==0 && Hc.UsbDevContext[1].Enabled);
}
STATIC VOID TestControllerStop(BOOLEAN Wide) {
  EFI_DRIVER_BINDING_PROTOCOL Binding={0};
  Setup(Wide,1); Halted=TRUE;
  Hc.UsbDevContext[1].InputContext=Wide ? (VOID *)&I64 : (VOID *)&I32;
  Hc.UsbDevContext[3].DevDesc.NumConfigurations=2; // descriptor array not yet allocated
  TRANSFER_RING *HeapRing=TrackPool(sizeof(*HeapRing)); *HeapRing=Ring;
  Transfer.Ring=HeapRing; Hc.UsbDevContext[1].EndpointTransferRing[2]=HeapRing;
  Hc.UsbDevContext[1].DevDesc.NumConfigurations=2;
  Hc.UsbDevContext[1].ConfDesc=TrackPool(2*sizeof(VOID *));
  Hc.UsbDevContext[1].ConfDesc[0]=TrackPool(sizeof(EFI_USB_CONFIG_DESCRIPTOR));
  Hc.UsbDevContext[1].ActiveAlternateSetting=TrackPool(2);
  // Failed early enumeration can own metadata while the slot is not Enabled.
  Hc.UsbDevContext[2].ConfDesc=TrackPool(sizeof(VOID *));
  Hc.UsbDevContext[2].DevDesc.NumConfigurations=1;
  Hc.UsbDevContext[2].ConfDesc[0]=TrackPool(sizeof(EFI_USB_CONFIG_DESCRIPTOR));
  Hc.UsbDevContext[2].ActiveAlternateSetting=TrackPool(1);
  InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
  FailUninstall=TRUE;
  assert(XhcDriverBindingStop(&Binding,NULL,0,NULL)==EFI_DEVICE_ERROR);
  // Hardware is already halted. Neither a command nor premature software
  // retirement is allowed while an installed protocol may still be used.
  assert(Commands==0 && RingFrees==0 && PoolOutstanding==7);
  assert(!Transfer.Retired && Transfer.Ring==HeapRing && Frees==0);
  assert(ScheduleFrees==0 && Closes==0 && Tpl==TPL_APPLICATION);
  FailUninstall=FALSE;
  assert(XhcDriverBindingStop(&Binding,NULL,0,NULL)==EFI_SUCCESS);
  assert(Commands==0 && PoolOutstanding==0 && Transfer.Retired);
  assert(Transfer.Ring==NULL && Transfer.Callback==NULL && Frees==1 && BufferFrees==1);
  assert(Hc.UsbDevContext[1].ConfDesc==NULL && Hc.UsbDevContext[1].OutputContext==NULL);
  assert(Hc.UsbDevContext[1].InputContext==NULL && RingFrees==3);
  assert(Hc.UsbDevContext[2].ConfDesc==NULL && Hc.UsbDevContext[2].ActiveAlternateSetting==NULL);
  assert(ScheduleFrees==1 && Closes==2 && Tpl==TPL_APPLICATION);
}

int main(void) {
  for (UINTN Wide=0; Wide!=2; Wide++) {
    TestSoftRetry((BOOLEAN)Wide);
    TestSlotRetirement((BOOLEAN)Wide);
    TestControllerStop((BOOLEAN)Wide);
    Setup((BOOLEAN)Wide,2); Transfer.Finished=TRUE; Transfer.Result=EDKII_USB_ERR_TRANSACTION;
    assert(XhcDequeueTrbFromEndpoint(&Hc,&Transfer)==EFI_SUCCESS);
    assert(Resets==1 && Stops==0 && Dequeues==1 && Transfer.Result==EDKII_USB_ERR_TRANSACTION);
    Setup((BOOLEAN)Wide,1); RaceHalt=TRUE;
    assert(XhcDequeueTrbFromEndpoint(&Hc,&Transfer)==EFI_SUCCESS);
    assert(Stops==1 && Resets==1 && Dequeues==1 && Hc.PendingUrb==NULL);
    Setup((BOOLEAN)Wide,1);
    assert(XhcDequeueTrbFromEndpoint(&Hc,&Transfer)==EFI_SUCCESS);
    assert(Stops==1 && Dequeues==1 && Hc.PendingUrb==NULL);
    Setup((BOOLEAN)Wide,3); RestartAfter=10;
    assert(XhcDequeueTrbFromEndpoint(&Hc,&Transfer)==EFI_SUCCESS);
    assert(Stops==10 && Dequeues==1 && Delays==9);
    Setup((BOOLEAN)Wide,3); StuckStopped=TRUE;
    assert(XhcDequeueTrbFromEndpoint(&Hc,&Transfer)==EFI_DEVICE_ERROR);
    assert(Stops==100 && Resets==0 && Dequeues==0 && Bells==0 && Hc.PendingUrb==NULL);
    Setup((BOOLEAN)Wide,2); FailReset=TRUE; InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
    assert(XhciDelAsyncIntTransfer(&Hc,7,0x81)==EFI_DEVICE_ERROR);
    assert(Frees==0 && BufferFrees==0 && !IsListEmpty(&Hc.AsyncIntTransfers));
    assert(Transfer.Cancelled && Transfer.Callback==NULL && Transfer.Context==NULL);
    XhcMonitorAsyncRequests(NULL,&Hc); assert(Polls==0 && Bells==0);
    assert(XhciInsertAsyncIntTransfer(&Hc,7,0x81,EFI_USB_SPEED_FULL,8,8,Callback,&Hc)==NULL);
    assert(Allocations==0);
    FailReset=FALSE;
    assert(XhciDelAsyncIntTransfer(&Hc,7,0x81)==EFI_SUCCESS);
    assert(Frees==1 && BufferFrees==1 && IsListEmpty(&Hc.AsyncIntTransfers));
    Setup((BOOLEAN)Wide,1); NoEvent=TRUE;
    assert(XhcStopEndpoint(&Hc,1,3,&Transfer)==EFI_DEVICE_ERROR && Hc.PendingUrb==NULL);
    Setup((BOOLEAN)Wide,2); NoEvent=TRUE;
    assert(XhcResetEndpoint(&Hc,1,3)==EFI_DEVICE_ERROR);
    assert(XhcSetTrDequeuePointer(&Hc,1,3,&Transfer)==EFI_DEVICE_ERROR);
    assert(Ring.RingDequeue==Trbs);
    Setup((BOOLEAN)Wide,1); FailDequeue=TRUE; InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
    assert(XhciDelAsyncIntTransfer(&Hc,7,0x81)==EFI_DEVICE_ERROR);
    assert(Frees==0 && Bells==0 && Transfer.Cancelled);
    Halted=TRUE;
    XhciDelAllAsyncIntTransfers(&Hc);
    assert(Frees==1 && BufferFrees==1);
    Setup((BOOLEAN)Wide,2); FailReset=TRUE; InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
    XhciDelAllAsyncIntTransfers(&Hc);
    assert(Frees==0 && BufferFrees==0 && Transfer.Cancelled);
    Setup((BOOLEAN)Wide,1); Transfer.Finished=TRUE; Transfer.Result=EFI_USB_NOERROR;
    InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
    assert(XhciDelAsyncIntTransfer(&Hc,7,0x81)==EFI_SUCCESS && Frees==1 && Dequeues==0);
    Setup((BOOLEAN)Wide,1); Hc.UsbDevContext[1].Enabled=FALSE; Hc.UsbDevContext[1].SlotId=0;
    Transfer.Ring=(VOID *)(UINTN)1; // Retired rings must never be dereferenced.
    assert(XhcDequeueTrbFromEndpoint(&Hc,&Transfer)==EFI_SUCCESS && Commands==0);
    Setup((BOOLEAN)Wide,1); FailHalt=TRUE; InsertHeadList(&Hc.AsyncIntTransfers,&Transfer.UrbList);
    EFI_DRIVER_BINDING_PROTOCOL Binding={0};
    assert(XhcDriverBindingStop(&Binding,NULL,0,NULL)==EFI_TIMEOUT);
    assert(Uninstalls==0 && Closes==0 && ScheduleFrees==0 && TimerStops==0 && Frees==0);
    assert(Commands==0 && RingFrees==0 && !Transfer.Retired);
    assert(Tpl==TPL_APPLICATION);
    FailHalt=FALSE;
    assert(XhcDriverBindingStop(&Binding,NULL,0,NULL)==EFI_SUCCESS);
    assert(Uninstalls==1 && Closes==2 && ScheduleFrees==1 && Frees==1 && BufferFrees==1);
    assert(Tpl==TPL_APPLICATION);
    Setup((BOOLEAN)Wide,1); Halted=TRUE; Transfer.Ring=(VOID *)(UINTN)1;
    assert(XhcDequeueTrbFromEndpoint(&Hc,&Transfer)==EFI_SUCCESS && Commands==0);
  }
  Setup(FALSE,1);
  Transfer.Ring=&Hc.CmdRing;
  Event.Type=TRB_TYPE_COMMAND_COMPLT_EVENT; Event.Completecode=TRB_COMPLETION_CONTEXT_STATE_ERROR;
  Event.TRBPtrLo=(UINT32)(UINTN)Trbs; Event.TRBPtrHi=(UINT32)((UINTN)Trbs>>32);
  Hc.EventRing.TrbNumber=1; Hc.EventRing.EventRingDequeue=(TRB_TEMPLATE *)&Event;
  HaveEvent=TRUE;
  assert(XhcCheckUrbResult(&Hc,&Transfer));
  assert(Transfer.EvtTrb==(TRB_TEMPLATE *)&Event && Transfer.Result!=EFI_USB_NOERROR);
  puts("xHCI: real 32/64-byte context recovery, halt race, command-error evidence, bounded preserving retry, TT exclusion, retained cancellation, device-slot reuse, retained pre-halted teardown and complete slot cleanup passed");
  return 0;
}
