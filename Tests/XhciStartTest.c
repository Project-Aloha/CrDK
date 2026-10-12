/* Actual DriverBinding Start/Stop failure lifetime; modeled hardware/services. MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Xhci.h"

STATIC EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
STATIC EFI_PCI_IO_PROTOCOL Pci;
STATIC USB_XHCI_INSTANCE *Hc,*mXhcFailedStarts;
STATIC EFI_DRIVER_BINDING_PROTOCOL Binding;
EFI_COMPONENT_NAME_PROTOCOL gXhciComponentName;
EFI_COMPONENT_NAME2_PROTOCOL gXhciComponentName2;
EFI_GUID gEfiPciIoProtocolGuid,gEfiDevicePathProtocolGuid,gEfiUsb2HcProtocolGuid;
EFI_GUID gEdkiiUsb2HcFreezeProtocolGuid,gEfiEventExitBootServicesGuid;
STATIC EFI_HANDLE Controller=(VOID*)0x1000;
enum {OK, NO_INSTANCE, RESET_FAIL, RUN_FAIL, TIMER_FAIL, EVENT_FAIL, INSTALL_FAIL,
      HALT_FAIL, POLL_CLOSE_FAIL, EBS_CLOSE_FAIL, RESTORE_FAIL, PCI_CLOSE_FAIL,
      INIT_FAIL, ATTR_GET_FAIL, ATTR_ENABLE_FAIL, OPEN_FAIL, UNINSTALL_FAIL};
STATIC UINTN StartFault,CleanupFault,Tpl,Runs,Halts,Frees,SchedFrees,Published;
STATIC UINTN SlotFrees,AsyncFrees;
STATIC BOOLEAN Running,PciOpen,Schedule,PollLive,EbsLive,MmioAllowed;
STATIC EFI_STATUS FaultStatus;
STATIC UINT32 Identity, PciReads, AttributeCalls;
STATIC UINT8 FirmwareState;
STATIC UINT16 RomState;
STATIC UINTN ReadFailOffset;
STATIC EFI_STATUS ReadFailStatus;
STATIC EFI_STATUS EFIAPI ReadPci(EFI_PCI_IO_PROTOCOL *P,EFI_PCI_IO_PROTOCOL_WIDTH W,UINT32 O,UINTN N,VOID *B){
  assert(P==&Pci && PciOpen && N==1);++PciReads;
  if(O==ReadFailOffset)return ReadFailStatus;
  if(O==0){assert(W==EfiPciIoWidthUint32);*(UINT32*)B=Identity;}
  else if(O==0xf4){assert(W==EfiPciIoWidthUint8);*(UINT8*)B=FirmwareState;}
  else {assert(O==0xf6 && W==EfiPciIoWidthUint16);*(UINT16*)B=RomState;}
  return EFI_SUCCESS;
}
STATIC EFI_EVENT_NOTIFY EbsFn;
STATIC VOID *EbsContext;
STATIC EFI_STATUS ExactFault(UINTN Phase){return (StartFault==Phase||CleanupFault==Phase)?FaultStatus:EFI_SUCCESS;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
BOOLEAN EFIAPI IsListEmpty(CONST LIST_ENTRY *L){return L->ForwardLink==L;}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
UINT64 EFIAPI MultU64x32(UINT64 V,UINT32 N){return V*N;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL N){assert(N>=Tpl);UINTN Old=Tpl;Tpl=N;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL N){assert(N<=Tpl);Tpl=N;}
VOID *EFIAPI AllocateZeroPool(UINTN N){
  assert(!Hc && !PciOpen && N==sizeof(*Hc));if(StartFault==NO_INSTANCE)return NULL;
  Hc=calloc(1,N);assert(Hc);return Hc;
}
VOID EFIAPI FreePool(VOID *P){
  assert(P==Hc && !Running && !PollLive && !EbsLive && !PciOpen && !Schedule);
  ++Frees;free(P);Hc=NULL;
}
EFI_STATUS EFIAPI FreeUnicodeStringTable(EFI_UNICODE_STRING_TABLE *T){return EFI_SUCCESS;}
EFI_STATUS EFIAPI AddUnicodeString2(CONST CHAR8 *L,CONST CHAR8 *S,EFI_UNICODE_STRING_TABLE **T,CONST CHAR16 *V,BOOLEAN I){return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI OpenProtocol(EFI_HANDLE H,EFI_GUID *G,VOID **P,EFI_HANDLE A,EFI_HANDLE C,UINT32 F){
  assert(H==Controller && C==Controller);
  if(G==&gEfiPciIoProtocolGuid){assert(!PciOpen);EFI_STATUS S=ExactFault(OPEN_FAIL);if(S!=EFI_SUCCESS)return S;PciOpen=TRUE;*P=&Pci;return EFI_SUCCESS;}
  if(G==&gEfiDevicePathProtocolGuid){*P=NULL;return EFI_SUCCESS;}
  assert(G==&gEfiUsb2HcProtocolGuid);if(!Published)return EFI_NOT_FOUND;*P=&Hc->Usb2Hc;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CloseProtocol(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE A,EFI_HANDLE C){
  assert(H==Controller && G==&gEfiPciIoProtocolGuid && C==Controller && PciOpen);
  EFI_STATUS S=ExactFault(PCI_CLOSE_FAIL);if(S==EFI_SUCCESS)PciOpen=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *P,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION O,UINT64 A,UINT64 *R){
  assert(P==&Pci && PciOpen);++AttributeCalls;
  if(O==EfiPciIoAttributeOperationGet){*R=0;return ExactFault(ATTR_GET_FAIL);}
  if(O==EfiPciIoAttributeOperationSupported){*R=EFI_PCI_DEVICE_ENABLE;return EFI_SUCCESS;}
  // A failed Attributes call can commit its writes too. No subsequent register
  // access is allowed after even a failed attempt to restore MEMORY-disabled.
  if(O==EfiPciIoAttributeOperationSet){assert(A==0 && !Running);MmioAllowed=FALSE;return ExactFault(RESTORE_FAIL);}
  assert(O==EfiPciIoAttributeOperationEnable);MmioAllowed=TRUE;return ExactFault(ATTR_ENABLE_FAIL);
}
EFI_STATUS XhcInitializeUsbHc(USB_XHCI_INSTANCE *X,EFI_PCI_IO_PROTOCOL *P,EFI_DEVICE_PATH_PROTOCOL *D,UINT64 A){
  assert(PciOpen && MmioAllowed && X==Hc);
  if(StartFault==INIT_FAIL)return FaultStatus;
  Hc->PciIo=P;Hc->OriginalPciAttributes=A;
  Hc->AsyncIntTransfers.ForwardLink=Hc->AsyncIntTransfers.BackLink=&Hc->AsyncIntTransfers;
  Hc->Signature=XHCI_INSTANCE_SIG;Hc->Freeze.Revision=EDKII_USB2_HC_FREEZE_PROTOCOL_REVISION;
  Hc->Freeze.HostController=&Hc->Usb2Hc;Hc->PollTimer=(VOID*)1;PollLive=TRUE;return EFI_SUCCESS;
}
#ifdef TEST_XHC_OLD_START
USB_XHCI_INSTANCE *XhcCreateUsbHc(EFI_PCI_IO_PROTOCOL *P,EFI_DEVICE_PATH_PROTOCOL *D,UINT64 A){
  assert(PciOpen && !Hc);if(StartFault==NO_INSTANCE)return NULL;
  Hc=calloc(1,sizeof(*Hc));assert(Hc);
  assert(XhcInitializeUsbHc(Hc,P,D,A)==EFI_SUCCESS);return Hc;
}
#endif
VOID XhcSetBiosOwnership(USB_XHCI_INSTANCE *X){assert(X==Hc && MmioAllowed);}
VOID XhcClearBiosOwnership(USB_XHCI_INSTANCE *X){assert(X==Hc && !Running && MmioAllowed);}
EFI_STATUS XhcResetHC(USB_XHCI_INSTANCE *X,UINT32 T){assert(X==Hc);return ExactFault(RESET_FAIL);}
BOOLEAN XhcIsHalt(USB_XHCI_INSTANCE *X){assert(X==Hc);return !Running;}
UINT32 XhcReadOpReg(USB_XHCI_INSTANCE *X,UINT32 O){return 0;}
VOID XhcInitSched(USB_XHCI_INSTANCE *X){assert(X==Hc && !Schedule);Schedule=TRUE;}
VOID XhcFreeSched(USB_XHCI_INSTANCE *X){assert(X==Hc && !Running && !PollLive && !EbsLive);Schedule=FALSE;++SchedFrees;}
EFI_STATUS XhcRunHC(USB_XHCI_INSTANCE *X,UINT32 T){assert(X==Hc && Schedule);Running=TRUE;++Runs;return ExactFault(RUN_FAIL);}
EFI_STATUS XhcHaltHC(USB_XHCI_INSTANCE *X,UINT32 T){
  assert(X==Hc && MmioAllowed);++Halts;EFI_STATUS S=ExactFault(HALT_FAIL);if(S==EFI_SUCCESS)Running=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI SetTimer(EFI_EVENT E,EFI_TIMER_DELAY D,UINT64 T){
  assert(E==Hc->PollTimer && PollLive);return D==TimerPeriodic?ExactFault(TIMER_FAIL):EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CreateEx(UINT32 T,EFI_TPL L,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){
  assert(!EbsLive && C==Hc);EFI_STATUS S=ExactFault(EVENT_FAIL);if(S!=EFI_SUCCESS)return S;
  EbsLive=TRUE;EbsFn=Fn;EbsContext=(VOID*)C;*E=(VOID*)2;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E){
  assert(!Running);EFI_STATUS S=ExactFault(E==(VOID*)1?POLL_CLOSE_FAIL:EBS_CLOSE_FAIL);
  if(S!=EFI_SUCCESS)return S;
  if(E==(VOID*)1){assert(PollLive);PollLive=FALSE;}else{assert(E==(VOID*)2 && EbsLive);EbsLive=FALSE;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
  assert(*H==Controller && Running && Schedule && PollLive && EbsLive);
  EFI_STATUS S=ExactFault(INSTALL_FAIL);if(S==EFI_SUCCESS)Published=1;return S;
}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...){assert(H==Controller && !Running && Published);EFI_STATUS S=ExactFault(UNINSTALL_FAIL);if(S==EFI_SUCCESS)Published=0;return S;}
STATIC VOID SlotAccess(USB_XHCI_INSTANCE *X){
  assert(X==Hc && MmioAllowed && !Running && !Published);
#ifndef TEST_XHC_OLD_STOP
  assert(!PollLive && !EbsLive);
#endif
}
VOID XhcFreeDeviceSlots(USB_XHCI_INSTANCE *X){SlotAccess(X);++SlotFrees;}
VOID XhciDelAllAsyncIntTransfers(USB_XHCI_INSTANCE *X){SlotAccess(X);++AsyncFrees;}
#include "XhciStartProduction.c"

STATIC VOID Setup(UINTN S,UINTN C,BOOLEAN Warn){
  assert(!Hc && !mXhcFailedStarts && !PciOpen && !PollLive && !EbsLive && !Schedule && !Published);
  StartFault=S;CleanupFault=C;FaultStatus=Warn?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
  Tpl=Runs=Halts=Frees=SchedFrees=SlotFrees=AsyncFrees=0;Running=MmioAllowed=FALSE;memset(&Bs,0,sizeof(Bs));
  Bs.OpenProtocol=OpenProtocol;Bs.CloseProtocol=CloseProtocol;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;
  Bs.SetTimer=SetTimer;Bs.CreateEventEx=CreateEx;Bs.CloseEvent=CloseEvent;
  Bs.InstallMultipleProtocolInterfaces=Install;Bs.UninstallMultipleProtocolInterfaces=Uninstall;
  Pci.Attributes=Attributes;Pci.Pci.Read=ReadPci;
  Identity=0x12348086;PciReads=AttributeCalls=0;FirmwareState=0;RomState=0;
  ReadFailOffset=MAX_UINTN;ReadFailStatus=EFI_DEVICE_ERROR;
}
int main(int argc,char **argv){
  UINTN Count=0;
  if(argc==2 && !strncmp(argv[1],"stop:",5)){
    Setup(OK,OK,FALSE);assert(XhcDriverBindingStart(&Binding,Controller,NULL)==EFI_SUCCESS);
    CleanupFault=(UINTN)atoi(argv[1]+5);
    assert(XhcDriverBindingStop(&Binding,Controller,0,NULL)!=EFI_SUCCESS);
    assert(Hc && PciOpen && !Frees && !Published);
    return 0;
  }
  if(argc==2){
    Setup((UINTN)atoi(argv[1]),OK,FALSE);
    assert(XhcDriverBindingStart(&Binding,Controller,NULL)!=EFI_SUCCESS);
    assert(!Hc && !PciOpen && !PollLive && !EbsLive && !Schedule && !Published);
    return 0;
  }
  // Renesas uPD720201/202 must have running firmware before enabling or
  // resetting xHCI. A failed preflight retains no PCI open or MMIO owner.
  struct {UINT32 Id;UINT8 Fw;UINT16 Rom;EFI_STATUS Expected;} FirmwareCases[]={
    {0x00141912,0,0,EFI_NOT_READY},{0x00151912,0,0,EFI_NOT_READY},
    {0x00141912,0x10,0,EFI_SUCCESS},{0x00151912,0x12,0,EFI_SUCCESS},
    {0x00141912,0,0x8010,EFI_SUCCESS},{0x00141912,0,0x8000,EFI_NOT_READY},
    {0x00141912,0x10,0x8000,EFI_SUCCESS},{0x00141912,1,0,EFI_NOT_READY},
    {0x00141912,2,0,EFI_DEVICE_ERROR},{0x00141912,0x20,0,EFI_DEVICE_ERROR},
    {0x00141912,0x30,0,EFI_DEVICE_ERROR},{0x00141912,0,0x8020,EFI_NOT_READY},
    {0x00141912,0,0x8014,EFI_NOT_READY},{0x00131912,0,0,EFI_SUCCESS},
    {0x00148086,0,0,EFI_SUCCESS}
  };
  for(UINTN I=0;I<ARRAY_SIZE(FirmwareCases);++I){
    Setup(OK,OK,FALSE);Identity=FirmwareCases[I].Id;
    FirmwareState=FirmwareCases[I].Fw;RomState=FirmwareCases[I].Rom;
    assert(XhcDriverBindingStart(&Binding,Controller,NULL)==FirmwareCases[I].Expected);
    if(FirmwareCases[I].Expected==EFI_SUCCESS){
      assert(Published && Runs==1);assert(XhcDriverBindingStop(&Binding,Controller,0,NULL)==EFI_SUCCESS);
    }else assert(!Hc && !PciOpen && !AttributeCalls && !Runs && !Halts && !MmioAllowed);
    ++Count;
  }
  for(UINTN W=0;W<2;++W)for(UINTN I=0;I<3;++I){
    Setup(OK,OK,FALSE);Identity=0x00141912;ReadFailOffset=I==0?0:I==1?0xf6:0xf4;
    ReadFailStatus=W?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
    assert(XhcDriverBindingStart(&Binding,Controller,NULL)==(W?EFI_NOT_READY:EFI_DEVICE_ERROR));
    assert(!Hc && !PciOpen && !AttributeCalls && !Runs && !Halts);++Count;
  }
  UINTN Faults[]={NO_INSTANCE,RESET_FAIL,RUN_FAIL,TIMER_FAIL,EVENT_FAIL,INSTALL_FAIL,INIT_FAIL,ATTR_GET_FAIL,ATTR_ENABLE_FAIL,OPEN_FAIL};
  for(UINTN Warn=0;Warn<2;++Warn)for(UINTN I=0;I<ARRAY_SIZE(Faults);++I){
    Setup(Faults[I],OK,(BOOLEAN)Warn);assert(XhcDriverBindingStart(&Binding,Controller,NULL)!=EFI_SUCCESS);
    assert(!Hc && !PciOpen && !PollLive && !EbsLive && !Schedule && !Published);++Count;
  }
  // Failures before a fully initialized instance keep their original PCI open
  // reachable too; cleanup performs no controller accesses in these phases.
  for(UINTN Early=INIT_FAIL;Early<=ATTR_ENABLE_FAIL;++Early)for(UINTN Clean=RESTORE_FAIL;Clean<=PCI_CLOSE_FAIL;++Clean){
    if(Early==ATTR_GET_FAIL && Clean==RESTORE_FAIL)continue;
    Setup(Early,Clean,FALSE);assert(XhcDriverBindingStart(&Binding,Controller,NULL)!=EFI_SUCCESS);
    assert(Hc && mXhcFailedStarts==Hc && PciOpen && !Schedule && !Halts && !Frees);
    CleanupFault=StartFault=OK;assert(XhcDriverBindingStop(&Binding,Controller,0,NULL)==EFI_SUCCESS);
    assert(!Hc && !PciOpen && !Halts);++Count;
  }
  for(UINTN Warn=0;Warn<2;++Warn)for(UINTN Clean=HALT_FAIL;Clean<=PCI_CLOSE_FAIL;++Clean){
    Setup(INSTALL_FAIL,Clean,FALSE);assert(XhcDriverBindingStart(&Binding,Controller,NULL)!=EFI_SUCCESS);
    assert(Hc && mXhcFailedStarts==Hc && Hc->FailedStart && PciOpen && Schedule && !Frees && !Published);
    FaultStatus=Warn?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
    assert(XhcDriverBindingStart(&Binding,Controller,NULL)==EFI_NOT_READY);
    assert(XhcDriverBindingStop(&Binding,Controller,0,NULL)!=EFI_SUCCESS && PciOpen && !Frees);
    CleanupFault=StartFault=OK;assert(XhcDriverBindingStop(&Binding,Controller,0,NULL)==EFI_SUCCESS);
    assert(!Hc && !mXhcFailedStarts && !PciOpen && !Schedule && Frees==1);++Count;
  }
  // Published stop shares the retained journal after protocol withdrawal.
  UINTN StopFaults[]={HALT_FAIL,UNINSTALL_FAIL,POLL_CLOSE_FAIL,EBS_CLOSE_FAIL,RESTORE_FAIL,PCI_CLOSE_FAIL};
  for(UINTN Warn=0;Warn<2;++Warn)for(UINTN I=0;I<ARRAY_SIZE(StopFaults);++I){
    Setup(OK,OK,(BOOLEAN)Warn);assert(XhcDriverBindingStart(&Binding,Controller,NULL)==EFI_SUCCESS && Published);
    CleanupFault=StopFaults[I];assert(XhcDriverBindingStop(&Binding,Controller,0,NULL)!=EFI_SUCCESS);
    assert(Hc && PciOpen && Schedule && !Frees);
    if(CleanupFault==HALT_FAIL || CleanupFault==UNINSTALL_FAIL)assert(Published && !mXhcFailedStarts);
    else assert(!Published && mXhcFailedStarts==Hc && Hc->FailedStart);
    CleanupFault=OK;assert(XhcDriverBindingStop(&Binding,Controller,0,NULL)==EFI_SUCCESS);
    assert(!Hc && !PciOpen && !Published && !mXhcFailedStarts && Frees==1 && SlotFrees==1 && AsyncFrees==1);++Count;
  }
  // A retained instance receiving EBS remains permanently closed to Stop.
  Setup(INSTALL_FAIL,HALT_FAIL,FALSE);assert(XhcDriverBindingStart(&Binding,Controller,NULL)!=EFI_SUCCESS);
  CleanupFault=StartFault=OK;assert(EbsLive);EbsFn((VOID*)2,EbsContext);
  assert(Hc->Frozen && !Running && XhcDriverBindingStop(&Binding,Controller,0,NULL)==EFI_NOT_READY);
  // Simulated reboot disposal, not a production EBS free path.
  free(Hc);Hc=mXhcFailedStarts=NULL;PollLive=EbsLive=PciOpen=Schedule=FALSE;
  printf("xHCI actual Start/Stop: %zu fault/retry phases and EBS retained lifetime PASS\n",(size_t)(Count+1));
  return 0;
}
