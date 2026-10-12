/* SPDX-License-Identifier: MIT
 * Actual completion/remap path with independently tracked PCI mapping tokens.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Xhci.h"

STATIC USB_XHCI_INSTANCE Hc;
STATIC EFI_PCI_IO_PROTOCOL Pci;
STATIC EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
STATIC URB *Transfer;
STATIC UINT8 Buffer[8], OldToken, NewToken;
STATIC BOOLEAN OldOwned, NewOwned, FailUnmap, FailMap, ShortMap, IdentityMap;
STATIC BOOLEAN DeleteInCallback,FreezeInCallback;
STATIC VOID *RetainedBuffer;
STATIC UINTN Maps, Unmaps, Builds, Bells, Callbacks, Allocations, Tpl;
STATIC UINT32 ExpectedResult;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) {}
VOID *EFIAPI AllocateZeroPool(UINTN N) { Allocations++; return calloc(1,N); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
STATIC EFI_STATUS EFIAPI ReleasePool(VOID *P) { free(P); return EFI_SUCCESS; }
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *P) { P->ForwardLink=P->BackLink=P; return P; }
LIST_ENTRY *EFIAPI InsertHeadList(LIST_ENTRY *H, LIST_ENTRY *P) {
  P->ForwardLink=H->ForwardLink; P->BackLink=H;
  H->ForwardLink->BackLink=P; H->ForwardLink=P; return H;
}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL N) { UINTN Old=Tpl; assert(N>=Old); Tpl=N; return Old; }
STATIC VOID EFIAPI Restore(EFI_TPL N) { assert(N<=Tpl); Tpl=N; }
STATIC EFI_STATUS EFIAPI Map(EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_OPERATION Op,
 VOID *Data, UINTN *Len, EFI_PHYSICAL_ADDRESS *Address, VOID **Token) {
  assert(P==&Pci && Data==Buffer && *Len==sizeof(Buffer));
  assert(!OldOwned && !NewOwned);
  assert(Op==(Transfer->Ep.Direction==EfiUsbDataIn ?
    EfiPciIoOperationBusMasterWrite : EfiPciIoOperationBusMasterRead));
  Maps++;
  if (FailMap) return EFI_OUT_OF_RESOURCES;
  *Len=ShortMap ? 4 : sizeof(Buffer); *Address=0x87654000;
  *Token=IdentityMap ? NULL : &NewToken; NewOwned=!IdentityMap;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Unmap(EFI_PCI_IO_PROTOCOL *P, VOID *Token) {
  assert(P==&Pci); Unmaps++;
  assert((Token==&OldToken && OldOwned) || (Token==&NewToken && NewOwned));
  if (FailUnmap) return EFI_DEVICE_ERROR;
  if (Token==&OldToken) OldOwned=FALSE; else NewOwned=FALSE;
  return EFI_SUCCESS;
}
UINT8 XhcBusDevAddrToSlotId(USB_XHCI_INSTANCE *X, UINT8 A) { assert(X==&Hc && A==7); return 1; }
BOOLEAN XhcCheckUrbResult(USB_XHCI_INSTANCE *X, URB *U) { return U->Finished; }
BOOLEAN XhcRetryTransaction(USB_XHCI_INSTANCE *X, URB *U) { return FALSE; }
EFI_STATUS XhcCreateTransferTrb(USB_XHCI_INSTANCE *X, URB *U) {
  /* Successful monitor may submit only a full, current mapping. */
  assert(!FailMap && !FailUnmap && !ShortMap && !OldOwned);
  assert(U->DataPhy==(VOID *)(UINTN)0x87654000 && (NewOwned || IdentityMap));
  Builds++; U->Finished=FALSE; U->Completed=0; return EFI_SUCCESS;
}
EFI_STATUS RingIntTransferDoorBell(USB_XHCI_INSTANCE *X, URB *U) { Bells++; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Callback(VOID *Data, UINTN Length, VOID *Context, UINT32 Result) {
  assert(Tpl==TPL_APPLICATION && Context==&Hc && Result==ExpectedResult); Callbacks++;
  if (Result==EFI_USB_NOERROR) {
    assert(Data!=NULL && Length==8 && memcmp(Data,Buffer,8)==0 && Bells==1);
  } else {
    assert(Data==NULL && Length==0 && Builds==0 && Bells==0 && Allocations==0);
  }
  if(FreezeInCallback){
    UINT32 Active=0;assert(Hc.ActiveCalls==1);
    assert(Hc.Freeze.Freeze(&Hc.Freeze,&Active)==EFI_NOT_READY && Active==1 && Hc.Frozen);
    RetainedBuffer=Data;
  }
  if (DeleteInCallback) {
    /* Self-cancellation may retire the URB before the monitor resumes. */
    if (Transfer->DataMap!=NULL) {
      FailUnmap=FALSE; assert(Unmap(&Pci,Transfer->DataMap)==EFI_SUCCESS);
    }
    Hc.AsyncIntTransfers.ForwardLink=Hc.AsyncIntTransfers.BackLink=&Hc.AsyncIntTransfers;
    free(Transfer); Transfer=NULL;
  }
  return EFI_SUCCESS;
}
#include "XhciAsyncMapProduction.c"

STATIC VOID Setup(BOOLEAN Out) {
  memset(&Hc,0,sizeof(Hc)); memset(&Pci,0,sizeof(Pci));
  Transfer=calloc(1,sizeof(*Transfer)); assert(Transfer);
  Hc.Signature=XHCI_INSTANCE_SIG;
  Hc.Freeze=(EDKII_USB2_HC_FREEZE_PROTOCOL){EDKII_USB2_HC_FREEZE_PROTOCOL_REVISION,&Hc.Usb2Hc,XhcFreeze};
  Hc.PciIo=&Pci; Pci.Map=Map; Pci.Unmap=Unmap;
  Services.RaiseTPL=Raise; Services.RestoreTPL=Restore; Services.FreePool=ReleasePool;
  InitializeListHead(&Hc.AsyncIntTransfers);
  Transfer->Ep.BusAddr=7; Transfer->Ep.Direction=Out ? EfiUsbDataOut : EfiUsbDataIn;
  Transfer->Data=Buffer; Transfer->DataLen=8; Transfer->Completed=8;
  Transfer->DataMap=&OldToken; Transfer->DataPhy=(VOID *)(UINTN)0x12345000;
  Transfer->Callback=Callback; Transfer->Context=&Hc; Transfer->Finished=TRUE;
  memset(Buffer,0xA5,sizeof(Buffer));
  OldOwned=TRUE; NewOwned=FailMap=FailUnmap=ShortMap=IdentityMap=DeleteInCallback=FALSE;
  FreezeInCallback=FALSE;RetainedBuffer=NULL;
  Maps=Unmaps=Builds=Bells=Callbacks=Allocations=0; Tpl=TPL_APPLICATION; ExpectedResult=0;
}
STATIC VOID Cleanup(VOID) {
  if (Transfer!=NULL) {
    if (Transfer->DataMap!=NULL) { FailUnmap=FALSE; assert(Unmap(&Pci,Transfer->DataMap)==EFI_SUCCESS); }
    free(Transfer); Transfer=NULL;
  }
  assert(!OldOwned && !NewOwned);
}
STATIC VOID Refresh(VOID) {
  for (UINTN Out=0; Out<2; Out++) {
    Setup((BOOLEAN)Out); FailMap=TRUE;
    assert(XhcFlushAsyncIntMap(&Hc,Transfer)==EFI_DEVICE_ERROR);
    assert(Transfer->DataPhy==NULL && Transfer->DataMap==NULL && !OldOwned && !NewOwned);
    Cleanup();
    Setup((BOOLEAN)Out); ShortMap=TRUE;
    assert(XhcFlushAsyncIntMap(&Hc,Transfer)==EFI_DEVICE_ERROR);
    assert(Transfer->DataMap==&NewToken && Transfer->DataPhy==NULL && NewOwned && Unmaps==1);
    /* A later refresh must release the retained short mapping exactly once. */
    ShortMap=FALSE; assert(XhcFlushAsyncIntMap(&Hc,Transfer)==EFI_SUCCESS);
    assert(Unmaps==2 && Maps==2 && NewOwned); Cleanup();
    Setup((BOOLEAN)Out); FailUnmap=TRUE;
    assert(XhcFlushAsyncIntMap(&Hc,Transfer)==EFI_DEVICE_ERROR);
    assert(Transfer->DataMap==&OldToken && OldOwned && Maps==0); Cleanup();
    Setup((BOOLEAN)Out); IdentityMap=TRUE;
    assert(XhcFlushAsyncIntMap(&Hc,Transfer)==EFI_SUCCESS);
    assert(Transfer->DataMap==NULL && Transfer->DataPhy==(VOID *)(UINTN)0x87654000); Cleanup();
  }
}
STATIC VOID Monitor(VOID) {
  Setup(FALSE);InsertHeadList(&Hc.AsyncIntTransfers,&Transfer->UrbList);
  UINT32 Active=99;assert(Hc.Freeze.Freeze(&Hc.Freeze,&Active)==EFI_SUCCESS && Active==0);
  XhcMonitorAsyncRequests(NULL,&Hc);
  assert(!Maps&&!Unmaps&&!Builds&&!Bells&&!Callbacks&&!Allocations&&OldOwned);
  assert(Hc.ActiveCalls==0 && Tpl==TPL_APPLICATION);Cleanup();
  Setup(FALSE);FreezeInCallback=TRUE;InsertHeadList(&Hc.AsyncIntTransfers,&Transfer->UrbList);
  XhcMonitorAsyncRequests(NULL,&Hc);
  assert(Hc.Frozen && Hc.ActiveCalls==0 && Callbacks==1 && RetainedBuffer && Tpl==TPL_APPLICATION);
  assert(Hc.Freeze.Freeze(&Hc.Freeze,&Active)==EFI_SUCCESS && Active==0);
  UINTN OldMaps=Maps,OldUnmaps=Unmaps;
  XhcMonitorAsyncRequests(NULL,&Hc);assert(Callbacks==1 && Maps==OldMaps && Unmaps==OldUnmaps);
  free(RetainedBuffer);Cleanup();

  for (UINTN Mode=0; Mode<4; Mode++) {
    Setup(FALSE); DeleteInCallback=TRUE;
    FailMap=Mode==1; ShortMap=Mode==2; FailUnmap=Mode==3;
    ExpectedResult=Mode ? EFI_USB_ERR_BUFFER : EFI_USB_NOERROR;
    InsertHeadList(&Hc.AsyncIntTransfers,&Transfer->UrbList);
    XhcMonitorAsyncRequests(NULL,&Hc);
    assert(Callbacks==1 && Transfer==NULL && Tpl==TPL_APPLICATION); Cleanup();
  }
}
int main(int argc, char **argv) {
  assert(argc==2);
  if (strcmp(argv[1],"refresh")==0) Refresh(); else { assert(strcmp(argv[1],"monitor")==0); Monitor(); }
  puts("xHCI async DMA ownership, short mapping, error callback and self-cancellation passed"); return 0;
}
