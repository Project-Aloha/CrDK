/* Actual OSKAL with an explicit GIC/UEFI event boundary model. */
#pragma once
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#ifndef NULL
#define NULL ((VOID *)0)
#endif
#include "../Library/OskInterruptLib/oskinterrupt.c"
#include "../Driver/MDSSCrDxe/MdpNativeEvents.h"
EFI_GUID gHardwareInterrupt2ProtocolGuid;
EFI_BOOT_SERVICES *gBS;
static EFI_TPL EventTpl;
static HARDWARE_INTERRUPT_HANDLER EventHardware[1020];
static EFI_EVENT_NOTIFY EventNotify;
static VOID *EventContext;
static EFI_EVENT EventToken;
static BOOLEAN EventPending,EventClosed,EventEnabled,EventDropEnable;
static UINT32 EventIrq,EventEois,EventServices,EventEnables,EventDisables,EventCloses,EventUnregisters;
static EFI_STATUS EventFailCreate,EventFailSignal,EventFailClose,EventFailDisable,EventFailUnregister;
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;(VOID)Format;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){(VOID)Level;return FALSE;}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return EventTpl;}
static EFI_TPL EFIAPI EventRaise(EFI_TPL T){EFI_TPL Old=EventTpl;assert(T>=Old);EventTpl=T;return Old;}
static VOID EFIAPI EventRestore(EFI_TPL T){assert(T<=EventTpl);EventTpl=T;}
static EFI_STATUS EFIAPI EventCreate(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Notify,VOID *C,EFI_EVENT *E){
  assert(EventTpl<=TPL_CALLBACK&&Type==EVT_NOTIFY_SIGNAL&&T==TPL_CALLBACK);
  if(EventFailCreate)return EventFailCreate;
  EventNotify=Notify;EventContext=C;EventToken=C;*E=EventToken;EventClosed=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventSignal(EFI_EVENT E){
  assert(EventTpl==TPL_HIGH_LEVEL&&E==EventToken&&!EventClosed);EventPending=TRUE;return EventFailSignal;
}
static EFI_STATUS EFIAPI EventClose(EFI_EVENT E){
  assert(E==EventToken&&!EventPending);++EventCloses;
  if(EventFailClose)return EventFailClose;EventClosed=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventRegister(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,UINTN N,HARDWARE_INTERRUPT_HANDLER H){
  (VOID)P;assert(N==EventIrq&&N<ARRAY_SIZE(EventHardware)&&EventTpl==TPL_HIGH_LEVEL);
  if(!H){++EventUnregisters;if(EventFailUnregister)return EventFailUnregister;}
  EventHardware[N]=H;EventEnabled=H!=NULL;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventEnable(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,UINTN N){
  (VOID)P;assert(N==EventIrq&&EventTpl==TPL_HIGH_LEVEL);++EventEnables;if(!EventDropEnable)EventEnabled=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventDisable(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,UINTN N){
  (VOID)P;assert(N==EventIrq&&EventTpl==TPL_HIGH_LEVEL);++EventDisables;if(EventFailDisable)return EventFailDisable;EventEnabled=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventState(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,UINTN N,BOOLEAN *V){(VOID)P;assert(N==EventIrq);*V=EventEnabled;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI EventEoi(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,UINTN N){(VOID)P;assert(N==EventIrq&&EventTpl==TPL_HIGH_LEVEL);++EventEois;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI EventTrigger(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,UINTN N,EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE T){
  (VOID)P;assert(N==EventIrq&&T==EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_HIGH);return EFI_SUCCESS;
}
static EFI_HARDWARE_INTERRUPT2_PROTOCOL EventGic={.RegisterInterruptSource=EventRegister,.EnableInterruptSource=EventEnable,.DisableInterruptSource=EventDisable,
 .GetInterruptSourceState=EventState,.EndOfInterrupt=EventEoi,.SetTriggerType=EventTrigger};
static EFI_STATUS EFIAPI EventLocate(EFI_GUID *Guid,VOID *Registration,VOID **Out){
  (VOID)Guid;(VOID)Registration;assert(EventTpl<=TPL_NOTIFY);*Out=&EventGic;return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES EventBs={.RaiseTPL=EventRaise,.RestoreTPL=EventRestore,.CreateEvent=EventCreate,.SignalEvent=EventSignal,.CloseEvent=EventClose,.LocateProtocol=EventLocate};
static void EventSetup(UINT32 Irq){
  memset(mCrIntTable,0,sizeof(mCrIntTable));memset(mCrIntLifetime,0,sizeof(mCrIntLifetime));
  memset(EventHardware,0,sizeof(EventHardware));mHwInterrupt2=NULL;mDispatchDepth=0;mDispatchEntry=NULL;
  gBS=&EventBs;EventIrq=Irq;EventTpl=TPL_APPLICATION;EventNotify=NULL;EventContext=NULL;EventToken=NULL;
  EventPending=EventClosed=EventEnabled=EventDropEnable=FALSE;
  EventEois=EventServices=EventEnables=EventDisables=EventCloses=EventUnregisters=0;
  EventFailCreate=EventFailSignal=EventFailClose=EventFailDisable=EventFailUnregister=EFI_SUCCESS;
  EventGic.GetInterruptSourceState=EventState;
}
static void EventFire(void){EFI_SYSTEM_CONTEXT X={0};assert(EventEnabled&&EventHardware[EventIrq]);EventHardware[EventIrq](EventIrq,X);assert(EventPending&&!EventEnabled);}
static void EventPump(void){assert(EventPending&&!EventClosed);EventPending=FALSE;EFI_TPL Old=EventTpl;EventTpl=TPL_CALLBACK;EventNotify(EventToken,EventContext);EventTpl=Old;}
