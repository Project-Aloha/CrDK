/* Actual HID lifecycle/report functions with adversarial EFI boot services.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "HidKbDxe.h"
#include "HidKeyboard.h"
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gHidKeyboardProtocolGuid,gEfiDevicePathProtocolGuid,gEfiSimpleTextInProtocolGuid,gEfiSimpleTextInputExProtocolGuid;
EFI_COMPONENT_NAME_PROTOCOL mHidKeyboardComponentName={.SupportedLanguages="eng"};
EFI_COMPONENT_NAME2_PROTOCOL mHidKeyboardComponentName2={.SupportedLanguages="en"};
EFI_DRIVER_BINDING_PROTOCOL mHidKeyboardDriverBinding={.DriverBindingHandle=(EFI_HANDLE)0x41};
static EFI_HANDLE Controller=(EFI_HANDLE)0x17;
static HID_KEYBOARD_PROTOCOL Protocol;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *Published;
static KEYBOARD_HID_REPORT_CALLBACK ReportCallback;
static VOID *ReportContext;
static BOOLEAN Opened,NestedLed,NestedUninstall;
static EFI_TPL Tpl=TPL_APPLICATION;
static UINTN Pools,Allocations,FailAllocation,Creates,FailCreate,Closes,FailClose,NotifyCalls,OtherNotifyCalls,OutputCalls;
static EFI_STATUS UnregisterStatus,UninstallStatus,CloseProtocolStatus,RegisterStatus,LayoutStatus,NestedStopStatus;
#ifdef CR_HID_LIFETIME_FIXED
static VOID *NotifySelf,*NotifyOther;
static HID_KB_DEV *NotifyDevice;
#endif
typedef struct {EFI_EVENT_NOTIFY Fn;VOID *Context;EFI_TPL Tpl;BOOLEAN Closed,Pending;} EVENT;
static EVENT *Events[256];static UINTN EventCount;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI AllocatePool(UINTN N){if(++Allocations==FailAllocation)return NULL;VOID *P=malloc(N);if(P)++Pools;return P;}
VOID *EFIAPI AllocateZeroPool(UINTN N){VOID *P=AllocatePool(N);if(P)memset(P,0,N);return P;}
VOID *EFIAPI AllocateCopyPool(UINTN N,CONST VOID *S){VOID *P=AllocatePool(N);if(P)memcpy(P,S,N);return P;}
VOID EFIAPI FreePool(VOID *P){assert(P&&Pools);--Pools;free(P);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){ }
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *P){P->ForwardLink=P->BackLink=P;return P;}
LIST_ENTRY *EFIAPI GetFirstNode(CONST LIST_ENTRY *H){return H->ForwardLink;}
LIST_ENTRY *EFIAPI GetNextNode(CONST LIST_ENTRY *H,CONST LIST_ENTRY *P){return P->ForwardLink;}
BOOLEAN EFIAPI IsNull(CONST LIST_ENTRY *H,CONST LIST_ENTRY *P){return H==P;}
BOOLEAN EFIAPI IsListEmpty(CONST LIST_ENTRY *H){return H->ForwardLink==H;}
LIST_ENTRY *EFIAPI InsertTailList(LIST_ENTRY *H,LIST_ENTRY *P){P->BackLink=H->BackLink;P->ForwardLink=H;H->BackLink->ForwardLink=P;H->BackLink=P;return H;}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *P){P->BackLink->ForwardLink=P->ForwardLink;P->ForwardLink->BackLink=P->BackLink;return P->ForwardLink;}
EFI_STATUS EFIAPI AddUnicodeString2(CONST CHAR8 *L,CONST CHAR8 *S,EFI_UNICODE_STRING_TABLE **T,CONST CHAR16 *V,BOOLEAN I){return EFI_SUCCESS;}
EFI_STATUS EFIAPI FreeUnicodeStringTable(EFI_UNICODE_STRING_TABLE *T){assert(!T);return EFI_SUCCESS;}
static EFI_TPL EFIAPI Raise(EFI_TPL N){EFI_TPL O=Tpl;assert(N>=O);Tpl=N;return O;}
static VOID EFIAPI Restore(EFI_TPL N){assert(N<=Tpl);Tpl=N;
 for(UINTN I=0;I<EventCount;++I){EVENT *E=Events[I];if(!E->Closed&&E->Pending&&Tpl<E->Tpl){E->Pending=FALSE;EFI_TPL Old=Tpl;Tpl=E->Tpl;E->Fn(E,E->Context);Tpl=Old;}}
}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Fn,VOID *C,EFI_EVENT *Out){
 if(++Creates==FailCreate)return EFI_OUT_OF_RESOURCES;assert(EventCount<256);EVENT *E=calloc(1,sizeof(*E));assert(E);*E=(EVENT){Fn,C,T,FALSE,FALSE};Events[EventCount++]=E;*Out=E;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI CreateEx(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){return Create(Type,T,Fn,(VOID *)C,Out);}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT V){EVENT *E=V;assert(E&&!E->Closed);if(++Closes==FailClose)return EFI_DEVICE_ERROR;E->Closed=TRUE;E->Pending=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Signal(EFI_EVENT V){EVENT *E=V;assert(E&&!E->Closed);E->Pending=TRUE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Timer(EFI_EVENT V,EFI_TIMER_DELAY D,UINT64 T){assert(V&&!((EVENT *)V)->Closed);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **I,EFI_HANDLE A,EFI_HANDLE C,UINT32 F){
 assert(H==Controller);if(G==&gEfiDevicePathProtocolGuid){*I=(VOID *)0x90;return EFI_SUCCESS;}
 if(G==&gHidKeyboardProtocolGuid){assert(!Opened);Opened=TRUE;*I=&Protocol;return EFI_SUCCESS;}
 if(G==&gEfiSimpleTextInProtocolGuid&&Published){*I=Published;return EFI_SUCCESS;}
 if(G==&gEfiSimpleTextInputExProtocolGuid&&Published)return EFI_SUCCESS;return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI CloseProtocol(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE A,EFI_HANDLE C){assert(Opened);if(EFI_ERROR(CloseProtocolStatus))return CloseProtocolStatus;Opened=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){va_list A;va_start(A,H);assert(va_arg(A,EFI_GUID *)==&gEfiSimpleTextInProtocolGuid);Published=va_arg(A,EFI_SIMPLE_TEXT_INPUT_PROTOCOL *);va_end(A);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...){if(EFI_ERROR(UninstallStatus))return UninstallStatus;assert(Published);
 if(NestedUninstall){NestedUninstall=FALSE;NestedStopStatus=HIDKeyboardDriverBindingStop(&mHidKeyboardDriverBinding,Controller,0,NULL);}
 Published=NULL;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Register(HID_KEYBOARD_PROTOCOL *P,KEYBOARD_HID_REPORT_CALLBACK Fn,VOID *C){if(EFI_ERROR(RegisterStatus))return RegisterStatus;assert(!ReportCallback);ReportCallback=Fn;ReportContext=C;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Unregister(HID_KEYBOARD_PROTOCOL *P){if(UnregisterStatus!=EFI_SUCCESS)return UnregisterStatus;ReportCallback=NULL;ReportContext=NULL;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Output(HID_KEYBOARD_PROTOCOL *P,KEYBOARD_HID_INTERFACE I,UINT8 *B,UINTN N){++OutputCalls;
 if(NestedLed){NestedLed=FALSE;EFI_TPL Old=Tpl;Tpl=TPL_CALLBACK;NestedStopStatus=HIDKeyboardDriverBindingStop(&mHidKeyboardDriverBinding,Controller,0,NULL);Tpl=Old;}
 return EFI_SUCCESS;}
static VOID EFIAPI LayoutEvent(EFI_EVENT E,VOID *C){(VOID)E;(VOID)C;}
EFI_STATUS InitKeyboardLayout(HID_KB_DEV *D){D->KeyConvertionTable=AllocateZeroPool(NUMBER_OF_VALID_HID_KEYCODE*sizeof(EFI_KEY_DESCRIPTOR));
 if(!D->KeyConvertionTable)return EFI_OUT_OF_RESOURCES;D->KeyConvertionTable[0x39-4].Modifier=EFI_CAPS_LOCK_MODIFIER;
 EFI_STATUS S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,LayoutEvent,D,NULL,&D->KeyboardLayoutEvent);return EFI_ERROR(S)?S:LayoutStatus;}
EFI_STATUS HIDKeyCodeToEfiInputKey(HID_KB_DEV *D,UINT8 Code,EFI_KEY_DATA *K){return EFI_NOT_READY;}
#include "HidLifetimeProduction.c"
static EFI_STATUS Start(VOID){return HIDKeyboardDriverBindingStart(&mHidKeyboardDriverBinding,Controller,NULL);}
static EFI_STATUS Stop(VOID){return HIDKeyboardDriverBindingStop(&mHidKeyboardDriverBinding,Controller,0,NULL);}
static HID_KB_DEV *Device(VOID){assert(Published);return HID_KB_DEV_FROM_THIS(Published);}
static VOID Reset(VOID){assert(!Pools&&!Opened&&!Published&&!ReportCallback);for(UINTN I=0;I<EventCount;++I){assert(Events[I]->Closed);free(Events[I]);}EventCount=0;
 Tpl=TPL_APPLICATION;Allocations=FailAllocation=Creates=FailCreate=Closes=FailClose=NotifyCalls=OtherNotifyCalls=OutputCalls=0;
 UnregisterStatus=UninstallStatus=CloseProtocolStatus=RegisterStatus=LayoutStatus=EFI_SUCCESS;NestedLed=NestedUninstall=FALSE;
 Services=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.CreateEvent=Create,.CreateEventEx=CreateEx,.CloseEvent=CloseEvent,.SignalEvent=Signal,.SetTimer=Timer,
 .OpenProtocol=Open,.CloseProtocol=CloseProtocol,.InstallMultipleProtocolInterfaces=Install,.UninstallMultipleProtocolInterfaces=Uninstall};
 Protocol=(HID_KEYBOARD_PROTOCOL){.RegisterKeyboardHidReportCallback=Register,.UnRegisterKeyboardHidReportCallback=Unregister,.SetOutputReport=Output};}
static VOID Queued(VOID){Reset();assert(Start()==EFI_SUCCESS);HID_KB_DEV *D=Device();EFI_KEY_DATA K={0};K.Key.UnicodeChar='a';Enqueue(&D->EfiKeyQueueForNotify,&K,sizeof(K));
 EFI_TPL Old=Raise(TPL_NOTIFY);assert(Signal(D->KeyNotifyProcessEvent)==EFI_SUCCESS);assert(Stop()==EFI_SUCCESS);Restore(Old);assert(!Pools&&!Published&&!Opened&&!ReportCallback);Reset();}
static VOID Report(VOID){Reset();assert(Start()==EFI_SUCCESS);UINT8 Input[8]={0,0,0x39};NestedLed=TRUE;
 ReportCallback(BootKeyboard,Input,sizeof(Input),ReportContext);
#ifdef CR_HID_LIFETIME_FIXED
 assert(NestedStopStatus==EFI_NOT_READY&&Published&&Opened&&ReportCallback&&Device()->Stopping&&Device()->ActiveCallbacks==0);
 assert(Stop()==EFI_SUCCESS);Reset();
#endif
}
static VOID UninstallReentry(VOID){Reset();assert(Start()==EFI_SUCCESS);NestedUninstall=TRUE;assert(Stop()==EFI_SUCCESS);assert(NestedStopStatus==EFI_NOT_READY);Reset();}
static VOID UninstallFailure(VOID){Reset();assert(Start()==EFI_SUCCESS);HID_KB_DEV *D=Device();UINTN P=Pools;UninstallStatus=EFI_ACCESS_DENIED;
 assert(Stop()==EFI_ACCESS_DENIED);assert(Published==&D->SimpleInput&&Pools==P&&Opened);UninstallStatus=EFI_SUCCESS;assert(Stop()==EFI_SUCCESS);Reset();}
#ifdef CR_HID_LIFETIME_FIXED
static VOID PartialStart(VOID){for(UINTN I=1;I<=5;++I){Reset();FailAllocation=I;EFI_STATUS S=Start();assert(S==(I<=2?EFI_OUT_OF_RESOURCES:EFI_DEVICE_ERROR)&&Tpl==TPL_APPLICATION&&!Pools&&!Opened&&!Published&&!ReportCallback);}
 for(UINTN I=1;I<=5;++I){Reset();FailCreate=I;EFI_STATUS S=Start();assert(S==(I<=4?EFI_OUT_OF_RESOURCES:EFI_DEVICE_ERROR)&&Tpl==TPL_APPLICATION&&!Pools&&!Opened&&!Published&&!ReportCallback);}
 Reset();RegisterStatus=EFI_ALREADY_STARTED;assert(Start()==EFI_ALREADY_STARTED&&!Pools&&!Opened&&!Published&&!ReportCallback);
 Reset();LayoutStatus=EFI_NOT_READY;FailClose=1;assert(Start()==EFI_NOT_READY&&Pools&&Opened&&!Published);assert(Start()==EFI_NOT_READY);FailClose=0;assert(Stop()==EFI_SUCCESS);Reset();
 Reset();NestedLed=TRUE;assert(Start()==EFI_DEVICE_ERROR&&NestedStopStatus==EFI_NOT_READY&&!Pools&&!Opened&&!Published);Reset();}
static VOID RetainedFailures(VOID){Reset();assert(Start()==EFI_SUCCESS);UINTN P=Pools;UnregisterStatus=EFI_NOT_READY;
 assert(Stop()==EFI_NOT_READY&&Published&&Opened&&ReportCallback&&Pools==P);UINT8 B[8]={0,0,0x39};ReportCallback(BootKeyboard,B,sizeof(B),ReportContext);assert(Pools==P&&OutputCalls==1);
 EFI_KEY_DATA K={0};VOID *Handle=NULL;
 assert(HIDKeyboardReadKeyStrokeEx(&Device()->SimpleInputEx,&K)==EFI_NOT_READY);
 assert(HIDKeyboardRegisterKeyNotify(&Device()->SimpleInputEx,&K,(EFI_KEY_NOTIFY_FUNCTION)(UINTN)1,&Handle)==EFI_NOT_READY);
 UnregisterStatus=EFI_SUCCESS;assert(Stop()==EFI_SUCCESS);Reset();
 for(UINTN I=1;I<=5;++I){Reset();assert(Start()==EFI_SUCCESS);FailClose=I;assert(Stop()==EFI_DEVICE_ERROR&&Pools&&!Published&&!ReportCallback&&Opened);FailClose=0;assert(Stop()==EFI_SUCCESS);Reset();}
 Reset();assert(Start()==EFI_SUCCESS);CloseProtocolStatus=EFI_DEVICE_ERROR;assert(Stop()==EFI_DEVICE_ERROR&&Pools&&!Published&&!ReportCallback&&Opened);CloseProtocolStatus=EFI_SUCCESS;assert(Stop()==EFI_SUCCESS);Reset();}
static EFI_STATUS EFIAPI Other(EFI_KEY_DATA *K){++OtherNotifyCalls;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Notify(EFI_KEY_DATA *K){++NotifyCalls;assert(HIDKeyboardReset(&NotifyDevice->SimpleInput,TRUE)==EFI_NOT_READY);
 assert(HIDKeyboardUnregisterKeyNotify(&NotifyDevice->SimpleInputEx,NotifySelf)==EFI_SUCCESS);
 assert(HIDKeyboardUnregisterKeyNotify(&NotifyDevice->SimpleInputEx,NotifyOther)==EFI_SUCCESS);
 assert(Stop()==EFI_NOT_READY&&Published&&NotifyDevice->ActiveCallbacks);return EFI_SUCCESS;}
static VOID ReentrantNotify(VOID){Reset();assert(Start()==EFI_SUCCESS);NotifyDevice=Device();EFI_KEY_DATA K={0};K.Key.UnicodeChar='a';
 assert(HIDKeyboardRegisterKeyNotify(&NotifyDevice->SimpleInputEx,&K,Notify,&NotifySelf)==EFI_SUCCESS);
 assert(HIDKeyboardRegisterKeyNotify(&NotifyDevice->SimpleInputEx,&K,Other,&NotifyOther)==EFI_SUCCESS);
 Enqueue(&NotifyDevice->EfiKeyQueueForNotify,&K,sizeof(K));Tpl=TPL_CALLBACK;KeyNotifyProcessHandler(NotifyDevice->KeyNotifyProcessEvent,NotifyDevice);Tpl=TPL_APPLICATION;
 assert(NotifyCalls==1&&!OtherNotifyCalls&&IsListEmpty(&NotifyDevice->NotifyList)&&!NotifyDevice->ActiveCallbacks&&!NotifyDevice->NotifyDispatchDepth);
 assert(Stop()==EFI_SUCCESS);Reset();}
static VOID QueueAllocation(VOID){Reset();HID_SIMPLE_QUEUE Q={0};assert(InitQueue(&Q,sizeof(EFI_KEY_DATA))==EFI_SUCCESS);EFI_KEY_DATA K={0};K.Key.UnicodeChar='a';Enqueue(&Q,&K,sizeof(K));
 HID_SIMPLE_QUEUE Saved=Q;FailAllocation=Allocations+1;assert(InitQueue(&Q,sizeof(EFI_KEY_DATA))==EFI_OUT_OF_RESOURCES&&!memcmp(&Q,&Saved,sizeof(Q)));
 assert(Dequeue(&Q,&K,sizeof(K))==EFI_SUCCESS&&K.Key.UnicodeChar=='a');DestroyQueue(&Q);DestroyQueue(&Q);Reset();}
#endif
int main(int Argc,char **Argv){const char *Mode=Argc>1?Argv[1]:"all";
 if(!strcmp(Mode,"queued")){Queued();return 0;}if(!strcmp(Mode,"report")){Report();return 0;}if(!strcmp(Mode,"uninstall")){UninstallFailure();return 0;}if(!strcmp(Mode,"uninstall-reentry")){UninstallReentry();return 0;}
 Queued();Report();UninstallFailure();
#ifdef CR_HID_LIFETIME_FIXED
 UninstallReentry();
#endif
#ifdef CR_HID_LIFETIME_FIXED
 PartialStart();RetainedFailures();ReentrantNotify();QueueAllocation();
#endif
 puts("PASS actual HID queues/report/LED and Start/Stop callback lifetime faults");return 0;}
