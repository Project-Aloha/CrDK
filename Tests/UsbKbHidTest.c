/* SPDX-License-Identifier: MIT
 * Actual provider and USB request functions; firmware boundaries are injected.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "UsbKbHidDxe.h"
#undef PcdGet32
#define PcdGet32(Token) 1000U
#undef REPORT_STATUS_CODE
#define REPORT_STATUS_CODE(...) ((VOID)0)
#undef REPORT_STATUS_CODE_WITH_DEVICE_PATH
#define REPORT_STATUS_CODE_WITH_DEVICE_PATH(...) ((VOID)mStatusCodes++)
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiUsbIoProtocolGuid, gHidKeyboardProtocolGuid, gEfiDevicePathProtocolGuid;
EFI_COMPONENT_NAME_PROTOCOL gUsbKbHidComponentName;
EFI_COMPONENT_NAME2_PROTOCOL gUsbKbHidComponentName2;
STATIC EFI_USB_IO_PROTOCOL mUsb;
EFI_DRIVER_BINDING_PROTOCOL gUsbKbHidDriverBinding;
STATIC USB_KB_HID_DEV *mDevice;
STATIC EFI_TPL mTpl;
STATIC EFI_STATUS mProtocolStatus, mIdleStatus, mEventStatus, mCancelStatus, mUninstallStatus, mInstallStatus, mCloseStatus, mCloseEventStatus, mDescriptorStatus, mSubmitStatus;
STATIC UINTN mControls, mIdles, mProtocols, mEvents, mFreed, mCancels, mSubmits, mUninstalls, mCloses, mReports, mTimers;
STATIC BOOLEAN mNested, mCleanupNested;
STATIC BOOLEAN mOpened, mAllocateFail;
STATIC UINTN mPacketSize, mStatusCodes;
LIST_ENTRY *EFIAPI GetFirstNode(CONST LIST_ENTRY *L) {return L->ForwardLink;}
LIST_ENTRY *EFIAPI GetNextNode(CONST LIST_ENTRY *L,CONST LIST_ENTRY *N) {(VOID)L;return N->ForwardLink;}
BOOLEAN EFIAPI IsNull(CONST LIST_ENTRY *L,CONST LIST_ENTRY *N) {return L==N;}
LIST_ENTRY *EFIAPI InsertTailList(LIST_ENTRY *L,LIST_ENTRY *N) {N->ForwardLink=L;N->BackLink=L->BackLink;L->BackLink->ForwardLink=N;L->BackLink=N;return L;}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *N) {N->BackLink->ForwardLink=N->ForwardLink;N->ForwardLink->BackLink=N->BackLink;return N->ForwardLink;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) {return memcpy(D,S,N);}
VOID *EFIAPI AllocateZeroPool(UINTN N) {assert(!mDevice && N==sizeof(*mDevice));if(mAllocateFail)return NULL;mDevice=calloc(1,N);return mDevice;}
EFI_STATUS EFIAPI AddUnicodeString2(CONST CHAR8 *L,CONST CHAR8 *S,EFI_UNICODE_STRING_TABLE **T,CONST CHAR16 *V,BOOLEAN I) {(VOID)L;(VOID)S;(VOID)T;(VOID)V;(VOID)I;return EFI_SUCCESS;}
VOID *EFIAPI ZeroMem(VOID *P, UINTN N) {return memset(P,0,N);}
VOID EFIAPI FreePool(VOID *P) {assert(P==mDevice);mFreed++;free(P);mDevice=NULL;}
EFI_STATUS EFIAPI FreeUnicodeStringTable(EFI_UNICODE_STRING_TABLE *P) {(VOID)P;return EFI_SUCCESS;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID) {return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F, UINTN L, CONST CHAR8 *D) {fprintf(stderr,"%s:%zu %s\n",F,(size_t)L,D);abort();}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L) {(VOID)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) {(VOID)L;(VOID)F;}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL T) {EFI_TPL O=mTpl;assert(T>=O);mTpl=T;return O;}
STATIC VOID EFIAPI Restore(EFI_TPL T) {assert(T<=mTpl);mTpl=T;}
STATIC EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **P,EFI_HANDLE A,EFI_HANDLE C,UINT32 F) {
 (VOID)H;(VOID)A;(VOID)C;
 if(G==&gEfiUsbIoProtocolGuid) {assert(F==EFI_OPEN_PROTOCOL_BY_DRIVER && !mOpened);mOpened=TRUE;*P=&mUsb;return EFI_SUCCESS;}
 if(G==&gEfiDevicePathProtocolGuid) {*P=(VOID*)0x4560;return EFI_SUCCESS;}
 assert(G==&gHidKeyboardProtocolGuid && F==EFI_OPEN_PROTOCOL_GET_PROTOCOL && mDevice);*P=&mDevice->HidKeyboard;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...) {(VOID)H;mUninstalls++;if(mCleanupNested)assert(UsbKbHidDriverBindingStop(&gUsbKbHidDriverBinding,(VOID*)1,0,NULL)==EFI_NOT_READY);return mUninstallStatus;}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...) {(VOID)H;return mInstallStatus;}
STATIC EFI_STATUS EFIAPI Descriptor(EFI_USB_IO_PROTOCOL *U,EFI_USB_INTERFACE_DESCRIPTOR *D) {(VOID)U;*D=mDevice->InterfaceDescriptor;D->NumEndpoints=1;D->InterfaceNumber=3;return mDescriptorStatus;}
STATIC EFI_STATUS EFIAPI Endpoint(EFI_USB_IO_PROTOCOL *U,UINT8 I,EFI_USB_ENDPOINT_DESCRIPTOR *D) {(VOID)U;(VOID)I;memset(D,0,sizeof(*D));D->Attributes=USB_ENDPOINT_INTERRUPT;D->EndpointAddress=0x81;D->Interval=10;D->MaxPacketSize=64;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI CloseProtocol(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE A,EFI_HANDLE C) {(VOID)H;(VOID)G;(VOID)A;(VOID)C;if(!EFI_ERROR(mCloseStatus))mOpened=FALSE;return mCloseStatus;}
STATIC EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E) {assert(E);mCloses++;return mCloseEventStatus;}
STATIC EFI_STATUS EFIAPI CreateEvent(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY F,VOID *C,EFI_EVENT *E) {
 assert(T==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && P==TPL_NOTIFY && F==UsbKbHidRecoveryHandler && C==mDevice);mEvents++;
 if(!EFI_ERROR(mEventStatus))*E=(VOID*)0x1230;return mEventStatus;
}
STATIC EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY D,UINT64 T) {assert(E);(VOID)T;mTimers++;assert(D==TimerCancel||D==TimerRelative);return EFI_SUCCESS;}
STATIC VOID TryDisconnect(VOID) {
 assert(UsbKbHidDriverBindingStop(&gUsbKbHidDriverBinding,(VOID*)1,0,NULL)==EFI_NOT_READY);
 assert(UnRegisterKeyboardHidReportCallback(&mDevice->HidKeyboard)==EFI_NOT_READY);
 assert(!mFreed && !mUninstalls && !mCancels && !mCloses);
}
STATIC EFI_STATUS EFIAPI Control(EFI_USB_IO_PROTOCOL *U,EFI_USB_DEVICE_REQUEST *R,EFI_USB_DATA_DIRECTION D,UINT32 T,VOID *B,UINTN N,UINT32 *S) {
 assert(U==mDevice->UsbIo && T==1000 && R->Index==3);mControls++;*S=EFI_USB_NOERROR;
 if(R->Request==EFI_USB_SET_REPORT_REQUEST) {
  assert(R->RequestType==0x21 && D==EfiUsbDataOut && R->Value==0x200 && R->Length==1 && N==1 && B);
  if(mNested)TryDisconnect();return EFI_SUCCESS;
 }
 if(R->Request==EFI_USB_SET_IDLE_REQUEST) {
  assert(mProtocols==1 && !mEvents && !mSubmits);
  assert(R->RequestType==0x21 && R->Value==0 && D==EfiUsbNoData && !B && !N && !R->Length);
  mIdles++;return mIdleStatus;
 }
 assert(R->Request==EFI_USB_SET_PROTOCOL_REQUEST && R->RequestType==0x21 && R->Value==0 && D==EfiUsbNoData && !N && !R->Length);
 mProtocols++;return mProtocolStatus;
}
STATIC EFI_STATUS EFIAPI Async(EFI_USB_IO_PROTOCOL *U,UINT8 Ep,BOOLEAN New,UINTN Interval,UINTN Size,EFI_ASYNC_USB_TRANSFER_CALLBACK F,VOID *C) {
 assert(U==mDevice->UsbIo && Ep==0x81);(VOID)Interval;
 if(New) {mSubmits++;mPacketSize=Size;assert(F==KeyboardHandler && C==mDevice);if(mNested)TryDisconnect();return mSubmitStatus;}
 mCancels++;assert(!Size && !F && !C);return mCancelStatus;
}
EFI_STATUS EFIAPI UsbGetConfiguration(EFI_USB_IO_PROTOCOL *U,UINT16 *V,UINT32 *S) {(VOID)U;*V=1;*S=0;return EFI_SUCCESS;}
EFI_STATUS EFIAPI UsbSetConfiguration(EFI_USB_IO_PROTOCOL *U,UINT16 V,UINT32 *S) {(VOID)U;(VOID)V;(VOID)S;abort();}
EFI_STATUS EFIAPI UsbClearEndpointHalt(EFI_USB_IO_PROTOCOL *U,UINT8 Ep,UINT32 *S) {(VOID)U;(VOID)Ep;*S=0;return EFI_SUCCESS;}
STATIC VOID EFIAPI Report(KEYBOARD_HID_INTERFACE I,UINT8 *B,UINTN N,VOID *C) {
 assert(I==BootKeyboard && B && N==8 && C==mDevice);mReports++;if(mNested)TryDisconnect();
 UINT8 Led=2;assert(SetOutputReport(&mDevice->HidKeyboard,BootKeyboard,&Led,1)==EFI_SUCCESS);
}
#include "UsbKbActual.inc"
STATIC VOID Reset(VOID) {
 assert(!mDevice);mDevice=calloc(1,sizeof(*mDevice));assert(mDevice);
 STATIC EFI_BOOT_SERVICES Boot={.RaiseTPL=Raise,.RestoreTPL=Restore,.OpenProtocol=Open,.UninstallMultipleProtocolInterfaces=Uninstall,.InstallMultipleProtocolInterfaces=Install,.CloseProtocol=CloseProtocol,.CloseEvent=CloseEvent,.CreateEvent=CreateEvent,.SetTimer=Timer};
 mUsb=(EFI_USB_IO_PROTOCOL){.UsbControlTransfer=Control,.UsbAsyncInterruptTransfer=Async,.UsbGetInterfaceDescriptor=Descriptor,.UsbGetEndpointDescriptor=Endpoint};
 gBS=&Boot;mDevice->Signature=USB_HID_KB_DEV_SIGNATURE;mDevice->UsbIo=&mUsb;
 mDevice->ControllerHandle=(VOID*)1; mDevice->AsyncActive=TRUE;mDevice->ProtocolInstalled=TRUE;mDevice->UsbIoOpened=TRUE;mOpened=TRUE;
 InsertTailList(&mUsbKbHidDevices,&mDevice->Link);
 mDevice->InterfaceDescriptor.InterfaceNumber=3;mDevice->IntEndpointDescriptor.EndpointAddress=0x81;
 mDevice->IntEndpointDescriptor.MaxPacketSize=64;mDevice->IntEndpointDescriptor.Interval=10;
 mProtocolStatus=mIdleStatus=mEventStatus=mCancelStatus=mUninstallStatus=mInstallStatus=mCloseStatus=mCloseEventStatus=mDescriptorStatus=mSubmitStatus=EFI_SUCCESS;mTpl=TPL_APPLICATION;
 mControls=mIdles=mProtocols=mEvents=mFreed=mCancels=mSubmits=mUninstalls=mCloses=mReports=mTimers=mStatusCodes=0;mNested=mCleanupNested=mAllocateFail=FALSE;
 assert(RegisterKeyboardHidReportCallback(&mDevice->HidKeyboard,Report,mDevice)==EFI_SUCCESS);
}
STATIC VOID Stop(VOID) {assert(UsbKbHidDriverBindingStop(&gUsbKbHidDriverBinding,(VOID*)1,0,NULL)==EFI_SUCCESS);assert(mFreed==1 && !mDevice && mTpl==TPL_APPLICATION);}
STATIC VOID StartFixture(VOID) {Reset();RemoveEntryList(&mDevice->Link);free(mDevice);mDevice=NULL;mOpened=FALSE;}
int main(void) {
 UINT8 Led=2,Keys[8]={0,0,4};
 Reset();assert(SetOutputReport(&mDevice->HidKeyboard,BootKeyboard,&Led,1)==EFI_SUCCESS);assert(mControls==1);Stop();
 Reset();mNested=TRUE;assert(SetOutputReport(&mDevice->HidKeyboard,BootKeyboard,&Led,1)==EFI_SUCCESS);assert(mDevice->ActiveCalls==0);mNested=FALSE;Stop();
 Reset();mNested=TRUE;assert(KeyboardHandler(Keys,8,mDevice,EFI_USB_NOERROR)==EFI_SUCCESS);assert(mReports==1 && mDevice->ActiveCalls==0);mNested=FALSE;Stop();
 Reset();mNested=TRUE;mDevice->DelayedRecoveryEvent=(VOID*)0x1230;assert(KeyboardHandler(NULL,0,mDevice,EFI_USB_ERR_TIMEOUT)==EFI_DEVICE_ERROR);assert(mReports==1 && mDevice->ActiveCalls==0 && mCancels==1 && mTimers==1);mNested=FALSE;Stop();
 Reset();assert(InitUsbKeyboard(mDevice)==EFI_SUCCESS);assert(mIdles==1 && mProtocols==1 && mEvents==1 && mControls==2);Stop();
 // SET_IDLE is a class compatibility request: rejected or timed-out requests
 // still permit polling, as in Linux, while protocol failures remain fatal.
 Reset();mIdleStatus=EFI_DEVICE_ERROR;assert(InitUsbKeyboard(mDevice)==EFI_SUCCESS);assert(mIdles==1 && mEvents==1);Stop();
 Reset();mIdleStatus=EFI_TIMEOUT;assert(InitUsbKeyboard(mDevice)==EFI_SUCCESS);assert(mIdles==1 && mEvents==1);Stop();
 Reset();mProtocolStatus=EFI_TIMEOUT;assert(InitUsbKeyboard(mDevice)==EFI_TIMEOUT && mEvents==0 && !mIdles);Stop();
 Reset();mEventStatus=EFI_OUT_OF_RESOURCES;assert(InitUsbKeyboard(mDevice)==EFI_OUT_OF_RESOURCES && mEvents==1 && mIdles==1);Stop();
 Reset();mNested=TRUE;mDevice->AsyncActive=FALSE;mDevice->IntEndpointDescriptor.MaxPacketSize=512;UsbKbHidRecoveryHandler(NULL,mDevice);assert(mSubmits==1 && mPacketSize==512 && mDevice->ActiveCalls==0);mNested=FALSE;Stop();
 Reset();mCancelStatus=EFI_DEVICE_ERROR;assert(UsbKbHidDriverBindingStop(&gUsbKbHidDriverBinding,(VOID*)1,0,NULL)==EFI_DEVICE_ERROR);assert(mDevice && !mFreed && !mUninstalls);mControls=0;assert(SetOutputReport(&mDevice->HidKeyboard,BootKeyboard,&Led,1)==EFI_NOT_READY && !mControls);UsbKbHidRecoveryHandler(NULL,mDevice);assert(!mSubmits);mCancelStatus=EFI_SUCCESS;Stop();
 Reset();mDevice->DelayedRecoveryEvent=(VOID*)0x1230;mCancelStatus=EFI_DEVICE_ERROR;assert(KeyboardHandler(NULL,0,mDevice,EFI_USB_ERR_TIMEOUT)==EFI_DEVICE_ERROR);assert(mDevice->AsyncActive && !mTimers);UsbKbHidRecoveryHandler(NULL,mDevice);assert(!mSubmits);mCancelStatus=EFI_SUCCESS;Stop();
 Reset();mUninstallStatus=EFI_ACCESS_DENIED;assert(UsbKbHidDriverBindingStop(&gUsbKbHidDriverBinding,(VOID*)1,0,NULL)==EFI_ACCESS_DENIED);assert(mDevice && !mFreed);mUninstallStatus=EFI_SUCCESS;Stop();
 Reset();assert(UnRegisterKeyboardHidReportCallback(&mDevice->HidKeyboard)==EFI_SUCCESS);assert(UnRegisterKeyboardHidReportCallback(&mDevice->HidKeyboard)==EFI_NOT_FOUND);Stop();
 Reset();mDevice->DelayedRecoveryEvent=(VOID*)0x1230;mCloseEventStatus=EFI_DEVICE_ERROR;assert(UsbKbHidDriverBindingStop(&gUsbKbHidDriverBinding,(VOID*)1,0,NULL)==EFI_DEVICE_ERROR);assert(mDevice && !mFreed && !mDevice->ProtocolInstalled);mCloseEventStatus=EFI_SUCCESS;Stop();
 Reset();mCloseStatus=EFI_DEVICE_ERROR;assert(UsbKbHidDriverBindingStop(&gUsbKbHidDriverBinding,(VOID*)1,0,NULL)==EFI_DEVICE_ERROR);assert(mDevice && !mFreed && mOpened);mCloseStatus=EFI_SUCCESS;Stop();
 Reset();mCleanupNested=TRUE;Stop();
 StartFixture();assert(UsbKbHidDriverBindingStart(&gUsbKbHidDriverBinding,(VOID*)1,NULL)==EFI_SUCCESS);assert(mDevice && mOpened && mDevice->ActiveCalls==0 && mIdles==1 && mProtocols==1 && mSubmits==1);Stop();
 StartFixture();mIdleStatus=EFI_DEVICE_ERROR;assert(UsbKbHidDriverBindingStart(&gUsbKbHidDriverBinding,(VOID*)1,NULL)==EFI_SUCCESS);assert(mIdles==1 && mSubmits==1);Stop();
 StartFixture();mIdleStatus=EFI_TIMEOUT;assert(UsbKbHidDriverBindingStart(&gUsbKbHidDriverBinding,(VOID*)1,NULL)==EFI_SUCCESS);assert(mIdles==1 && mSubmits==1);Stop();
 StartFixture();mDescriptorStatus=EFI_DEVICE_ERROR;assert(UsbKbHidDriverBindingStart(&gUsbKbHidDriverBinding,(VOID*)1,NULL)==EFI_DEVICE_ERROR);assert(!mDevice && !mOpened && !mEvents && !mSubmits);
 StartFixture();mInstallStatus=EFI_OUT_OF_RESOURCES;assert(UsbKbHidDriverBindingStart(&gUsbKbHidDriverBinding,(VOID*)1,NULL)==EFI_OUT_OF_RESOURCES);assert(!mDevice && !mOpened && mCancels==1 && mCloses==1);
 StartFixture();mInstallStatus=EFI_OUT_OF_RESOURCES;mCancelStatus=EFI_DEVICE_ERROR;assert(UsbKbHidDriverBindingStart(&gUsbKbHidDriverBinding,(VOID*)1,NULL)==EFI_OUT_OF_RESOURCES);assert(mDevice && mOpened && !mCloses && !mFreed);mCancelStatus=EFI_SUCCESS;Stop();
 StartFixture();mProtocolStatus=EFI_TIMEOUT;assert(UsbKbHidDriverBindingStart(&gUsbKbHidDriverBinding,(VOID*)1,NULL)==EFI_TIMEOUT);assert(!mDevice && !mOpened && !mEvents && !mSubmits);
 StartFixture();mAllocateFail=TRUE;mCloseStatus=EFI_DEVICE_ERROR;assert(UsbKbHidDriverBindingStart(&gUsbKbHidDriverBinding,(VOID*)1,NULL)==EFI_OUT_OF_RESOURCES);assert(!mDevice && !mOpened && !mEvents && !mFreed);
 // Persistent wire failures must not replenish the host-controller retry
 // budget forever. Cancellation and key release remain real operations.
 Reset();mDevice->DelayedRecoveryEvent=(VOID*)0x1230;
 for (UINTN I=0;I<USB_HID_KB_RECOVERY_LIMIT;I++) {
  assert(KeyboardHandler(NULL,0,mDevice,EFI_USB_ERR_TIMEOUT)==EFI_DEVICE_ERROR);
  assert(!mDevice->AsyncActive);
  UsbKbHidRecoveryHandler(NULL,mDevice);
 }
 assert(mSubmits==USB_HID_KB_RECOVERY_LIMIT-1 && mTimers==mSubmits);
 assert(mCancels==USB_HID_KB_RECOVERY_LIMIT && mReports==1 && mStatusCodes==1);
 UsbKbHidRecoveryHandler(NULL,mDevice);assert(mSubmits==USB_HID_KB_RECOVERY_LIMIT-1);Stop();
 // A completed successful transfer starts a fresh error episode.
 Reset();mDevice->DelayedRecoveryEvent=(VOID*)0x1230;
 assert(KeyboardHandler(NULL,0,mDevice,EFI_USB_ERR_TIMEOUT)==EFI_DEVICE_ERROR);
 UsbKbHidRecoveryHandler(NULL,mDevice);
 assert(KeyboardHandler(Keys,8,mDevice,EFI_USB_NOERROR)==EFI_SUCCESS);
 assert(mDevice->ConsecutiveErrors==0);
 assert(KeyboardHandler(NULL,0,mDevice,EFI_USB_ERR_TIMEOUT)==EFI_DEVICE_ERROR);
 assert(mDevice->ConsecutiveErrors==1 && mStatusCodes==2 && mReports==3);Stop();
 // A failed submission may retain controller ownership. Never clear it or
 // submit a duplicate without explicit successful cancellation.
 Reset();mDevice->AsyncActive=FALSE;mSubmitStatus=EFI_DEVICE_ERROR;
 UsbKbHidRecoveryHandler(NULL,mDevice);assert(mDevice->AsyncActive && mSubmits==1);
 UsbKbHidRecoveryHandler(NULL,mDevice);assert(mSubmits==1);Stop();
 puts("actual USB HID boot LED / SET_IDLE initialization and rejected-request compatibility / checked initialization / reentrant disconnect / retained cancellation / bounded recovery: PASS");return 0;
}
