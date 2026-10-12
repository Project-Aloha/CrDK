/* Actual xHCI event consumer with an independent controller producer.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Xhci.h"
STATIC USB_XHCI_INSTANCE Hc;
STATIC TRB_TEMPLATE Events[8], TransferTrbs[2], CommandTrbs[2];
#define TransferTrb TransferTrbs[0]
#define CommandTrb CommandTrbs[0]
STATIC TRANSFER_RING TransferRing;
STATIC URB Interrupt, Command;
STATIC UINTN Producer, Produced, Consumed;
STATIC UINT32 ProducerCycle;
STATIC UINT64 RuntimeDequeue;
STATIC UINTN Translations;
STATIC UINTN RuntimeWrites;
STATIC UINT32 LastLowWrite;
VOID *EFIAPI ZeroMem(VOID *P, UINTN N) {return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) {return memcpy(D,S,N);}
VOID EFIAPI MemoryFence(VOID) {}
UINT64 EFIAPI LShiftU64(UINT64 V, UINTN N) {return V<<N;}
UINT64 EFIAPI RShiftU64(UINT64 V, UINTN N) {return V>>N;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) {return FALSE;}
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) {}
BOOLEAN XhcIsHalt(USB_XHCI_INSTANCE *X) {return FALSE;}
BOOLEAN XhcIsSysError(USB_XHCI_INSTANCE *X) {return FALSE;}
EFI_PHYSICAL_ADDRESS UsbHcGetHostAddrForPciAddr(USBHC_MEM_POOL *P, VOID *M, UINTN N, BOOLEAN A) {++Translations;return (UINTN)M;}
EFI_PHYSICAL_ADDRESS UsbHcGetPciAddrForHostAddr(USBHC_MEM_POOL *P, VOID *M, UINTN N, BOOLEAN A) {return (UINTN)M;}
UINT32 XhcReadRuntimeReg(USB_XHCI_INSTANCE *X, UINT32 O) {
  assert(X==&Hc);return O==XHC_ERDP_OFFSET ? (UINT32)RuntimeDequeue : (UINT32)(RuntimeDequeue>>32);
}
VOID XhcWriteRuntimeReg(USB_XHCI_INSTANCE *X, UINT32 O, UINT32 V) {
  assert(X==&Hc);++RuntimeWrites;if(O==XHC_ERDP_OFFSET){LastLowWrite=V;RuntimeDequeue=(RuntimeDequeue&0xffffffff00000000ULL)|(V&~0xfU);}
  else RuntimeDequeue=(RuntimeDequeue&0xffffffffULL)|((UINT64)V<<32);
}
UINT32 XhcReadOpReg(USB_XHCI_INSTANCE *X,UINT32 O) {return 0;}
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *P) {P->ForwardLink=P->BackLink=P;return P;}
LIST_ENTRY *EFIAPI InsertHeadList(LIST_ENTRY *H, LIST_ENTRY *P) {
  P->ForwardLink=H->ForwardLink;P->BackLink=H;H->ForwardLink->BackLink=P;H->ForwardLink=P;return H;
}
#include "XhciEventProduction.c"
STATIC VOID Init(UINTN Start) {
  memset(&Hc,0,sizeof(Hc));memset(Events,0,sizeof(Events));
  memset(&Interrupt,0,sizeof(Interrupt));memset(&Command,0,sizeof(Command));
  memset(&TransferTrb,0,sizeof(TransferTrb));memset(&CommandTrb,0,sizeof(CommandTrb));
  Hc.EventRing=(EVENT_RING){NULL,Events,8,Events+Start,Events+Start,1};
  // Entries before Start were consumed in this cycle; entries after it still
  // carry the opposite producer ownership from their previous generation.
  for(UINTN I=0;I<Start;++I)Events[I].CycleBit=1;
  RuntimeDequeue=(UINTN)(Events+Start);Producer=Start;ProducerCycle=1;Produced=Consumed=Translations=RuntimeWrites=0;
  LastLowWrite=0;
  InitializeListHead(&Hc.AsyncIntTransfers);InsertHeadList(&Hc.AsyncIntTransfers,&Interrupt.UrbList);
  memset(&TransferRing,0,sizeof(TransferRing));
  TransferRing.RingSeg0=TransferRing.RingDequeue=&TransferTrb;Hc.CmdRing.RingSeg0=&CommandTrb;
  TransferTrb.Type=TRB_TYPE_NORMAL;((TRANSFER_TRB_NORMAL *)&TransferTrb)->Length=8;
  CommandTrb.Type=TRB_TYPE_DIS_SLOT;
  Interrupt.TrbStart=Interrupt.TrbEnd=&TransferTrb;Interrupt.TrbNum=1;Interrupt.Ring=&TransferRing;
  Interrupt.Ep.BusAddr=1;Interrupt.Ep.EpAddr=1;Interrupt.Ep.Direction=EfiUsbDataIn;
  Command.TrbStart=Command.TrbEnd=&CommandTrb;Command.TrbNum=1;Command.Ring=&Hc.CmdRing;
  Hc.MaxSlotsEn=2;Hc.UsbDevContext[1].Enabled=TRUE;Hc.UsbDevContext[1].SlotId=1;
  Hc.UsbDevContext[1].BusDevAddr=1;Hc.UsbDevContext[1].EndpointTransferRing[2]=&TransferRing;
}
STATIC VOID Emit(UINT8 Type,UINT8 Code,TRB_TEMPLATE *Trb) {
  assert(Produced-Consumed<8);EVT_TRB_TRANSFER *E=(VOID *)(Events+Producer);memset(E,0,sizeof(*E));
  E->Type=Type;E->Completecode=Code;E->SlotId=1;E->EndpointId=3;
  E->TRBPtrLo=(UINT32)(UINTN)Trb;E->TRBPtrHi=(UINT32)((UINTN)Trb>>32);
  E->CycleBit=ProducerCycle;++Produced;
  if(++Producer==8){Producer=0;ProducerCycle^=1;}
}
#ifndef TEST_OLD_EVENT_CYCLE
STATIC EFI_STATUS Consume(TRB_TEMPLATE **Out) {
  EFI_STATUS S=XhcCheckNewEvent(&Hc,&Hc.EventRing,Out);if(!EFI_ERROR(S))++Consumed;return S;
}
#endif
int main(VOID) {
  // Scan crosses wrap, then one error returns before the actual dequeue wraps.
  // A pending command completion must remain visible on the next poll.
  Init(6);Emit(TRB_TYPE_TRANS_EVENT,TRB_COMPLETION_USB_TRANSACTION_ERROR,&TransferTrb);
  Emit(TRB_TYPE_COMMAND_COMPLT_EVENT,TRB_COMPLETION_SUCCESS,&CommandTrb);
  Emit(TRB_TYPE_TRANS_EVENT,TRB_COMPLETION_SUCCESS,&TransferTrb);
  assert(XhcCheckUrbResult(&Hc,&Interrupt));++Consumed;
  assert(Interrupt.Result==EDKII_USB_ERR_TRANSACTION&&Hc.EventRing.EventRingDequeue==Events+7);
#ifdef TEST_OLD_EVENT_CYCLE
  assert(Hc.EventRing.EventRingCCS==0);
  assert(!XhcCheckUrbResult(&Hc,&Command));
  assert(Hc.EventRing.EventRingDequeue==Events+7);
  puts("Actual old xHCI wrapped-error scan loses following DisableSlot completion: reproduced");
  return 0;
#else
  assert(Hc.EventRing.EventRingCCS==1);
  assert(XhcCheckUrbResult(&Hc,&Command)&&Command.EvtTrb==(TRB_TEMPLATE *)&Events[7]);Consumed+=2;
  assert(Hc.EventRing.EventRingDequeue==Events+1&&Hc.EventRing.EventRingCCS==0);
  TRB_TEMPLATE *T=NULL;assert(Consume(&T)==EFI_NOT_READY);
  // Producer can publish a new event after the scan. Ownership is checked at
  // dequeue time; the cached scan position must not suppress it.
  assert(XhcSyncEventRing(&Hc,&Hc.EventRing)==EFI_SUCCESS);
  Emit(TRB_TYPE_TRANS_EVENT,TRB_COMPLETION_SUCCESS,&TransferTrb);
  assert(Consume(&T)==EFI_SUCCESS&&T==Events+1);assert(Consume(&T)==EFI_NOT_READY);
  // Full ring: equal enqueue/dequeue pointers are not an empty-ring proof.
  Init(3);for(UINTN I=0;I<8;++I)Emit(TRB_TYPE_TRANS_EVENT,TRB_COMPLETION_SUCCESS,&TransferTrb);
  assert(XhcSyncEventRing(&Hc,&Hc.EventRing)==EFI_SUCCESS);
  assert(Hc.EventRing.EventRingCCS==1);
  for(UINTN I=0;I<8;++I)assert(Consume(&T)==EFI_SUCCESS&&T==Events+(3+I)%8);
  assert(Consume(&T)==EFI_NOT_READY&&Hc.EventRing.EventRingCCS==0);
  Init(3);
  for(UINTN I=0;I<7;++I)Emit(TRB_TYPE_PORT_STATUS_CHANGE_EVENT,TRB_COMPLETION_SUCCESS,&TransferTrb);
  Emit(TRB_TYPE_COMMAND_COMPLT_EVENT,TRB_COMPLETION_SUCCESS,&CommandTrb);
  assert(XhcCheckUrbResult(&Hc,&Command)&&Command.EvtTrb==(TRB_TEMPLATE *)(Events+2));Consumed+=8;
  assert(Hc.EventRing.EventRingDequeue==Events+3&&Hc.EventRing.EventRingCCS==0);
  assert(RuntimeDequeue==(UINTN)(Events+3)&&RuntimeWrites==2&&(LastLowWrite&BIT3));
  // Repeated ownership generations do not replay the old transaction error.
  Init(7);for(UINTN Round=0;Round<1024;++Round){
    Emit(TRB_TYPE_TRANS_EVENT,Round==0 ? TRB_COMPLETION_USB_TRANSACTION_ERROR : TRB_COMPLETION_SUCCESS,&TransferTrb);
    assert(XhcSyncEventRing(&Hc,&Hc.EventRing)==EFI_SUCCESS);
    assert(Consume(&T)==EFI_SUCCESS);
    assert(((EVT_TRB_TRANSFER *)T)->Completecode==(Round==0 ? TRB_COMPLETION_USB_TRANSACTION_ERROR : TRB_COMPLETION_SUCCESS));
    assert(Consume(&T)==EFI_NOT_READY);
  }
  // An unowned event cannot modify the caller's output pointer.
  T=&CommandTrb;assert(Consume(&T)==EFI_NOT_READY&&T==&CommandTrb);
  // Foreign slot/DCI, disabled slot and mismatched ring are not this request;
  // unassigned slots are rejected before translating their old DMA pointers.
  for(UINTN Case=0;Case<6;++Case){
    Init(0);Emit(TRB_TYPE_TRANS_EVENT,TRB_COMPLETION_USB_TRANSACTION_ERROR,&TransferTrb);
    EVT_TRB_TRANSFER *E=(VOID *)Events;
    if(Case==0)E->SlotId=0;
    if(Case==1)E->SlotId=3;
    if(Case==2)Hc.UsbDevContext[1].Enabled=FALSE;
    if(Case==3)E->EndpointId=2;
    if(Case==4)Hc.UsbDevContext[1].BusDevAddr=2;
    if(Case==5)Hc.UsbDevContext[1].EndpointTransferRing[2]=NULL;
    assert(!XhcCheckUrbResult(&Hc,&Interrupt)&&!Interrupt.Result&&!Interrupt.Completed);
    assert(TransferRing.RingDequeue==&TransferTrb);if(Case<3)assert(Translations==0);
  }
  Init(0);Emit(TRB_TYPE_COMMAND_COMPLT_EVENT,TRB_COMPLETION_USB_TRANSACTION_ERROR,&TransferTrb);
  assert(!XhcCheckUrbResult(&Hc,&Interrupt)&&!Interrupt.Result);
  puts("Actual xHCI wrapped error/command, concurrent publication, full ring and 1024 producer generations PASS");
  return 0;
#endif
}
