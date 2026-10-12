/** @file
  UEFI input adapter for the portable Crane PMIC button core.
  SPDX-License-Identifier: MIT
**/
#include <Uefi.h>
#include <Configuration/BootDevices.h>
#include <Guid/EventGroup.h>
#include <Protocol/ButtonServices.h>
#include <Protocol/EFICrSpmiProtocol.h>
#include <Protocol/EFICrButtonState.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Library/BaseMemoryLib.h>
#include <Library/ButtonsCrLib.h>
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC CR_BUTTON_CONTEXT mButtons;
STATIC PmicGpioDeviceContext mGpio;
STATIC EFI_CR_SPMI_PROTOCOL *mProvider;
STATIC CR_SPMI_CLIENT mClient;
STATIC CR_SPMI_BUS mBus;
STATIC EFI_HANDLE mHandle;
STATIC EFI_EVENT mTimer, mWork, mNotify, mExit;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mInput;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mInputEx;
STATIC MS_BUTTON_SERVICES_PROTOCOL mButtonServices;
STATIC EFI_CR_BUTTON_STATE_PROTOCOL mButtonState;
STATIC BOOLEAN mStopped, mServicing, mNotifying, mPublished;
STATIC BOOLEAN mBootUp, mBootDown;
STATIC UINT16 mNotifyHead, mNotifyCount;
STATIC BUTTON_CODE mNotifyKeys[CR_BUTTON_QUEUE_SIZE];
typedef struct {
  EFI_KEY_DATA Key;
  EFI_KEY_NOTIFY_FUNCTION Callback;
} KEY_NOTIFY;
STATIC KEY_NOTIFY mKeyNotifies[16];

STATIC UINT64 NowUs(VOID) {
  return GetTimeInNanoSecond(GetPerformanceCounter()) / 1000;
}
STATIC VOID MakeKey(BUTTON_CODE Code, EFI_KEY_DATA *Key) {
  ZeroMem(Key, sizeof(*Key));
  if (Code == BUTTON_CODE_POWER) Key->Key.UnicodeChar = CHAR_CARRIAGE_RETURN;
  else Key->Key.ScanCode = Code == BUTTON_CODE_VOLUME_UP ? SCAN_UP : SCAN_DOWN;
  Key->KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID;
  Key->KeyState.KeyToggleState = EFI_TOGGLE_STATE_VALID;
}
STATIC VOID Wake(VOID *Context) {
  (VOID)Context;
  if (!mStopped && mWork != NULL) (VOID)gBS->SignalEvent(mWork);
}
STATIC VOID KeyReady(VOID *Context, BUTTON_CODE Code) {
  (VOID)Context;
  if (mNotifyCount != CR_BUTTON_QUEUE_SIZE) {
    mNotifyKeys[(mNotifyHead + mNotifyCount) % CR_BUTTON_QUEUE_SIZE] = Code;
    mNotifyCount++;
    (VOID)gBS->SignalEvent(mNotify);
  }
}
STATIC BOOLEAN Matches(CONST EFI_KEY_DATA *Want, CONST EFI_KEY_DATA *Key) {
  return ((Want->Key.ScanCode == 0 && Want->Key.UnicodeChar == 0) ||
          (Want->Key.ScanCode == Key->Key.ScanCode &&
           Want->Key.UnicodeChar == Key->Key.UnicodeChar)) &&
         (Want->KeyState.KeyShiftState == 0 ||
          Want->KeyState.KeyShiftState == Key->KeyState.KeyShiftState) &&
         (Want->KeyState.KeyToggleState == 0 ||
          Want->KeyState.KeyToggleState == Key->KeyState.KeyToggleState);
}
STATIC VOID EFIAPI NotifyKeys(EFI_EVENT Event, VOID *Context) {
  UINTN Index;
  EFI_KEY_DATA Key;
  EFI_TPL OldTpl;
  (VOID)Event; (VOID)Context;
  if (mNotifying) return;
  mNotifying = TRUE;
  while (!mStopped && mNotifyCount != 0) {
    OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
    MakeKey(mNotifyKeys[mNotifyHead], &Key);
    mNotifyHead = (UINT16)((mNotifyHead + 1) % CR_BUTTON_QUEUE_SIZE);
    mNotifyCount--;
    gBS->RestoreTPL(OldTpl);
    for (Index = 0; !mStopped && Index < ARRAY_SIZE(mKeyNotifies); Index++) {
      EFI_KEY_NOTIFY_FUNCTION Callback = mKeyNotifies[Index].Callback;
      if (Callback != NULL && Matches(&mKeyNotifies[Index].Key, &Key))
        (VOID)Callback(&Key);
    }
  }
  mNotifying = FALSE;
}
STATIC VOID EFIAPI WaitKey(EFI_EVENT Event, VOID *Context) {
  EFI_TPL OldTpl;
  BOOLEAN Ready;
  (VOID)Context;
  OldTpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  Ready = !mStopped && CrButtonsHasKey(&mButtons);
  gBS->RestoreTPL(OldTpl);
  if (Ready) (VOID)gBS->SignalEvent(Event);
}
STATIC VOID EFIAPI Service(EFI_EVENT Event, VOID *Context) {
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  UINT32 Delay;
  (VOID)Event; (VOID)Context;
  if (mStopped || mServicing) return;
  mServicing = TRUE;
  OldTpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  Status = CrButtonsService(&mButtons, NowUs());
  Delay = CrButtonsNextServiceUs(&mButtons);
  gBS->RestoreTPL(OldTpl);
  if (!mStopped) {
    if (Status!=EFI_SUCCESS) DEBUG((DEBUG_ERROR, "ButtonsCr: input read failed: %r\n", Status));
    Status = gBS->SetTimer(mTimer, Delay == 0 ? TimerCancel : TimerRelative,
                          (UINT64)Delay * 10);
    if (Status!=EFI_SUCCESS) DEBUG((DEBUG_ERROR, "ButtonsCr: service timer failed: %r\n", Status));
  }
  mServicing = FALSE;
}
STATIC EFI_STATUS EFIAPI ResetEx(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                BOOLEAN ExtendedVerification) {
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  (VOID)ExtendedVerification;
  if (This != &mInputEx) return EFI_INVALID_PARAMETER;
  if (mStopped) return EFI_NOT_READY;
  OldTpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  Status = CrButtonsReset(&mButtons, NowUs());
  mNotifyHead = mNotifyCount = 0;
  gBS->RestoreTPL(OldTpl);
  Wake(NULL);
  return Status;
}
STATIC EFI_STATUS EFIAPI Reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Verify) {
  return This == &mInput ? ResetEx(&mInputEx, Verify) : EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI ReadEx(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *Key) {
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  BUTTON_CODE Code;
  if (This != &mInputEx || Key == NULL) return EFI_INVALID_PARAMETER;
  if (mStopped) return EFI_NOT_READY;
  OldTpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  Status = CrButtonsRead(&mButtons, &Code);
  gBS->RestoreTPL(OldTpl);
  if (Status==EFI_SUCCESS) MakeKey(Code, Key);
  return Status;
}
STATIC EFI_STATUS EFIAPI ReadKey(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key) {
  EFI_KEY_DATA Data;
  EFI_STATUS Status;
  if (This != &mInput || Key == NULL) return EFI_INVALID_PARAMETER;
  Status = ReadEx(&mInputEx, &Data);
  if (Status==EFI_SUCCESS) *Key = Data.Key;
  return Status;
}
STATIC EFI_STATUS EFIAPI SetState(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_TOGGLE_STATE *State) {
  if (This != &mInputEx || State == NULL) return EFI_INVALID_PARAMETER;
  return *State == EFI_TOGGLE_STATE_VALID ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI RegisterNotify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
  EFI_KEY_DATA *Key, EFI_KEY_NOTIFY_FUNCTION Callback, VOID **Handle) {
  EFI_TPL OldTpl;
  UINTN Index, Empty = ARRAY_SIZE(mKeyNotifies);
  if (This != &mInputEx || Key == NULL || Callback == NULL || Handle == NULL)
    return EFI_INVALID_PARAMETER;
  if (mStopped) return EFI_NOT_READY;
  *Handle = NULL;
  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  for (Index = 0; Index < ARRAY_SIZE(mKeyNotifies); Index++) {
    if (mKeyNotifies[Index].Callback == NULL) Empty = Index;
    else if (mKeyNotifies[Index].Callback == Callback &&
             mKeyNotifies[Index].Key.Key.ScanCode == Key->Key.ScanCode &&
             mKeyNotifies[Index].Key.Key.UnicodeChar == Key->Key.UnicodeChar &&
             mKeyNotifies[Index].Key.KeyState.KeyShiftState == Key->KeyState.KeyShiftState &&
             mKeyNotifies[Index].Key.KeyState.KeyToggleState == Key->KeyState.KeyToggleState) {
      *Handle = &mKeyNotifies[Index]; break;
    }
  }
  if (*Handle == NULL && Empty != ARRAY_SIZE(mKeyNotifies)) {
    mKeyNotifies[Empty].Key = *Key;
    mKeyNotifies[Empty].Callback = Callback;
    *Handle = &mKeyNotifies[Empty];
  }
  gBS->RestoreTPL(OldTpl);
  return *Handle == NULL ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI UnregisterNotify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, VOID *Handle) {
  EFI_TPL OldTpl;
  UINTN Index;
  if (This != &mInputEx || Handle == NULL) return EFI_INVALID_PARAMETER;
  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  for (Index = 0; Index < ARRAY_SIZE(mKeyNotifies); Index++) {
    if (Handle == &mKeyNotifies[Index] && mKeyNotifies[Index].Callback != NULL) {
      mKeyNotifies[Index].Callback = NULL;
      gBS->RestoreTPL(OldTpl);
      return EFI_SUCCESS;
    }
  }
  gBS->RestoreTPL(OldTpl);
  return EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI BootUp(MS_BUTTON_SERVICES_PROTOCOL *This, BOOLEAN *Pressed) {
  if (This != &mButtonServices || Pressed == NULL) return EFI_INVALID_PARAMETER;
  *Pressed = mBootUp; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI BootDown(MS_BUTTON_SERVICES_PROTOCOL *This, BOOLEAN *Pressed) {
  if (This != &mButtonServices || Pressed == NULL) return EFI_INVALID_PARAMETER;
  *Pressed = mBootDown; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI BootClear(MS_BUTTON_SERVICES_PROTOCOL *This) {
  if (This != &mButtonServices) return EFI_INVALID_PARAMETER;
  mBootUp = mBootDown = FALSE; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI RawState(EFI_CR_BUTTON_STATE_PROTOCOL *This,UINT32 *Pressed) {
  EFI_TPL Tpl; EFI_STATUS S;
  if (This!=&mButtonState || !Pressed) return EFI_INVALID_PARAMETER;
  Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  if (Tpl>TPL_CALLBACK || mStopped || mServicing) S=EFI_NOT_READY;
  else S=CrButtonsGetPressed(&mButtons,Pressed);
  gBS->RestoreTPL(Tpl);return S;
}
STATIC EFI_STATUS EFIAPI RawSnapshot(EFI_CR_BUTTON_STATE_PROTOCOL *This,EFI_CR_BUTTON_SNAPSHOT *Out) {
  EFI_TPL Tpl;EFI_STATUS S;EFI_CR_BUTTON_SNAPSHOT Candidate={0};
  if(This!=&mButtonState || !Out)return EFI_INVALID_PARAMETER;
  Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  if(Tpl>TPL_CALLBACK || mStopped || mServicing)S=EFI_NOT_READY;
  else S=CrButtonsGetSnapshot(&mButtons,&Candidate.Pressed,&Candidate.PowerPressSequence);
  if(S==EFI_SUCCESS){Candidate.Size=sizeof(Candidate);Candidate.Revision=EFI_CR_BUTTON_SNAPSHOT_REVISION;*Out=Candidate;}
  gBS->RestoreTPL(Tpl);return S;
}
STATIC CR_STATUS BusRead(VOID *Context, UINT8 Sid, UINT16 Address, UINT8 *Data, UINTN Size) {
  CR_SPMI_BUS *Bus=Context;return Bus->Read(Bus,Sid,Address,Data,Size);
}
STATIC CR_STATUS BusWrite(VOID *Context, UINT8 Sid, UINT16 Address, CONST UINT8 *Data, UINTN Size) {
  CR_SPMI_BUS *Bus=Context;return Bus->Write(Bus,Sid,Address,Data,Size);
}
STATIC EFI_STATUS CloseEvents(VOID) {
  EFI_EVENT *Events[] = {&mTimer, &mWork, &mNotify, &mExit,
                        &mInput.WaitForKey, &mInputEx.WaitForKeyEx};
  for (UINTN Index=0; Index<ARRAY_SIZE(Events); Index++) if (*Events[Index]) {
    EFI_STATUS S=gBS->CloseEvent(*Events[Index]);if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_NOT_READY;
    *Events[Index]=NULL;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Stop(VOID) {
  mStopped=TRUE;
  if(mServicing||mNotifying)return EFI_NOT_READY;
  EFI_STATUS Status;
  if(mTimer){Status=gBS->SetTimer(mTimer,TimerCancel,0);if(Status!=EFI_SUCCESS)return EFI_ERROR(Status)?Status:EFI_NOT_READY;}
  EFI_TPL OldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  Status=CrButtonsStop(&mButtons);
  if(Status==EFI_SUCCESS && mClient.Provider)Status=mProvider->Release(mProvider,&mClient);
  gBS->RestoreTPL(OldTpl);
  if(Status==EFI_SUCCESS)return CloseEvents();
  DEBUG((DEBUG_ERROR,"ButtonsCr: retained shared client cleanup: %r\n",Status));return Status;
}
STATIC VOID EFIAPI ExitBootServices(EFI_EVENT Event, VOID *Context) {
  (VOID)Event; (VOID)Context; (VOID)Stop();
}
EFI_STATUS EFIAPI ButtonsCrUnload(EFI_HANDLE ImageHandle) {
  EFI_STATUS Status;
  (VOID)ImageHandle;
  if (mServicing || mNotifying) return EFI_ACCESS_DENIED;
  Status = Stop();
  if (Status!=EFI_SUCCESS) return EFI_ERROR(Status)?Status:EFI_NOT_READY;
  if (mPublished) {
    Status = gBS->UninstallMultipleProtocolInterfaces(mHandle,
      &gEfiSimpleTextInProtocolGuid, &mInput,
      &gEfiSimpleTextInputExProtocolGuid, &mInputEx,
      &gMsButtonServicesProtocolGuid, &mButtonServices,
      &gEfiCrButtonStateProtocolGuid, &mButtonState,
      &gEfiDevicePathProtocolGuid, &KeypadDevicePath, NULL);
    if (Status==EFI_SUCCESS) mPublished = FALSE;
  }
  return Status==EFI_SUCCESS||EFI_ERROR(Status)?Status:EFI_NOT_READY;
}
EFI_STATUS EFIAPI ButtonsCrEntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status, Cleanup;
  CR_INTERRUPT_GUARD Guard;
  UINTN Index;
  CONST ButtonsTargetContext *Target;
  CONST PmicGpioTargetContext *GpioTarget;
  PmicGpioBusOps Bus = {.Read = BusRead, .Write = BusWrite};
  (VOID)ImageHandle; (VOID)SystemTable;
  if (mPublished || mButtons.Initialized || mClient.Provider) return EFI_ALREADY_STARTED;
  Status=gBS->LocateProtocol(&gEfiCrSpmiProtocolGuid,NULL,(VOID **)&mProvider);
  if(Status!=EFI_SUCCESS)return Status;
  if(!mProvider || mProvider->Revision!=EFI_CR_SPMI_PROTOCOL_REVISION || mProvider->Size<sizeof(*mProvider))return EFI_UNSUPPORTED;
  Target=CrDalGetButtonsContext();GpioTarget=CrDalGetPmicGpioContext();
  if(!Target || !GpioTarget)return EFI_NOT_FOUND;
  mStopped = FALSE;
  mButtonState.Revision=EFI_CR_BUTTON_STATE_PROTOCOL_REVISION; mButtonState.GetState=RawState; mButtonState.GetSnapshot=RawSnapshot;
  mInput.Reset = Reset; mInput.ReadKeyStroke = ReadKey;
  mInputEx.Reset = ResetEx; mInputEx.ReadKeyStrokeEx = ReadEx;
  mInputEx.SetState = SetState; mInputEx.RegisterKeyNotify = RegisterNotify;
  mInputEx.UnregisterKeyNotify = UnregisterNotify;
  mButtonServices.PreBootVolumeUpButtonThenPowerButtonCheck = BootUp;
  mButtonServices.PreBootVolumeDownButtonThenPowerButtonCheck = BootDown;
  mButtonServices.PreBootClearVolumeButtonState = BootClear;
  Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Service, NULL, &mTimer);
  if (Status!=EFI_SUCCESS) goto Failure;
  Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Service, NULL, &mWork);
  if (Status!=EFI_SUCCESS) goto Failure;
  Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NotifyKeys, NULL, &mNotify);
  if (Status!=EFI_SUCCESS) goto Failure;
  Status = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY, WaitKey, NULL, &mInput.WaitForKey);
  if (Status!=EFI_SUCCESS) goto Failure;
  Status = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY, WaitKey, NULL, &mInputEx.WaitForKeyEx);
  if (Status!=EFI_SUCCESS) goto Failure;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices, NULL,
                             &gEfiEventExitBootServicesGuid, &mExit);
  if (Status!=EFI_SUCCESS) goto Failure;
  /* This image has its own cold OSKAL provider cache. Bootstrap at normal
     TPL before excluding IRQs for the PDC/SPMI route setup. */
  Status = CrInterruptGuardAcquire(&Guard);
  if (Status!=EFI_SUCCESS) goto Failure;
  Status=mProvider->Acquire(mProvider,CrSpmiClientButtons,&mButtons,&mClient,&mBus);
  Bus.Context=&mBus;
  if(Status==EFI_SUCCESS)Status=PmicGpioInitialize(&mGpio,GpioTarget,&Bus);
  if(Status==EFI_SUCCESS)Status=CrButtonsStartShared(&mButtons,Target,&mBus,&mGpio,Wake,KeyReady,NULL,NowUs());
  if(Status==EFI_SUCCESS)for(Index=0;Index<Target->ButtonCount;Index++) {
    if(Target->Buttons[Index].Code==BUTTON_CODE_VOLUME_UP)mBootUp=mButtons.State[Index].Stable;
    if(Target->Buttons[Index].Code==BUTTON_CODE_VOLUME_DOWN)mBootDown=mButtons.State[Index].Stable;
  }
  CrInterruptGuardRelease(&Guard);
  if (Status!=EFI_SUCCESS) goto Failure;
  Status = gBS->InstallMultipleProtocolInterfaces(&mHandle,
    &gEfiSimpleTextInProtocolGuid, &mInput,
    &gEfiSimpleTextInputExProtocolGuid, &mInputEx,
    &gMsButtonServicesProtocolGuid, &mButtonServices,
      &gEfiCrButtonStateProtocolGuid, &mButtonState,
    &gEfiDevicePathProtocolGuid, &KeypadDevicePath, NULL);
  if (Status!=EFI_SUCCESS) goto Failure;
  mPublished = TRUE;
  DEBUG((DEBUG_WARN, "ButtonsCr: shared PMIC IRQ ready; Vol+=Up Vol-=Down Power=Enter\n"));
  return EFI_SUCCESS;
Failure:
  DEBUG((DEBUG_ERROR, "ButtonsCr: initialization failed: %r\n", Status));
  Cleanup = Stop();
  /* A retained IRQ still calls this image. Keep it resident for unload retry. */
  return Cleanup!=EFI_SUCCESS ? EFI_SUCCESS : Status;
}
