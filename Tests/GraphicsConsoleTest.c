/** Production GOP override, rendering-display selection and OSK deferred bind. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>
#include "../../MUCommonCrPkg/Include/Configuration/DisplayDevicePath.h"
#include <Protocol/RenderingEngine.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_GUID gEfiGraphicsOutputProtocolGuid = { .Data1 = 1 };
EFI_GUID gEfiDevicePathProtocolGuid = { .Data1 = 2 };
EFI_GUID gMsSREProtocolGuid = { .Data1 = 3 };
EFI_GUID gMsSWMProtocolGuid = { .Data1 = 4 };
EFI_GUID gEfiSimpleTextInProtocolGuid, gEfiSimpleTextInputExProtocolGuid;
EFI_GUID gEfiConsoleInDeviceGuid;
static EFI_GUID OverrideGuid = { .Data1 = 5 }, SelectedGuid;
static EFI_DISPLAY_DEVICE_PATH PrimaryPath = ANDROMEDA_DISPLAY_DEVICE_PATH_INIT;
static EFI_DISPLAY_DEVICE_PATH SecondaryPath = ANDROMEDA_DP_DISPLAY_DEVICE_PATH_INIT;
static BOOLEAN SelectPrimary;
EFI_BOOT_SERVICES *gBS;
static EFI_SYSTEM_TABLE SystemTable;
EFI_SYSTEM_TABLE *gST = &SystemTable;
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL Input;
static BOOLEAN InputInstalled;

typedef struct {
  EFI_EVENT_NOTIFY Notify;
  VOID *Context;
  BOOLEAN Closed, Timer;
} TEST_EVENT;
static TEST_EVENT Events[12];
static unsigned EventCount, Pools, Registrations, Signals, Stage2, KeyboardInit, Connections;
static unsigned FailRegistration;
static BOOLEAN NativeGop, NativeOverride, OtherGop, NativeSre, OtherSre, WindowReady;
static BOOLEAN FailLocate, FailRemove, Bound;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info = { .HorizontalResolution = 1920, .VerticalResolution = 1080 };
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode = { .Info = &Info };
static EFI_GRAPHICS_OUTPUT_PROTOCOL Virtual = { .Mode = &Mode }, Native = { .Mode = &Mode }, Other = { .Mode = &Mode };
static MS_RENDERING_ENGINE_PROTOCOL Engine;
static EFI_DEVICE_PATH_PROTOCOL Path;
static EFI_HANDLE NativeHandle = (VOID *)2;

BOOLEAN EFIAPI CompareGuid (CONST GUID *A, CONST GUID *B) { return memcmp(A,B,sizeof(*A)) == 0; }
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) {}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return FALSE; }
VOID EFIAPI FreePool (VOID *Buffer) { assert(Pools); Pools--; free(Buffer); }
static EFI_STATUS EFIAPI LocateHandles (EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Guid, VOID *Key,
                                        UINTN *Count, EFI_HANDLE **Handles) {
  assert(Type == ByProtocol);
  *Handles = NULL;
  if(FailLocate) return EFI_DEVICE_ERROR;
  EFI_HANDLE Found[3]; UINTN N = 0;
  if(Guid == &gEfiGraphicsOutputProtocolGuid) {
    Found[N++] = (VOID *)1;
    if(NativeGop) Found[N++] = NativeHandle;
    if(OtherGop) Found[N++] = (VOID *)3;
  } else {
    assert(Guid == &gMsSREProtocolGuid);
    if(NativeSre) Found[N++] = NativeHandle;
    if(OtherSre) Found[N++] = (VOID *)3;
  }
  if(!N) return EFI_NOT_FOUND;
  *Count = N; *Handles = malloc(N * sizeof(**Handles)); assert(*Handles); Pools++;
  memcpy(*Handles, Found, N * sizeof(**Handles));
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI HandleProtocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Interface) {
  *Interface = NULL;
  if(Guid == &gEfiDevicePathProtocolGuid) {
    if(Handle == (VOID *)1) return EFI_NOT_FOUND;
    *Interface = SelectPrimary ? (Handle == NativeHandle ? (VOID *)&PrimaryPath : (VOID *)&SecondaryPath) : (VOID *)&Path;
  } else if(Guid == &gEfiGraphicsOutputProtocolGuid) {
    if(Handle == (VOID *)1) *Interface = &Virtual;
    else if(Handle == NativeHandle && NativeGop) *Interface = &Native;
    else if(Handle == (VOID *)3 && OtherGop) *Interface = &Other;
    else return EFI_NOT_FOUND;
  } else if(Guid == &gEfiSimpleTextInputExProtocolGuid) {
    if(Handle != (VOID *)11 || !InputInstalled) return EFI_NOT_FOUND;
    *Interface = &Input;
  } else {
    assert(Guid == &gMsSREProtocolGuid);
    if((Handle == NativeHandle && NativeSre) || (Handle == (VOID *)3 && OtherSre)) *Interface = &Engine;
    else return EFI_NOT_FOUND;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CreateEvent (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify,
                                      VOID *Context, EFI_EVENT *Event) {
  assert((Type == EVT_NOTIFY_SIGNAL || Type == (EVT_NOTIFY_SIGNAL | EVT_TIMER)) && EventCount < ARRAY_SIZE(Events));
  Events[EventCount] = (TEST_EVENT){ .Notify = Notify, .Context = Context };
  *Event = &Events[EventCount++]; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseEvent (EFI_EVENT Event) {
  TEST_EVENT *E = Event; assert(!E->Closed); E->Closed = TRUE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetTimer (EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Time) {
  TEST_EVENT *E = Event; assert(!E->Closed && Type == TimerPeriodic && Time == 1000000);
  E->Timer = TRUE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SignalEvent (EFI_EVENT Event) {
  TEST_EVENT *E = Event; assert(!E->Closed); Signals++; E->Notify(Event, E->Context); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI RegisterNotify (EFI_GUID *Guid, EFI_EVENT Event, VOID **Registration) {
  Registrations++;
  if(Registrations == FailRegistration) return EFI_OUT_OF_RESOURCES;
  *Registration = Event; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Install (EFI_HANDLE *Handle, EFI_GUID *Guid,
                                  EFI_INTERFACE_TYPE Type, VOID *Interface) {
  assert(*Handle == NativeHandle && Guid == &OverrideGuid && Interface == &Native);
  NativeOverride = TRUE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Uninstall (EFI_HANDLE Handle, ...) {
  va_list Args; va_start(Args, Handle); EFI_GUID *Guid = va_arg(Args, EFI_GUID *);
  VOID *Interface = va_arg(Args, VOID *); va_end(Args);
  assert(Handle == NativeHandle && Interface == &Native);
  if(Guid == &gEfiGraphicsOutputProtocolGuid) {
    if(FailRemove) return EFI_ACCESS_DENIED;
    NativeGop = FALSE;
  } else { assert(Guid == &OverrideGuid); NativeOverride = FALSE; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LocateProtocol (EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  assert(Guid == &gMsSWMProtocolGuid); *Interface = WindowReady ? &Engine : NULL;
  return WindowReady ? EFI_SUCCESS : EFI_NOT_FOUND;
}

#undef PcdGetPtr
#define PcdGetPtr(Token) TEST_##Token
#define TEST_PcdMsGopOverrideProtocolGuid (&OverrideGuid)
#define TEST_PcdMsGopOverrideControllerGuid (&SelectedGuid)
#define DriverInit OverrideInit
#define DriverUnload OverrideUnload
#include "../Override/MU_PLUS/MsGraphicsPkg/GopOverrideDxe/GopOverrideDxe.c"
#undef DriverInit
#undef DriverUnload

static EFI_HANDLE SwmImage;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *SwmGop;
static MS_RENDERING_ENGINE_PROTOCOL *mRenderingEngine;
static EFI_EVENT SwmEvent;
static VOID *SwmRegistration, *mSreRegistration, *mInputRegistration, *mTheme;
static EFI_ABSOLUTE_POINTER_MODE mAbsPointerMode;
static VOID *MsUiGetPlatformTheme (VOID) { return NULL; }
static EFI_STATUS EFIAPI DriverInitStage2 (EFI_HANDLE Handle) {
  assert(SwmGop == &Native && mRenderingEngine == &Engine);
  assert(gST->ConsoleInHandle == (VOID *)11 && InputInstalled);
  Stage2++; WindowReady = TRUE; return EFI_SUCCESS;
}
static EFI_STATUS WindowCleanUp (EFI_HANDLE Handle) { abort(); }
#define mImageHandle SwmImage
#define mGop SwmGop
#define mGopRegisterEvent SwmEvent
#define mGopRegistration SwmRegistration
#define GopRegisteredCallback WindowCallback
#define DriverInit WindowInit
#define DriverCleanUp WindowCleanUp
#include "WindowInit.inc"
#undef DriverCleanUp
#undef DriverInit
#undef GopRegisteredCallback
#undef mGopRegistration
#undef mGopRegisterEvent
#undef mGop
#undef mImageHandle

static EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
static MS_SIMPLE_WINDOW_MANAGER_PROTOCOL *mSWMProtocol;
static EFI_HANDLE mControllerHandle = (VOID *)9;
static EFI_EVENT mSwmNotifyEvent, mKeyRepeatTimerEvent, mCheckDisplayModeTimerEvent;
static VOID *mSwmRegistration;
static struct { EFI_SIMPLE_TEXT_INPUT_PROTOCOL SimpleTextIn; EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL SimpleTextInEx; } mOSK;
static EFI_DRIVER_BINDING_PROTOCOL gOSKDriverBinding = { .DriverBindingHandle = (VOID *)10 };
static EFI_STATUS EFIAPI OSKDriverInit (VOID) { assert(mGop == &Native); KeyboardInit++; return EFI_SUCCESS; }
#include "KeyboardBind.inc"
static EFI_STATUS EFIAPI OpenProtocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Interface,
                                       EFI_HANDLE Agent, EFI_HANDLE Controller, UINT32 Attr) {
  assert(Handle == mControllerHandle && Guid == &gEfiDevicePathProtocolGuid);
  if(Bound) return EFI_ALREADY_STARTED;
  Bound = TRUE; *Interface = &Path; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseProtocol (EFI_HANDLE Handle, EFI_GUID *Guid, EFI_HANDLE Agent, EFI_HANDLE Controller) {
  assert(Bound); Bound = FALSE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Connect (EFI_HANDLE Handle, EFI_HANDLE *Drivers, EFI_DEVICE_PATH_PROTOCOL *Remaining, BOOLEAN Recursive) {
  assert(Handle == mControllerHandle && Drivers[0] == gOSKDriverBinding.DriverBindingHandle && !Drivers[1] && !Recursive);
  Connections++; return OSKDriverBindingStart(&gOSKDriverBinding, Handle, NULL);
}
static void Reset (void) {
  assert(!Pools);
  EventCount = Registrations = Signals = Stage2 = KeyboardInit = Connections = FailRegistration = 0;
  NativeGop = TRUE; SelectPrimary = FALSE; memset(&SelectedGuid,0,sizeof(SelectedGuid));
  NativeOverride = OtherGop = NativeSre = OtherSre = WindowReady = FailLocate = FailRemove = Bound = FALSE;
  mGopRegisterEvent = SwmEvent = mSwmNotifyEvent = NULL;
  gST->ConsoleInHandle = (VOID *)11; InputInstalled = TRUE;
  SwmGop = mGop = NULL; mRenderingEngine = NULL;
}
int main (void) {
  EFI_BOOT_SERVICES Bs = { .LocateHandleBuffer = LocateHandles, .HandleProtocol = HandleProtocol,
    .CreateEvent = CreateEvent, .CloseEvent = CloseEvent, .SignalEvent = SignalEvent, .SetTimer = SetTimer,
    .RegisterProtocolNotify = RegisterNotify, .InstallProtocolInterface = Install,
    .UninstallMultipleProtocolInterfaces = Uninstall, .LocateProtocol = LocateProtocol,
    .OpenProtocol = OpenProtocol, .CloseProtocol = CloseProtocol, .ConnectController = Connect };
  gBS = &Bs;
  Reset();
  assert(OverrideInit(NULL, NULL) == EFI_SUCCESS);
  assert(Signals == 1 && NativeOverride && !NativeGop && !mGopRegisterEvent && !Pools);
  Reset(); NativeGop = FALSE;
  assert(OverrideInit(NULL, NULL) == EFI_SUCCESS && mGopRegisterEvent && !NativeOverride);
  NativeGop = TRUE; SignalEvent(mGopRegisterEvent);
  assert(NativeOverride && !mGopRegisterEvent);
  Reset(); OtherGop = TRUE;
  assert(OverrideInit(NULL, NULL) == EFI_SUCCESS && !NativeOverride && NativeGop && mGopRegisterEvent);
  /* DP may publish before the primary, or both before override dispatch.
     Only the configured primary becomes the single rendering-engine parent. */
  Reset(); OtherGop = SelectPrimary = TRUE; SelectedGuid = PrimaryPath.DisplayDevicePath.Guid;
  assert(OverrideInit(NULL, NULL) == EFI_SUCCESS && NativeOverride && !NativeGop && OtherGop && !mGopRegisterEvent);
  Reset(); NativeGop = FALSE; OtherGop = SelectPrimary = TRUE; SelectedGuid = PrimaryPath.DisplayDevicePath.Guid;
  assert(OverrideInit(NULL, NULL) == EFI_SUCCESS && !NativeOverride && OtherGop && mGopRegisterEvent);
  NativeGop = TRUE; SignalEvent(mGopRegisterEvent);
  assert(NativeOverride && OtherGop && !mGopRegisterEvent);
  Reset(); SelectPrimary = TRUE; SelectedGuid = PrimaryPath.DisplayDevicePath.Guid;
  PrimaryPath.EndDevicePath.SubType = END_INSTANCE_DEVICE_PATH_SUBTYPE;
  assert(OverrideInit(NULL, NULL) == EFI_SUCCESS && !NativeOverride && NativeGop && mGopRegisterEvent);
  PrimaryPath.EndDevicePath.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SignalEvent(mGopRegisterEvent); assert(NativeOverride && !mGopRegisterEvent);
  Reset(); FailLocate = TRUE;
  assert(OverrideInit(NULL, NULL) == EFI_SUCCESS && !Pools && !NativeOverride);
  Reset(); FailRemove = TRUE;
  assert(OverrideInit(NULL, NULL) == EFI_SUCCESS && NativeGop && !NativeOverride && mGopRegisterEvent);
  FailRemove = FALSE; SignalEvent(mGopRegisterEvent); assert(NativeOverride && !mGopRegisterEvent);
  Reset(); FailRegistration = 1;
  assert(OverrideInit(NULL, NULL) == EFI_OUT_OF_RESOURCES && !mGopRegisterEvent && Events[0].Closed);

  /* ConSplitter installs InputEx before assigning ConsoleInHandle. Neither
     early notification may trigger stage2 or permanently consume the GOP. */
  Reset(); NativeSre = TRUE; gST->ConsoleInHandle = NULL; InputInstalled = FALSE;
  assert(WindowInit((VOID *)7, NULL) == EFI_SUCCESS && SwmEvent && !Stage2 && !SwmGop);
  InputInstalled = TRUE; SignalEvent(SwmEvent);
  assert(!Stage2 && !SwmGop && SwmEvent);
  gST->ConsoleInHandle = (VOID *)11; InputInstalled = FALSE; SignalEvent(SwmEvent);
  assert(!Stage2 && !SwmGop && SwmEvent);
  InputInstalled = TRUE;
  assert(((TEST_EVENT *)SwmEvent)->Timer);
  SignalEvent(SwmEvent); assert(Stage2 == 1 && !SwmEvent);
  WindowCallback(NULL, NULL); assert(Stage2 == 1);

  Reset(); NativeSre = TRUE;
  assert(WindowInit((VOID *)7, NULL) == EFI_SUCCESS);
  assert(Stage2 == 1 && SwmGop == &Native && !SwmEvent && Registrations == 3 && Signals == 1);
  assert(mAbsPointerMode.AbsoluteMaxX == 1920 && mAbsPointerMode.AbsoluteMaxY == 1080);
  Reset();
  assert(WindowInit((VOID *)7, NULL) == EFI_SUCCESS && SwmEvent && !Stage2 && !SwmGop);
  NativeSre = TRUE; SignalEvent(SwmEvent); assert(Stage2 == 1 && !SwmEvent);
  Reset(); NativeSre = OtherSre = OtherGop = TRUE;
  assert(WindowInit((VOID *)7, NULL) == EFI_SUCCESS && !Stage2 && !SwmGop && !mRenderingEngine);
  Reset(); FailRegistration = 2;
  assert(WindowInit(NULL, NULL) == EFI_OUT_OF_RESOURCES && !SwmEvent && Events[0].Closed);

  Reset(); NativeSre = TRUE;
  assert(OSKDriverBindingStart(&gOSKDriverBinding, mControllerHandle, NULL) == EFI_NOT_FOUND && !Bound);
  assert(OSKRegisterWindowManagerNotify() == EFI_SUCCESS && mSwmNotifyEvent && !Connections);
  WindowReady = TRUE; SignalEvent(mSwmNotifyEvent);
  assert(Connections == 1 && KeyboardInit == 1 && mGop == &Native && Bound && !mSwmNotifyEvent);
  Reset(); NativeSre = WindowReady = TRUE;
  assert(OSKRegisterWindowManagerNotify() == EFI_SUCCESS && KeyboardInit == 1 && !mSwmNotifyEvent);
  Reset(); FailRegistration = 1;
  assert(OSKRegisterWindowManagerNotify() == EFI_OUT_OF_RESOURCES && !mSwmNotifyEvent && Events[0].Closed);
  assert(!Pools);
  puts("Graphics console: native plus virtual GOP, existing/delayed protocols, ambiguity, rollback, same-handle SRE and deferred OSK bind PASS");
  return 0;
}
