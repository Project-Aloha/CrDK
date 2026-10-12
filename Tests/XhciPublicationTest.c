/* Actual TRB construction against a weakly ordered DMA visibility model.
   Device observation at ring advancement is independent of CPU store order.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Xhci.h"

STATIC USB_XHCI_INSTANCE Hc;
STATIC DEVICE_CONTEXT Context;
STATIC DEVICE_CONTEXT_64 Context64;
STATIC EFI_PCI_IO_PROTOCOL Pci;
STATIC TRANSFER_RING Ring;
STATIC TRB_TEMPLATE Trbs[16], Visible[16];
STATIC UINT32 Published[16];
STATIC UINTN Fences, Observations;
STATIC BOOLEAN Watching;
STATIC URB Transfer;
STATIC UINT8 Buffer[0x20000];
EFI_BOOT_SERVICES *gBS;

STATIC VOID Observe(VOID) {
  UINTN I;
  if (!Watching) return;
  for (I=0; I<16; ++I) {
    UINT32 Control=((UINT32 *)&Trbs[I])[3];
    /* A new ownership/control publication is visible before ordinary DMA
       stores unless the producer orders the complete body first. */
    if (Control==Published[I] || Trbs[I].Type==0) continue;
    Observations++;
    assert(Trbs[I].Parameter1==Visible[I].Parameter1);
    assert(Trbs[I].Parameter2==Visible[I].Parameter2);
    assert(Trbs[I].Status==Visible[I].Status);
    Published[I]=Control;
  }
}
VOID EFIAPI MemoryFence(VOID) { Fences++; memcpy(Visible,Trbs,sizeof(Trbs)); }
UINT64 EFIAPI RShiftU64(UINT64 Value, UINTN Shift) { return Value>>Shift; }
VOID *EFIAPI ZeroMem(VOID *P, UINTN N) { return memset(P,0,N); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) {
  if (Watching && (UINTN)D>=(UINTN)Trbs &&
      (UINTN)D<(UINTN)(Trbs+16) && N==sizeof(TRB_TEMPLATE)) {
    /* A whole-structure copy can expose its control word independently.
       The command copied into the DMA ring must still be unowned. */
    assert(((CONST TRB_TEMPLATE *)S)->CycleBit!=(Hc.CmdRing.RingPCS&BIT0));
  }
  return memcpy(D,S,N);
}
VOID *EFIAPI AllocateZeroPool(UINTN N) { return calloc(1,N); }
VOID EFIAPI FreePool(VOID *P) { free(P); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) {}
STATIC EFI_STATUS EFIAPI Map(EFI_PCI_IO_PROTOCOL *P,
 EFI_PCI_IO_PROTOCOL_OPERATION Op, VOID *Data, UINTN *N,
 EFI_PHYSICAL_ADDRESS *Address, VOID **Token) {
  assert(P==&Pci && Data==Buffer); *Address=0x12345000; *Token=Buffer;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Unmap(EFI_PCI_IO_PROTOCOL *P, VOID *Token) {
  assert(P==&Pci && Token==Buffer); return EFI_SUCCESS;
}
EFI_STATUS EFIAPI XhcSyncTrsRing(USB_XHCI_INSTANCE *X, TRANSFER_RING *R);
#include "XhciPublicationProduction.c"
EFI_STATUS EFIAPI XhcSyncTrsRing(USB_XHCI_INSTANCE *X, TRANSFER_RING *R) {
  Observe(); return ActualXhcSyncTrsRing(X,R);
}

STATIC VOID Setup(UINT8 Type, UINT8 Cycle, BOOLEAN Wide, UINTN Start) {
  memset(&Hc,0,sizeof(Hc)); memset(&Context,0,sizeof(Context));
  memset(&Context64,0,sizeof(Context64)); memset(&Ring,0,sizeof(Ring));
  memset(&Transfer,0,sizeof(Transfer)); memset(Trbs,0,sizeof(Trbs));
  memset(Visible,0,sizeof(Visible)); memset(Published,0,sizeof(Published));
  Hc.HcCParams.Data.Csz=Wide; Hc.MaxSlotsEn=2; Hc.PciIo=&Pci;
  Hc.UsbDevContext[1].Enabled=TRUE; Hc.UsbDevContext[1].BusDevAddr=7;
  Hc.UsbDevContext[1].SlotId=1;
  Hc.UsbDevContext[1].OutputContext=Wide ? (VOID *)&Context64 : (VOID *)&Context;
  Context.EP[2].EPType=Context64.EP[2].EPType=Type;
  Hc.UsbDevContext[1].EndpointTransferRing[2]=&Ring;
  Ring.RingSeg0=Trbs; Ring.TrbNumber=16; Ring.RingPCS=Cycle;
  Ring.RingEnqueue=Trbs+Start; Ring.RingDequeue=Trbs+14;
  for (UINTN I=0; I<16; ++I) Trbs[I].CycleBit=!Cycle;
  LINK_TRB *Link=(LINK_TRB *)&Trbs[15]; Link->Type=TRB_TYPE_LINK;
  Link->TC=1; Trbs[15].Parameter1=0x45678000;
  /* A wrap returns to retired entries from the previous producer cycle. */
  if (Start!=0) for (UINTN I=0; I<Start; ++I) Trbs[I].CycleBit=Cycle;
  Transfer.Ep.BusAddr=7; Transfer.Ep.EpAddr=1;
  Transfer.Ep.Direction=EfiUsbDataIn; Transfer.Data=Buffer;
  Transfer.DataLen=8;
  Pci.Map=Map; Pci.Unmap=Unmap;
  memcpy(Visible,Trbs,sizeof(Trbs));
  for (UINTN I=0; I<16; ++I) Published[I]=((UINT32 *)&Trbs[I])[3];
  Fences=Observations=0; Watching=TRUE;
}
int main(VOID) {
  EFI_USB_DEVICE_REQUEST Request={0x80,6,0x100,0,8};
  for (UINTN Wide=0; Wide<2; ++Wide) for (UINTN Cycle=0; Cycle<2; ++Cycle) {
    Setup(ED_INTERRUPT_IN,(UINT8)Cycle,(BOOLEAN)Wide,0);
    for (UINTN I=0; I<6; ++I) {
      assert(XhcCreateTransferTrb(&Hc,&Transfer)==EFI_SUCCESS);
      Observe(); assert(Transfer.TrbNum==1);
      assert(Transfer.TrbStart->Parameter1==0x12345000);
    }
    assert(Fences>=6 && Observations>=6);
    Setup(ED_CONTROL_BIDIR,(UINT8)Cycle,(BOOLEAN)Wide,0);
    Transfer.Request=&Request;
    assert(XhcCreateTransferTrb(&Hc,&Transfer)==EFI_SUCCESS);
    Observe(); assert(Transfer.TrbNum==3 && Fences>=3);
    Setup(ED_BULK_IN,(UINT8)Cycle,(BOOLEAN)Wide,0);
    Transfer.DataLen=sizeof(Buffer);
    assert(XhcCreateTransferTrb(&Hc,&Transfer)==EFI_SUCCESS);
    Observe(); assert(Transfer.TrbNum==2 && Fences>=2);
    Setup(ED_INTERRUPT_IN,(UINT8)Cycle,(BOOLEAN)Wide,13);
    Ring.RingDequeue=Trbs+12;
    Transfer.DataLen=sizeof(Buffer);
    assert(XhcCreateTransferTrb(&Hc,&Transfer)==EFI_SUCCESS);
    Observe(); assert(Ring.RingEnqueue==Trbs && Ring.RingPCS!=Cycle);
    assert(Fences>=3); /* both payloads and the link ownership */
    Setup(ED_INTERRUPT_IN,(UINT8)Cycle,(BOOLEAN)Wide,0);
    Hc.CmdRing=Ring;
    TRB_TEMPLATE Command={0x1234,0x5678,0x9876,0,0,TRB_TYPE_RESET_ENDPOINT,0};
    URB *U=XhcCreateCmdTrb(&Hc,&Command);
    assert(U!=NULL); Observe(); assert(Fences!=0);
    assert(U->TrbStart->CycleBit==Cycle); free(U);
  }
  puts("actual xHCI TRB: weak DMA visibility, both cycle phases, control/bulk/interrupt, link and command PASS");
  return 0;
}
