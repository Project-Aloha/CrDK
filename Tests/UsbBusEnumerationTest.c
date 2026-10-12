/* Production USB bus enumeration/removal against an independent port/driver
 * ownership model. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "UsbBus.h"

STATIC USB_BUS Bus;
STATIC USB_DEVICE Root,Old,Descendant,Replacement;
STATIC USB_INTERFACE Hub;
STATIC USB_HUB_API Api;
STATIC EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
STATIC EFI_USB_PORT_STATUS Physical;
STATIC UINTN Removals,Freed,Enumerated,Reads,Clears,Acks,FailureCount;
STATIC EFI_STATUS ReadFailure,EnumerationFailure,TimerFailure,CloseFailure;
STATIC USB_ENDPOINT_DESC HubEndpoint;
STATIC USB_INTERFACE_SETTING HubSetting;
STATIC USB_ENDPOINT_DESC *HubEndpoints[1];
STATIC struct {EFI_EVENT_NOTIFY Notify;VOID *Context;BOOLEAN Armed,Closed,Signaled;} Timer;
STATIC BOOLEAN DeferSignal,SubmitFailure,AsyncOwned;
STATIC UINTN AlreadyCancelled;
STATIC UINTN Signals,TimerArms,TimerCancels,Closes;
USB_HUB_API mUsbHubApi;
STATIC BOOLEAN ResetRequested,AdmissionClosed,PartialOnFailure,PartialDisconnectFail;

VOID *EFIAPI AllocateZeroPool(UINTN N){return calloc(1,N);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){}
STATIC EFI_STATUS EFIAPI ReleasePool(VOID *P){free(P);return EFI_SUCCESS;}
EFI_STATUS UsbRemoveConfig(USB_DEVICE *D){
  Removals++;
  if(FailureCount){FailureCount--;return EFI_NOT_READY;}
  D->NumOfInterface=0;return EFI_SUCCESS;
}
VOID UsbFreeDevice(USB_DEVICE *D){assert(D!=&Root);Freed++;}
EFI_STATUS UsbEnumerateNewDev(USB_INTERFACE *H,UINT8 Port,BOOLEAN Reset){
  assert(H==&Hub&&Port==0);Enumerated++;ResetRequested=Reset;
  // Replacement ownership is legal only after the entire old subtree retires.
  if(Bus.Devices[1]||Bus.Devices[2])AdmissionClosed=FALSE;
  if(EFI_ERROR(EnumerationFailure)&&!PartialOnFailure)return EnumerationFailure;
  Replacement=(USB_DEVICE){.Address=1,.ParentAddr=Root.Address,.ParentPort=Port,.ParentIf=H,.Bus=&Bus,.Connected=TRUE,.DisconnectFail=PartialDisconnectFail};
  Bus.Devices[1]=&Replacement;return EnumerationFailure;
}
EFI_STATUS UsbHubAckHubStatus(USB_DEVICE *D){assert(D==&Root);Acks++;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI PortStatus(USB_INTERFACE *H,UINT8 Port,EFI_USB_PORT_STATUS *S){
  assert(H==&Hub&&Port==0);Reads++;if(EFI_ERROR(ReadFailure))return ReadFailure;
  *S=Physical;
  // Real xHCI GetRootHubPortStatus ACKs the physical change before returning
  // it to the bus, so a subsequent poll cannot rely on the original bits.
  Physical.PortChangeStatus=0;return EFI_SUCCESS;
}
STATIC VOID EFIAPI Clear(USB_INTERFACE *H,UINT8 Port){assert(H==&Hub&&Port==0);Clears++;}
STATIC EFI_STATUS EFIAPI CreateEvent(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Out){
  assert(Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL)&&Tpl==TPL_CALLBACK);
  Timer.Notify=Notify;Timer.Context=Context;Timer.Closed=FALSE;*Out=&Timer;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetTimer(EFI_EVENT Event,EFI_TIMER_DELAY Delay,UINT64 Time){
  assert(Event==&Timer&&!Timer.Closed);
  if(EFI_ERROR(TimerFailure))return TimerFailure;
  if(Delay==TimerRelative){assert(Time==USB_ROOTHUB_POLL_INTERVAL);Timer.Armed=TRUE;TimerArms++;}
  else{assert(Delay==TimerCancel&&Time==0);Timer.Armed=FALSE;TimerCancels++;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CloseEvent(EFI_EVENT Event){
  assert(Event==&Timer&&!Timer.Closed);
  if(EFI_ERROR(CloseFailure))return CloseFailure;
  assert(!Timer.Armed);Timer.Closed=TRUE;Closes++;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SignalEvent(EFI_EVENT Event){
  assert(Event==&Timer&&!Timer.Closed);Signals++;Timer.Signaled=TRUE;
  if(!DeferSignal){Timer.Signaled=FALSE;Timer.Notify(Event,Timer.Context);}return EFI_SUCCESS;
}
STATIC VOID FireTimer(VOID){assert(Timer.Armed&&!Timer.Closed);Timer.Armed=FALSE;Timer.Notify(&Timer,Timer.Context);}
STATIC EFI_STATUS EFIAPI AsyncTransfer(EFI_USB_IO_PROTOCOL *Io,UINT8 Ep,BOOLEAN New,UINTN Interval,UINTN Length,EFI_ASYNC_USB_TRANSFER_CALLBACK Callback,VOID *Context){
  assert(Io==&Hub.UsbIo&&Ep==0x81);
  if(New){if(SubmitFailure)return EFI_DEVICE_ERROR;AsyncOwned=TRUE;return EFI_SUCCESS;}
  if(!AsyncOwned){AlreadyCancelled++;return EFI_NOT_FOUND;}
  AsyncOwned=FALSE;return EFI_SUCCESS;
}
EFI_STATUS UsbIoClearFeature(EFI_USB_IO_PROTOCOL *Io,UINTN Target,UINT16 Feature,UINT16 Index){return EFI_SUCCESS;}
EFI_STATUS UsbHubReadDesc(USB_DEVICE *D,EFI_USB_HUB_DESCRIPTOR *Desc){memset(Desc,0,sizeof(*Desc));Desc->NumPorts=1;return EFI_SUCCESS;}
EFI_STATUS UsbHubCtrlSetHubDepth(USB_DEVICE *D,UINT16 Depth){return EFI_SUCCESS;}
EFI_STATUS UsbHubCtrlSetPortFeature(USB_DEVICE *D,UINT8 Port,EFI_USB_PORT_FEATURE Feature){return EFI_SUCCESS;}
#include "UsbBusEnumerationProduction.c"

STATIC VOID Setup(BOOLEAN External,BOOLEAN Child){
  memset(&Bus,0,sizeof(Bus));memset(&Root,0,sizeof(Root));memset(&Old,0,sizeof(Old));
  memset(&Descendant,0,sizeof(Descendant));memset(&Hub,0,sizeof(Hub));
  Bus.MaxDevices=8;Bus.Devices[0]=&Root;Root.Bus=&Bus;Root.Connected=TRUE;
  Hub.Device=&Root;Hub.HubApi=&Api;Hub.NumOfPort=1;Hub.IsHub=TRUE;
  Api.GetPortStatus=PortStatus;Api.ClearPortChange=Clear;Services.FreePool=ReleasePool;
  Services.CreateEvent=CreateEvent;Services.SetTimer=SetTimer;Services.CloseEvent=CloseEvent;Services.SignalEvent=SignalEvent;
  HubEndpoint.Desc.EndpointAddress=0x81;HubEndpoint.Desc.Attributes=USB_ENDPOINT_INTERRUPT;
  HubEndpoints[0]=&HubEndpoint;memset(&HubSetting,0,sizeof(HubSetting));HubSetting.Desc.NumEndpoints=1;HubSetting.Endpoints=HubEndpoints;
  Hub.IfSetting=&HubSetting;Hub.UsbIo.UsbAsyncInterruptTransfer=AsyncTransfer;mUsbHubApi=Api;
  memset(&Timer,0,sizeof(Timer));TimerFailure=CloseFailure=EFI_SUCCESS;SubmitFailure=DeferSignal=FALSE;
  Signals=TimerArms=TimerCancels=Closes=AlreadyCancelled=0;AsyncOwned=FALSE;
  Old=(USB_DEVICE){.Address=1,.ParentAddr=0,.ParentPort=0,.ParentIf=&Hub,.Bus=&Bus,.Connected=TRUE};
  Bus.Devices[1]=&Old;
  if(Child){Descendant=(USB_DEVICE){.Address=2,.ParentAddr=1,.ParentPort=1,.Bus=&Bus,.Connected=TRUE};Bus.Devices[2]=&Descendant;}
  if(External){assert(UsbHubInit(&Hub)==EFI_SUCCESS);}
  Physical=(EFI_USB_PORT_STATUS){.PortStatus=USB_PORT_STAT_CONNECTION|USB_PORT_STAT_ENABLE,.PortChangeStatus=USB_PORT_STAT_C_CONNECTION};
  Removals=Freed=Enumerated=Reads=Clears=Acks=FailureCount=0;
  ReadFailure=EnumerationFailure=EFI_SUCCESS;ResetRequested=FALSE;AdmissionClosed=TRUE;PartialOnFailure=PartialDisconnectFail=FALSE;
}
STATIC VOID Enumerate(BOOLEAN External){
  if(External){
    if(Physical.PortChangeStatus){UINT8 Data=2;assert(UsbOnHubInterrupt(&Data,1,&Hub,0)==EFI_SUCCESS);}
    else FireTimer();
  }else UsbRootHubEnumeration(NULL,&Hub);
}
int main(VOID){
  for(UINTN External=0;External<2;External++){
    Setup((BOOLEAN)External,FALSE);FailureCount=1;Enumerate((BOOLEAN)External);
#ifdef TEST_BEFORE_RECONNECT_RETENTION
    assert(Enumerated==1&&!AdmissionClosed&&Bus.Devices[1]==&Replacement);
    Setup((BOOLEAN)External,TRUE);FailureCount=1;
    assert(UsbRemoveDevice(&Old)==EFI_NOT_READY&&Old.Connected&&!Old.DisconnectFail&&Bus.Devices[1]==&Old&&Bus.Devices[2]==&Descendant);
    if(Hub.ChangeMap)free(Hub.ChangeMap);
#else
    assert(!Enumerated&&AdmissionClosed&&Bus.Devices[1]==&Old&&!Old.Connected&&Old.DisconnectFail&&Hub.PortReenumerationPending[0]&&!Physical.PortChangeStatus);
    assert(UsbEnumeratePort(&Hub,0)==EFI_SUCCESS&&Enumerated==1&&AdmissionClosed&&Freed==1&&ResetRequested&&Bus.Devices[1]==&Replacement&&!Hub.PortReenumerationPending[0]);
    Setup((BOOLEAN)External,TRUE);FailureCount=2;Enumerate((BOOLEAN)External);
    assert(!Enumerated&&Bus.Devices[1]==&Old&&Bus.Devices[2]==&Descendant&&!Old.Connected&&!Descendant.Connected&&Old.DisconnectFail&&Descendant.DisconnectFail&&Hub.PortReenumerationPending[0]);
    // A hub retry works even after ChangeMap was freed; root callbacks always
    // read the port. No extra change or physical reconnect is needed.
    Enumerate((BOOLEAN)External);assert(!Enumerated&&Hub.PortReenumerationPending[0]);
    Enumerate((BOOLEAN)External);assert(Enumerated==1&&AdmissionClosed&&Freed==2&&Bus.Devices[1]==&Replacement&&!Bus.Devices[2]&&!Hub.PortReenumerationPending[0]);
    Setup((BOOLEAN)External,FALSE);FailureCount=1;Enumerate((BOOLEAN)External);
    ReadFailure=EFI_TIMEOUT;Enumerate((BOOLEAN)External);assert(!Enumerated&&Hub.PortReenumerationPending[0]&&Bus.Devices[1]==&Old);
    ReadFailure=EFI_DEVICE_ERROR;Enumerate((BOOLEAN)External);assert(!Enumerated&&Hub.PortReenumerationPending[0]&&Bus.Devices[1]==&Old);
    ReadFailure=0;Physical.PortStatus=0;Enumerate((BOOLEAN)External);assert(!Enumerated&&Freed==1&&!Bus.Devices[1]&&!Hub.PortReenumerationPending[0]);
    Setup((BOOLEAN)External,FALSE);FailureCount=1;Enumerate((BOOLEAN)External);
    EnumerationFailure=EFI_DEVICE_ERROR;Enumerate((BOOLEAN)External);assert(Enumerated==1&&Hub.PortReenumerationPending[0]&&!Bus.Devices[1]);
    EnumerationFailure=0;Enumerate((BOOLEAN)External);assert(Enumerated==2&&!Hub.PortReenumerationPending[0]&&AdmissionClosed);
    Setup((BOOLEAN)External,FALSE);Physical.PortChangeStatus=0;
    if(External){UINT8 Data=0;assert(UsbOnHubInterrupt(&Data,1,&Hub,0)==EFI_SUCCESS);assert(!Timer.Armed);}
    else Enumerate(FALSE);
    assert(!Enumerated&&!Removals&&Bus.Devices[1]==&Old&&Old.Connected);
    // A first enumeration failure consumes change bits too. Three attempts
    // are finite, and our own C_RESET must not renew the reset budget.
    Setup((BOOLEAN)External,FALSE);Bus.Devices[1]=NULL;EnumerationFailure=EFI_DEVICE_ERROR;
    Enumerate((BOOLEAN)External);assert(Enumerated==1&&Hub.PortReenumerationPending[0]);
    Enumerate((BOOLEAN)External);assert(Enumerated==2&&Hub.PortReenumerationPending[0]);
    Enumerate((BOOLEAN)External);assert(Enumerated==3&&!Hub.PortReenumerationPending[0]);
    if(External)assert(!Timer.Armed);
    Physical.PortChangeStatus=USB_PORT_STAT_C_RESET;Enumerate((BOOLEAN)External);assert(Enumerated==3);
    EnumerationFailure=0;Physical.PortChangeStatus=USB_PORT_STAT_C_CONNECTION;
    Enumerate((BOOLEAN)External);assert(Enumerated==4&&!Hub.PortEnumerationFailures[0]&&!Hub.PortReenumerationPending[0]);
    // Production ON_ERROR retains its assigned address and partial child.
    // Retirement must continue after the reset budget is exhausted.
    for(UINTN FailedDisconnect=0;FailedDisconnect<2;FailedDisconnect++){
      Setup((BOOLEAN)External,FALSE);Bus.Devices[1]=NULL;PartialOnFailure=TRUE;PartialDisconnectFail=(BOOLEAN)FailedDisconnect;EnumerationFailure=EFI_DEVICE_ERROR;
      Enumerate((BOOLEAN)External);assert(Enumerated==1&&Bus.Devices[1]==&Replacement&&Hub.PortReenumerationPending[0]);
      Enumerate((BOOLEAN)External);assert(Enumerated==2&&Bus.Devices[1]==&Replacement&&Hub.PortReenumerationPending[0]);
      Enumerate((BOOLEAN)External);assert(Enumerated==3&&Bus.Devices[1]==&Replacement&&Hub.PortReenumerationPending[0]);
      FailureCount=2;Enumerate((BOOLEAN)External);assert(Enumerated==3&&Bus.Devices[1]==&Replacement&&Hub.PortReenumerationPending[0]&&Replacement.DisconnectFail);
      Enumerate((BOOLEAN)External);assert(Enumerated==3&&Bus.Devices[1]==&Replacement&&Hub.PortReenumerationPending[0]);
      Enumerate((BOOLEAN)External);assert(Enumerated==3&&!Bus.Devices[1]&&!Hub.PortReenumerationPending[0]);
      if(External)assert(!Timer.Armed);else Enumerate(FALSE);
      assert(Enumerated==3);EnumerationFailure=0;Physical.PortChangeStatus=USB_PORT_STAT_C_CONNECTION;
      Enumerate((BOOLEAN)External);assert(Enumerated==4&&Bus.Devices[1]==&Replacement&&!Hub.PortReenumerationPending[0]&&AdmissionClosed);
    }
#endif
  }
#ifndef TEST_BEFORE_RECONNECT_RETENTION
  // Interrupt -> actual SignalEvent -> callback -> explicitly armed timer.
  // Timer API failure leaves exact pending ownership but claims no dispatch.
  Setup(TRUE,FALSE);FailureCount=1;TimerFailure=EFI_DEVICE_ERROR;Enumerate(TRUE);
  assert(Hub.PortReenumerationPending[0]&&!Timer.Armed&&!Enumerated&&Bus.Devices[1]==&Old);
  TimerFailure=0;Physical.PortChangeStatus=USB_PORT_STAT_C_CONNECTION;Enumerate(TRUE);
  assert(Enumerated==1&&!Hub.PortReenumerationPending[0]);
  // Two IRQ bitmaps before CALLBACK dispatch coalesce without replacing/leaking.
  Setup(TRUE,FALSE);DeferSignal=TRUE;UINT8 Data=2;
  assert(UsbOnHubInterrupt(&Data,1,&Hub,0)==EFI_SUCCESS);UINT8 *Map=Hub.ChangeMap;
  Data=1;assert(UsbOnHubInterrupt(&Data,1,&Hub,0)==EFI_SUCCESS&&Hub.ChangeMap==Map&&Map[0]==3&&Signals==2);
  Timer.Signaled=FALSE;Timer.Notify(&Timer,Timer.Context);assert(!Hub.ChangeMap&&Enumerated==1&&!Timer.Armed);
  // Failed timer cancel/Close retain the event and interface; exact retry closes.
  Setup(TRUE,FALSE);FailureCount=1;Enumerate(TRUE);assert(Timer.Armed);
  TimerFailure=EFI_DEVICE_ERROR;assert(UsbHubRelease(&Hub)==EFI_DEVICE_ERROR&&Hub.IsHub&&!Timer.Closed);
  TimerFailure=0;CloseFailure=EFI_DEVICE_ERROR;assert(UsbHubRelease(&Hub)==EFI_DEVICE_ERROR&&Hub.IsHub&&!Timer.Closed&&!Timer.Armed);
  CloseFailure=0;assert(UsbHubRelease(&Hub)==EFI_SUCCESS&&Timer.Closed&&!Hub.IsHub&&Closes==1&&AlreadyCancelled==2);
  // Parent retirement closes admission before the queued timer can perform IO.
  Setup(TRUE,FALSE);FailureCount=1;Enumerate(TRUE);Root.Connected=FALSE;UINTN PriorReads=Reads;
  FireTimer();assert(Reads==PriorReads&&!Timer.Armed);
  assert(UsbHubRelease(&Hub)==EFI_SUCCESS);
  // Init queue failure explicitly cancels the timer before closing its event.
  Setup(FALSE,FALSE);SubmitFailure=TRUE;
  assert(UsbHubInit(&Hub)==EFI_DEVICE_ERROR&&TimerCancels==1&&Closes==1&&Timer.Closed&&!Hub.HubNotify);
#endif
#ifdef TEST_BEFORE_RECONNECT_RETENTION
  puts("Before: failed removal permits replacement ownership; descendant failure leaves parent callable PASS");
#else
  puts("USB bus actual root/hub enumeration: retained subtree, fresh-status reconnect retry, no foreign replacement and unchanged healthy port PASS");
#endif
  return 0;
}
