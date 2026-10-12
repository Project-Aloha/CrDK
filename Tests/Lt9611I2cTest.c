/** @file
 *  Exercise LT9611UXC transactions and MCU cleanup through Crane I2C.
 *  SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef NULL
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/CrDalLib.h>
#include <Library/TimerLib.h>
#include <Protocol/EFII2cCrProtocol.h>
#include <Protocol/EFIGpiCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFIMdpDsiProtocol.h>
#include <Protocol/EFIMdssHandoffProtocol.h>

static BOOLEAN HandoffEnabled;
static UINT32 NativePanelSelection;
#define _PCD_GET_MODE_32_PcdMdssPanelProfile NativePanelSelection
#define _PCD_GET_MODE_BOOL_PcdMdssHandoffEnable HandoffEnabled
static BOOLEAN ExtendedEnabled;
#define _PCD_GET_MODE_BOOL_PcdMdssHdkExtendedProfileEnable ExtendedEnabled

static EFI_BOOT_SERVICES BootServices;
EFI_BOOT_SERVICES *gBS = &BootServices;
EFI_GUID gEfiI2cCrProtocolGuid;
EFI_GUID gEfiGpioCrProtocolGuid;
EFI_GUID gEfiGpiCrProtocolGuid;
EFI_GUID gEfiLt9611CrProtocolGuid;
EFI_GUID gEfiLt9611PanelCrProtocolGuid;
EFI_GUID gEfiMdpDsiProtocolGuid;
EFI_GUID gEfiMdssHandoffProtocolGuid;
EFI_GUID gEfiEventReadyToBootGuid = EFI_EVENT_GROUP_READY_TO_BOOT;

static EFI_I2C_CR_PROTOCOL Instances[2];
static UINT8 Page;
static BOOLEAN Locked;
static UINTN Transfers;
static UINTN FailOnTransfer;
static UINTN Unlocks;
static UINTN Sleeps;
static UINTN AvailableHandles = 2;
static BOOLEAN FailLockWriteAfterCommit;
static UINTN GpiTransfers;
static BOOLEAN FailGpiTransfer;
static UINTN CorruptOnTransfer, FirmwareReads;
static BOOLEAN CorruptGpiId, TraceProbe;
static CHAR8 ProbeSequence[32];
static UINTN ProbeCount;
static void ProbeTransfer(CHAR8 Mode) {
  if (TraceProbe) { assert(ProbeCount < sizeof(ProbeSequence)); ProbeSequence[ProbeCount++] = Mode; }
}
static UINT8 PageD0[256];
static UINT8 FirmwareVersion = 0x40;
static UINT8 IrqStatus, HpdStatus;
static BOOLEAN InRawIrq, EdgeDuringService, WorkerDuringConfigure;
static EFI_TPL CurrentTpl;
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID) {return CurrentTpl;}
static EFI_EVENT_NOTIFY TimerNotify, ExitNotify, ReadyNotify;
static UINTN CreatedEvents, ClosedEvents, TimerCancels;
static BOOLEAN FailCreate, FailTimer, FailInstall, FailIrqRegister;
static UINT64 TimerTime, TimerDeadline;
static UINTN TimerArms;
static BOOLEAN TimerArmed;
static BOOLEAN CpuInterrupts = TRUE;
static EFI_MDSS_HANDOFF_FREEZE FrozenCallback;
static EFI_MDSS_HANDOFF_SNAPSHOT SnapshotCallback;
static VOID *HandoffContext;
static EFI_STATUS RegistrationStatus;
static UINTN Registrations;
static BOOLEAN FreezeInTransfer;
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Bytes) { return memset(Buffer,0,Bytes); }
VOID *EFIAPI CopyMem (VOID *Destination, CONST VOID *Source, UINTN Bytes) { return memmove(Destination,Source,Bytes); }
VOID EFIAPI MemoryFence (VOID) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
BOOLEAN EFIAPI SaveAndDisableInterrupts (VOID) { BOOLEAN Old=CpuInterrupts;CpuInterrupts=FALSE;return Old; }
BOOLEAN EFIAPI SetInterruptState (BOOLEAN State) { CpuInterrupts=State;return State; }
static EFI_STATUS EFIAPI SetParticipant (EFI_MDSS_HANDOFF_PROTOCOL *This,
    UINT32 Owner, VOID *Context, EFI_MDSS_HANDOFF_FREEZE Freeze,
    EFI_MDSS_HANDOFF_SNAPSHOT Snapshot) {
  assert(This && Owner==CR_MDSS_NATIVE_BRIDGE && Context && Freeze && Snapshot);
  ++Registrations;
  if(RegistrationStatus!=EFI_SUCCESS)return RegistrationStatus;
  FrozenCallback=Freeze;SnapshotCallback=Snapshot;HandoffContext=Context;
  return EFI_SUCCESS;
}
static EFI_MDSS_HANDOFF_PROTOCOL Handoff={EFI_MDSS_HANDOFF_PROTOCOL_REVISION,SetParticipant};
VOID Lt9611IrqHandler (VOID *Param);
static EFI_TPL EFIAPI RaiseTpl (EFI_TPL Tpl) { EFI_TPL old=CurrentTpl; assert(Tpl>=old); CurrentTpl=Tpl; return old; }
static VOID EFIAPI RestoreTpl (EFI_TPL Tpl) { CurrentTpl=Tpl; }
static EFI_STATUS EFIAPI CreateEvent (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify, VOID *Context, EFI_EVENT *Event) {
  assert(!Context);
  if(FailCreate) return EFI_OUT_OF_RESOURCES;
  if(Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL)) { assert(Tpl==TPL_CALLBACK); TimerNotify=Notify; *Event=(VOID *)1; }
  else { assert(Type==EVT_SIGNAL_EXIT_BOOT_SERVICES && Tpl==TPL_NOTIFY); ExitNotify=Notify; *Event=(VOID *)2; }
  CreatedEvents++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CreateEventEx (UINT32 Type, EFI_TPL Tpl,
    EFI_EVENT_NOTIFY Notify, CONST VOID *Context, CONST EFI_GUID *Group,
    EFI_EVENT *Event) {
  assert(Type == EVT_NOTIFY_SIGNAL && Tpl == TPL_CALLBACK);
  assert(Context == NULL && Group == &gEfiEventReadyToBootGuid);
  assert(Notify != NULL && Event != NULL);
  if (FailCreate) return EFI_OUT_OF_RESOURCES;
  /* Expire any timer started too early while event setup is still running. */
  if (TimerArmed) {
    EFI_TPL Saved=CurrentTpl;
    TimerTime=TimerDeadline; TimerArmed=FALSE; CurrentTpl=TPL_CALLBACK;
    TimerNotify((VOID *)1,NULL); CurrentTpl=Saved;
  }
  ReadyNotify = Notify;
  *Event = (VOID *)3;
  CreatedEvents++;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetTimer (EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Time) {
  assert(Event==(VOID *)1);
  if(Type==TimerCancel) { assert(!Time); TimerCancels++; TimerArmed=FALSE; return EFI_SUCCESS; }
  assert(Type==TimerRelative && Time==100000);
  if(FailTimer) return EFI_DEVICE_ERROR;
  TimerDeadline=TimerTime+Time; TimerArmed=TRUE; TimerArms++;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseEvent (EFI_EVENT Event) { assert(Event==(VOID *)1 || Event==(VOID *)2 || Event==(VOID *)3); ClosedEvents++; return EFI_SUCCESS; }

static UINT16 PowerPins[] = {9, 109, 107};
static BOOLEAN TraceGpio;
static UINT8 PinLevels[110];
static UINTN EventCount;
static UINTN GpioOperations;
static UINTN FailGpioOperation;
static UINTN RegisteredIrqs;
static UINTN InstalledBridges;
static CHAR8 EventKinds[16];
static UINT32 EventValues[16];
static CR_DAL_LT9611_CONFIG BridgeConfig = {
  .I2cInstance = 10, .I2cAddress = 0x2b,
  .InterruptPin = 44, .PowerPinCount = 3, .PowerPins = PowerPins
};

static void RecordEvent (CHAR8 Kind, UINT32 Value) {
  if (!TraceGpio) return;
  assert(EventCount < sizeof(EventKinds));
  EventKinds[EventCount] = Kind;
  EventValues[EventCount++] = Value;
}

BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN ErrorLevel) {
  (VOID)ErrorLevel;
  return TRUE;
}
VOID EFIAPI DebugPrint (UINTN ErrorLevel, CONST CHAR8 *Format, ...) {
  (VOID)ErrorLevel;
  (VOID)Format;
}

STATIC CR_MDSS_TARGET TestMdss={.Soc=0x8450};
STATIC CR_TARGET_QUP_CONTEXT TestQup={.GpiCount=1};
CONST CR_MDSS_TARGET *CrDalGetMdssContext(VOID){return &TestMdss;}
CONST CR_TARGET_QUP_CONTEXT *CrDalGetQupContext(VOID){return &TestQup;}
CONST CR_DAL_LT9611_CONFIG *CrDalGetLt9611Config (VOID) { return &BridgeConfig; }
UINTN EFIAPI MicroSecondDelay (UINTN Time) {
  assert(Time == 1000 || Time == 20000 || Time == 50000 || Time == 300000);
  RecordEvent('D', (UINT32)Time);
  ++Sleeps;
  TimerTime += Time * 10U;
  return Time;
}
VOID EFIAPI FreePool (VOID *Buffer) { free(Buffer); }

static EFI_STATUS EFIAPI LocateI2cHandles (EFI_LOCATE_SEARCH_TYPE SearchType,
    EFI_GUID *Protocol, VOID *SearchKey, UINTN *Count, EFI_HANDLE **Handles) {
  assert(SearchType == ByProtocol && Protocol == &gEfiI2cCrProtocolGuid);
  assert(SearchKey == NULL);
  *Count = AvailableHandles;
  if (*Count == 0) {
    *Handles = NULL;
    return EFI_SUCCESS;
  }
  *Handles = malloc(2 * sizeof(**Handles));
  assert(*Handles != NULL);
  (*Handles)[0] = (EFI_HANDLE)&Instances[0];
  (*Handles)[1] = (EFI_HANDLE)&Instances[1];
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI HandleI2c (EFI_HANDLE Handle, EFI_GUID *Protocol,
    VOID **Interface) {
  assert(Protocol == &gEfiI2cCrProtocolGuid);
  *Interface = (VOID *)Handle;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI LocateGpi (EFI_GUID *Protocol, VOID *Registration,
    VOID **Interface) {
  assert(Registration == NULL);
  if(Protocol==&gEfiMdssHandoffProtocolGuid){*Interface=&Handoff;return EFI_SUCCESS;}
  if (Protocol == &gEfiMdpDsiProtocolGuid) {
    *Interface = NULL;
    return EFI_NOT_FOUND;
  }
  if (Protocol == &gEfiGpioCrProtocolGuid) {
    extern EFI_GPIO_CR_PROTOCOL TestGpioProtocol;
    *Interface = &TestGpioProtocol;
    return EFI_SUCCESS;
  }
  assert(Protocol == &gEfiGpiCrProtocolGuid);
  extern EFI_GPI_CR_PROTOCOL TestGpiProtocol;
  *Interface = &TestGpiProtocol;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI ConfigGpio (EFI_GPIO_CR_PROTOCOL *This,
    GpioConfigParams *Config) {
  assert(This != NULL && Config != NULL);
  assert(Config->FunctionSel == GPIO_FUNC_NORMAL);
  assert(Config->DriveStrength == GPIO_DRIVE_STRENGTH_UNCHANGE);
  if (Config->PinNumber == BridgeConfig.InterruptPin) {
    assert(!Config->OutputEnable && Config->OutputValue == GPIO_VALUE_UNCHANGE);
  }
  RecordEvent('C', (Config->PinNumber << 8) | Config->OutputValue);
  ++GpioOperations;
  EFI_STATUS S=GpioOperations == FailGpioOperation ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  if(S==EFI_SUCCESS&&Config->OutputEnable&&Config->OutputValue<=1)PinLevels[Config->PinNumber]=Config->OutputValue;
  return S;
}

static EFI_STATUS EFIAPI SetGpio (EFI_GPIO_CR_PROTOCOL *This,
    UINT16 Pin, UINT8 Value) {
  assert(This != NULL && (Pin == PowerPins[0] || Pin == PowerPins[1] || Pin == PowerPins[2]) && Value <= 1);
  RecordEvent('S', (Pin << 8) | Value);
  ++GpioOperations;
  EFI_STATUS S=GpioOperations == FailGpioOperation ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  if(S==EFI_SUCCESS)PinLevels[Pin]=Value;return S;
}

static EFI_STATUS EFIAPI ReadGpio(EFI_GPIO_CR_PROTOCOL *This,UINT16 Pin,UINT8 *Value) {
  assert(This&&Value&&(Pin==PowerPins[0]||Pin==PowerPins[1]||Pin==PowerPins[2]));*Value=PinLevels[Pin];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GetGpio(EFI_GPIO_CR_PROTOCOL *This,GpioConfigParams *Config) {
  assert(This&&Config);Config->OutputEnable=TRUE;Config->FunctionSel=GPIO_FUNC_NORMAL;
  Config->OutputValue=PinLevels[Config->PinNumber];return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI RegisterIrq (EFI_GPIO_CR_PROTOCOL *This,
    UINT16 Pin, CR_INTERRUPT_HANDLER Handler, VOID *Context,
    CR_INTERRUPT_TRIGGER_TYPE Trigger) {
  assert(This != NULL && Pin == BridgeConfig.InterruptPin && Handler != NULL && Context == NULL);
  assert(Trigger == CR_INTERRUPT_TRIGGER_EDGE_FALLING);
  ++RegisteredIrqs;
  if(FailIrqRegister) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

EFI_GPIO_CR_PROTOCOL TestGpioProtocol = {
  .Revision = EFI_GPIO_CR_PROTOCOL_REVISION,
  .SetIoValue = SetGpio,
  .ReadIoValue = ReadGpio,
  .GetGpioConfig = GetGpio,
  .ConfigGpio = ConfigGpio,
  .RegisterGpioInterrupt = RegisterIrq,
};

static EFI_STATUS EFIAPI InstallBridge (EFI_HANDLE *Handle, ...) {
  assert(Handle != NULL);
  if(FailInstall) return EFI_DEVICE_ERROR;
  ++InstalledBridges;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI GpiTransfer (EFI_GPI_CR_PROTOCOL *This,
    UINT32 Instance, UINT16 Address, EFI_GPI_I2C_DESCRIPTOR *Descriptors,
    UINT16 Count, UINT32 *Transferred) {
  UINT8 Register;
  assert(This != NULL && Instance == 10 && Address == 0x2b);
  assert(Transferred == NULL);
  ++GpiTransfers;
  ProbeTransfer('G');
  if (FailGpiTransfer) return EFI_OUT_OF_RESOURCES;
  assert(Count == 1 || Count == 2);
  Register = Descriptors[0].Buffer[0];
  assert((Descriptors[0].Flags & (EFI_GPI_I2C_FLAG_START |
                                  EFI_GPI_I2C_FLAG_WRITE)) != 0);
  if (Count == 1) {
    assert(Descriptors[0].Length == 2 &&
           (Descriptors[0].Flags & EFI_GPI_I2C_FLAG_STOP) != 0);
    if (Register == 0xff) {
      Page = Descriptors[0].Buffer[1];
    } else {
      assert(Register == 0xee && Page == 0x80);
      Locked = Descriptors[0].Buffer[1] == 1;
      if (!Locked) ++Unlocks;
    }
    return EFI_SUCCESS;
  }
  assert(Locked && Page == 0x81 && Register < 3);
  assert(Descriptors[0].Length == 1 && Descriptors[1].Length == 1);
  Descriptors[1].Buffer[0] = (UINT8[]){0x17, 0x04, 0x92}[Register];
  if (CorruptGpiId && Register == 2) Descriptors[1].Buffer[0] ^= 1;
  return EFI_SUCCESS;
}

EFI_GPI_CR_PROTOCOL TestGpiProtocol = {
  .Revision = EFI_GPI_CR_PROTOCOL_REVISION,
  .I2cTransfer = GpiTransfer,
};

static EFI_STATUS EFIAPI Transfer (EFI_I2C_CR_PROTOCOL *This,
    MU_I2C_SLAVE_CONFIG *Config, MU_I2C_DESCRIPTOR *Descriptors,
    UINT16 Count, MU_I2C_CALLBACK Callback, VOID *Context, UINT32 Delay,
    UINT32 *Transferred) {
  UINT8 Register;
  assert(This == &Instances[1] && Config->SlaveAddress == 0x2b);
  assert(Config->BusFrequencyKHz == 400 && Config->Mode == MU_I2C_MODE_I2C);
  assert(Config->SlaveMaxClockStretchUs == 500);
  assert(Callback == NULL && Context == NULL && Delay == 0 && Transferred == NULL);
  assert(!InRawIrq);
  if(WorkerDuringConfigure) {
    UINTN Before=Transfers; EFI_TPL Saved=CurrentTpl;
    WorkerDuringConfigure=FALSE; CurrentTpl=TPL_CALLBACK; TimerNotify((VOID *)1,NULL); CurrentTpl=Saved;
    assert(Transfers==Before);
  }
  ++Transfers;
  if (FreezeInTransfer) {
    FreezeInTransfer=FALSE;
    assert(FrozenCallback && FrozenCallback(HandoffContext)==EFI_NOT_READY);
  }
  ProbeTransfer('F');
  if (Transfers == FailOnTransfer) return EFI_TIMEOUT;
  assert(Count == 1 || Count == 2);
  assert((Descriptors[0].Flags & (MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE)) ==
         (MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE));
  Register = Descriptors[0].Buffer[0];
  if (Count == 1) {
    assert(Descriptors[0].Length == 2 &&
           Descriptors[0].Flags == (MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE |
                                    MU_I2C_FLAG_STOP));
    if (Register == 0xff) {
      Page = Descriptors[0].Buffer[1];
      assert(Page == 0x80 || Page == 0x81 || Page == 0xd0 || Page == 0xb0);
    } else if(Page==0xb0) { assert(Locked && Register==0x22 && Descriptors[0].Buffer[1]==0); IrqStatus=0;
    } else if (Page == 0xd0) {
      assert(Locked);
      PageD0[Register] = Descriptors[0].Buffer[1];
    } else {
      assert(Register == 0xee && Page == 0x80);
      Locked = Descriptors[0].Buffer[1] == 1;
      if (!Locked) ++Unlocks;
      if (Locked && FailLockWriteAfterCommit) {
        FailLockWriteAfterCommit = FALSE;
        return EFI_TIMEOUT;
      }
    }
    return EFI_SUCCESS;
  }
  assert(Locked && ((Page == 0x81 && Register < 3) || Page == 0xd0 || (Page == 0xb0 && Register >= 0x21 && Register <= 0x23)));
  assert(Descriptors[0].Length == 1 &&
         Descriptors[0].Flags == (MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE));
  assert(Descriptors[1].Length == 1 &&
         Descriptors[1].Flags == (MU_I2C_FLAG_START | MU_I2C_FLAG_READ |
                                  MU_I2C_FLAG_STOP));
  if(Page==0xb0 && Register==0x23 && EdgeDuringService) {
    EdgeDuringService=FALSE; InRawIrq=TRUE; Lt9611IrqHandler(NULL); InRawIrq=FALSE;
  }
  if (Page == 0xb0 && Register == 0x21) ++FirmwareReads;
  Descriptors[1].Buffer[0] = Page == 0xb0 ? (Register==0x21 ? FirmwareVersion : Register==0x22 ? IrqStatus:HpdStatus) : Page == 0xd0 ? PageD0[Register] :
      (UINT8[]){0x17, 0x04, 0x92}[Register];
  if (Transfers == CorruptOnTransfer) Descriptors[1].Buffer[0] ^= 1;
  return EFI_SUCCESS;
}

#include "../Driver/Lt9611uxcCrDxe/lt9611.c"
#include "../Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.c"
#include "../Library/MdssHandoffCrLib/Handoff.c"

/* Independent test cases model separate firmware image instances. Production
   Entry is never retried after this image has registered its participant. */
static void NewHdkImage(void) {
  assert(mIrqTimer==NULL && mExitBootEvent==NULL && mI2cTestEvent==NULL);
  HandoffEnabled=TRUE; mHandoffFrozen=FALSE; mHandoffRegistered=FALSE;
  NativePanelSelection=0; mResidentRetained=FALSE;
  TestMdss.Soc=0x8450; TestQup.GpiCount=1;
  ExtendedEnabled=FALSE;memset(&mPanelOwner,0,sizeof(mPanelOwner));
  mStartupBusy=FALSE; mMcuBusy=FALSE; mBootServicesLive=FALSE; mIrqPending=FALSE;
  memset(&mNativeBridge,0,sizeof(mNativeBridge));
  FrozenCallback=NULL; SnapshotCallback=NULL; HandoffContext=NULL;
  RegistrationStatus=EFI_SUCCESS; FailGpioOperation=0; GpioOperations=0;
  FailOnTransfer=0; Page=0x80; Locked=FALSE; FirmwareVersion=0x40;
  CurrentTpl=TPL_APPLICATION; EdgeDuringService=FALSE; WorkerDuringConfigure=FALSE;
  FailCreate=FailTimer=FailInstall=FailIrqRegister=FALSE;
}

static void HandoffTests(const EFI_MDP_DSI_TIMING *Timing) {
  CR_MDSS_HANDOFF_PARTICIPANT Snapshot, Saved;
  CR_MDSS_NATIVE_HANDOFF Record;
  VOID *Map=(VOID*)0x1234;
  NewHdkImage(); RegistrationStatus=EFI_DEVICE_ERROR;
  UINTN Before=Transfers, BeforeEvents=CreatedEvents;
  assert(Lt9611EntryPoint(NULL,NULL)==EFI_DEVICE_ERROR && !GpioOperations &&
      Transfers==Before && CreatedEvents==BeforeEvents && !mHandoffRegistered);
  NewHdkImage(); BridgeConfig.I2cAddress=0x2a;
  assert(Lt9611EntryPoint(NULL,NULL)==EFI_UNSUPPORTED && !GpioOperations);
  BridgeConfig.I2cAddress=0x2b;
  const UINT32 AckedPins[6]={0,0,1,3,7,3};
  for(UINTN Failure=1;Failure<=6;++Failure) {
    NewHdkImage(); FailGpioOperation=Failure;
    UINTN BeforeInstalled=InstalledBridges, BeforeIrqs=RegisteredIrqs;
    assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS && mHandoffRegistered &&
        InstalledBridges==BeforeInstalled && RegisteredIrqs==BeforeIrqs &&
        GpioOperations==Failure && !mStartupBusy);
    memset(&Snapshot,0x5a,sizeof(Snapshot)); Saved=Snapshot;
    Map=(VOID*)0x1234;
    assert(SnapshotCallback(HandoffContext,&Snapshot,&Map)==EFI_NOT_READY &&
        !memcmp(&Snapshot,&Saved,sizeof(Snapshot)) && Map==(VOID*)0x1234);
    UINTN Closed=ClosedEvents;
    CurrentTpl=TPL_NOTIFY;
    assert(FrozenCallback(HandoffContext)==EFI_SUCCESS && CpuInterrupts &&
        ClosedEvents==Closed && !mIrqPending);
    assert(SnapshotCallback(HandoffContext,&Snapshot,&Map)==EFI_SUCCESS && !Map &&
        Snapshot.Owner==4 && Snapshot.Bridge.State==3 && Snapshot.Bridge.Flags==3 &&
        Snapshot.Bridge.HighPins==AckedPins[Failure-1] &&
        Snapshot.Bridge.I2cInstance==10 && Snapshot.Bridge.I2cAddress==0x2b &&
        Snapshot.Bridge.PowerGpio0==9 && Snapshot.Bridge.PowerGpio1==109 &&
        Snapshot.Bridge.ResetGpio==107 && Snapshot.Bridge.IrqGpio==44);
    memset(&Record,0,sizeof(Record));
    assert(CrMdssHandoffInit(&Record,1,2,0xfc400000,8192,31,31)==CR_SUCCESS &&
        CrMdssHandoffAppend(&Record,&Snapshot)==CR_SUCCESS);
    InRawIrq=TRUE;Lt9611IrqHandler(NULL);InRawIrq=FALSE;
    assert(!mIrqPending && FrozenCallback(HandoffContext)==EFI_SUCCESS);
    assert(Lt9611EntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);
    CurrentTpl=TPL_APPLICATION;
  }
  NewHdkImage();
  assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS && mHandoffRegistered);
  assert(mNativeBridge.State==CR_MDSS_NATIVE_UNCERTAIN && mNativeBridge.HighPins==7);
  assert(Lt9611ConfigureVideo(&mLt9611Bridge,Timing)==EFI_SUCCESS && mNativeBridge.State==1 && mNativeBridge.Flags==1);
  EFI_MDP_DSI_TIMING Invalid=*Timing;Invalid.DsiLanes=2;
  assert(Lt9611ConfigureVideo(&mLt9611Bridge,&Invalid)==EFI_UNSUPPORTED && mNativeBridge.State==1 && mNativeBridge.Flags==1);
  UINTN Closed=ClosedEvents, GpioBefore=GpioOperations, TimerBefore=TimerArms;
  Before=Transfers; CurrentTpl=TPL_NOTIFY;
  assert(FrozenCallback((VOID*)1)==EFI_INVALID_PARAMETER);
  mMcuBusy=TRUE;
  assert(FrozenCallback(HandoffContext)==EFI_NOT_READY && !mBootServicesLive && !mIrqPending);
  memset(&Snapshot,0x5a,sizeof(Snapshot)); Saved=Snapshot; Map=(VOID*)0x1234;
  assert(SnapshotCallback(HandoffContext,&Snapshot,&Map)==EFI_NOT_READY && !memcmp(&Snapshot,&Saved,sizeof(Snapshot)) && Map==(VOID*)0x1234);
  mMcuBusy=FALSE;
  assert(FrozenCallback(HandoffContext)==EFI_SUCCESS && ClosedEvents==Closed && CpuInterrupts);
  assert(SnapshotCallback(HandoffContext,&Snapshot,&Map)==EFI_SUCCESS && Snapshot.Bridge.State==1 && Snapshot.Bridge.Flags==1 && Snapshot.Bridge.HighPins==7);
  Saved=Snapshot;
  InRawIrq=TRUE;Lt9611IrqHandler(NULL);InRawIrq=FALSE;
  TimerNotify((VOID*)1,NULL); ReadyNotify((VOID*)3,NULL);
  assert(!mIrqPending && Transfers==Before && GpioOperations==GpioBefore &&
      ClosedEvents==Closed && TimerArms==TimerBefore);
  assert(Lt9611ConfigureVideo(&mLt9611Bridge,Timing)==EFI_NOT_READY && Transfers==Before);
  assert(SnapshotCallback(HandoffContext,&Snapshot,&Map)==EFI_SUCCESS && !memcmp(&Snapshot,&Saved,sizeof(Snapshot)));
  ExitNotify((VOID*)2,NULL);
  assert(ClosedEvents==Closed+3 && !mIrqTimer && !mExitBootEvent && !mI2cTestEvent);
  assert(SnapshotCallback(HandoffContext,&Snapshot,&Map)==EFI_SUCCESS && !memcmp(&Snapshot,&Saved,sizeof(Snapshot)));
  CurrentTpl=TPL_APPLICATION;
  /* A freeze interrupting an actual report programming call closes future
     I/O, refuses completion until its MCU owner drains, and retains the
     uncertain exact bridge journal instead of publishing a live result. */
  NewHdkImage();assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS);
  FreezeInTransfer=TRUE;
  assert(Lt9611ConfigureVideo(&mLt9611Bridge,Timing)==EFI_NOT_READY && !mMcuBusy && !mIrqPending);
  assert(FrozenCallback(HandoffContext)==EFI_SUCCESS);
  assert(SnapshotCallback(HandoffContext,&Snapshot,&Map)==EFI_SUCCESS && Snapshot.Bridge.State==3 && Snapshot.Bridge.Flags==3);
  ExitNotify((VOID*)2,NULL);
  assert(CreatedEvents==ClosedEvents);
  /* Actual opted-in adapter publishes the bridge-owned GPIO service. Its
     ordinary MCU path cannot steal that lease, and the IRQ worker never
     talks to an unpowered bridge. */
  NewHdkImage();ExtendedEnabled=TRUE;
  assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS&&mPanelOwner.Initialized&&mPanelOwner.Powered);
  int Owner;BOOLEAN Owned=FALSE;
  assert(mPanelOwner.Protocol.Acquire(&mPanelOwner.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
  Before=Transfers;
  assert(Lt9611ConfigureVideo(&mLt9611Bridge,Timing)==EFI_ACCESS_DENIED&&Transfers==Before);
  assert(mPanelOwner.Protocol.Reset(&mPanelOwner.Protocol,&Owner,FALSE)==EFI_SUCCESS&&!PinLevels[107]);
  mIrqPending=TRUE;Before=Transfers;
  TimerNotify((VOID*)1,NULL);assert(Transfers==Before&&mIrqPending&&TimerArmed);
  assert(mPanelOwner.Protocol.Reset(&mPanelOwner.Protocol,&Owner,TRUE)==EFI_SUCCESS&&PinLevels[107]);
  assert(mPanelOwner.Protocol.Prepare(&mPanelOwner.Protocol,&Owner,Timing)==EFI_SUCCESS&&Transfers>Before);
  assert(mPanelOwner.Protocol.Release(&mPanelOwner.Protocol,&Owner,&Owned)==EFI_SUCCESS&&!Owned&&!mPanelOwner.Powered);
  assert(!PinLevels[9]&&!PinLevels[109]&&!PinLevels[107]);Before=Transfers;
  TimerNotify((VOID*)1,NULL);assert(Transfers==Before&&TimerArmed);
  assert(mPanelOwner.Protocol.Acquire(&mPanelOwner.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned&&mPanelOwner.Powered);
  assert(mPanelOwner.Protocol.Prepare(&mPanelOwner.Protocol,&Owner,Timing)==EFI_SUCCESS);
  CurrentTpl=TPL_NOTIFY;assert(FrozenCallback(HandoffContext)==EFI_SUCCESS);CurrentTpl=TPL_APPLICATION;
  Before=Transfers;
  assert(mPanelOwner.Protocol.Ready(&mPanelOwner.Protocol,&Owner)==EFI_NOT_READY);
  TimerNotify((VOID*)1,NULL);assert(Transfers==Before);
  ExitNotify((VOID*)2,NULL);assert(CreatedEvents==ClosedEvents);
  puts("LT9611 MDSH: real live/partial GPIO journal, pure freeze, busy retention, post-freeze ISR/event/protocol fencing passed");
}

static void NativeRouteSetup(void) {
  NewHdkImage();HandoffEnabled=FALSE;NativePanelSelection=2;
  TestMdss.Soc=0x8350;TestQup.GpiCount=0;
  BridgeConfig.I2cInstance=Instances[1].Instance=15;BridgeConfig.InterruptPin=50;
  PowerPins[0]=47;PowerPins[1]=49;PowerPins[2]=48;
}
static void NativeRouteTests(const EFI_MDP_DSI_TIMING *Timing) {
  for(UINTN Failure=1;Failure<=6;++Failure){
    NativeRouteSetup();FailGpioOperation=Failure;
    UINTN Installed=InstalledBridges,Events=CreatedEvents;
    assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS&&mResidentRetained&&!mStartupBusy);
    assert(InstalledBridges==Installed&&CreatedEvents==Events&&!mPanelOwner.Initialized);
  }
  NativeRouteSetup();FailInstall=TRUE;
  assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS&&mResidentRetained&&mPanelOwner.Initialized);
  assert(!mIrqTimer&&!mExitBootEvent&&!mI2cTestEvent&&!mBootServicesLive&&mHandoffFrozen);
  assert(CreatedEvents==ClosedEvents);
  NativeRouteSetup();
  assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS&&mPanelOwner.Initialized&&mPanelOwner.RetainSupplies);
  assert(mPanelOwner.Pins[0]==47&&mPanelOwner.Pins[1]==49&&mPanelOwner.Pins[2]==48);
  UINTN BeforeGpi=GpiTransfers,BeforeTransfers=Transfers;
  CurrentTpl=TPL_CALLBACK;ReadyNotify((VOID*)3,NULL);CurrentTpl=TPL_APPLICATION;
  assert(GpiTransfers==BeforeGpi&&Transfers==BeforeTransfers+8&&!mI2cTestEvent);
  UINT32 Owner=1;BOOLEAN Owned=FALSE;
  assert(mPanelOwner.Protocol.Acquire(&mPanelOwner.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
  assert(mPanelOwner.Protocol.Prepare(&mPanelOwner.Protocol,&Owner,Timing)==EFI_SUCCESS);
  assert(mPanelOwner.Protocol.Release(&mPanelOwner.Protocol,&Owner,&Owned)==EFI_SUCCESS&&!Owned);
  assert(PinLevels[47]&&PinLevels[49]&&!PinLevels[48]);
  assert(mPanelOwner.Protocol.Acquire(&mPanelOwner.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
  CurrentTpl=TPL_NOTIFY;ExitNotify((VOID*)2,NULL);CurrentTpl=TPL_APPLICATION;
  assert(CreatedEvents==ClosedEvents&&!mBootServicesLive&&mHandoffFrozen);
  BeforeTransfers=Transfers;InRawIrq=TRUE;Lt9611IrqHandler(NULL);InRawIrq=FALSE;
  TimerNotify((VOID*)1,NULL);
  assert(Transfers==BeforeTransfers&&mPanelOwner.Protocol.Ready(&mPanelOwner.Protocol,&Owner)==EFI_NOT_READY);
  puts("LT9611: SM8350 I2C15 FIFO-only producer, copied GPIO47/49/48 route, retained supplies, partial startup and EBS passed");
}

int main (void) {
  EFI_I2C_CR_PROTOCOL *Selected = NULL;
  UINT8 Id[3] = {0};
  EFI_MDP_DSI_TIMING Timing = {
    .Width = 1920, .Height = 1080,
    .HFrontPorch = 88, .HBackPorch = 148, .HSync = 44,
    .VFrontPorch = 4, .VBackPorch = 36, .VSync = 5,
    .DsiLanes = 4, .DsiController = 0, .DsiRefreshRate = 60U << 16,
  };
  CR_DAL_LT9611_CONFIG Config = { .I2cInstance = 10, .I2cAddress = 0x2b };
  BootServices.RaiseTPL=RaiseTpl; BootServices.RestoreTPL=RestoreTpl;
  BootServices.CreateEvent=CreateEvent; BootServices.CreateEventEx=CreateEventEx;
  BootServices.SetTimer=SetTimer; BootServices.CloseEvent=CloseEvent;
  BootServices.LocateHandleBuffer = LocateI2cHandles;
  BootServices.HandleProtocol = HandleI2c;
  BootServices.LocateProtocol = LocateGpi;
  Instances[0].Revision = Instances[1].Revision = EFI_I2C_CR_PROTOCOL_REVISION;
  Instances[0].Instance = 1;
  Instances[1].Instance = 10;
  Instances[0].MuTransfer = Instances[1].MuTransfer = Transfer;

  assert(Lt9611FindI2c(10, &Selected) == EFI_SUCCESS);
  assert(Selected == &Instances[1]);
  assert(Lt9611FindI2c(9, &Selected) == EFI_NOT_FOUND);
  assert(Selected == NULL && Transfers == 0);
  AvailableHandles = 0;
  Selected = &Instances[1];
  assert(Lt9611FindI2c(10, &Selected) == EFI_NOT_FOUND);
  assert(Selected == NULL && Transfers == 0);
  AvailableHandles = 2;

  /* Actual HDK silicon revision: Linux does not require revision byte 05. */
  TraceProbe=TRUE;
  assert(Lt9611TestI2c(&Config) == EFI_SUCCESS);
  TraceProbe=FALSE;
  assert(Transfers == 16 && GpiTransfers == 8 && Unlocks == 3 && !Locked && Sleeps == 3);
  assert(ProbeCount == 24);
  for(UINTN i=0;i<24;i++) assert(ProbeSequence[i] == (i>=8 && i<16 ? 'G':'F'));
  /* Keep the following individual MCU-cleanup checks independently counted. */
  Transfers=8; Unlocks=2; Sleeps=2;
  assert(Lt9611ReadChipId(&Instances[1], 0x2b, Id) == EFI_SUCCESS);
  assert(Id[0] == 0x17 && Id[1] == 0x04 && Id[2] == 0x92);
  assert(Transfers == 16 && Unlocks == 3 && !Locked);

  FailOnTransfer = Transfers + 5;
  assert(Lt9611ReadChipId(&Instances[1], 0x2b, Id) == EFI_TIMEOUT);
  assert(Transfers == 23 && Unlocks == 4 && !Locked);
  assert(Sleeps == 4);

  FailOnTransfer = 0;
  FailLockWriteAfterCommit = TRUE;
  assert(Lt9611ReadChipId(&Instances[1], 0x2b, Id) == EFI_TIMEOUT);
  assert(Transfers == 27 && Unlocks == 5 && !Locked);
  assert(Sleeps == 5);

  FailOnTransfer = Transfers + 3;
  assert(Lt9611ReadChipId(&Instances[1], 0x2b, Id) == EFI_TIMEOUT);
  assert(Transfers == 32 && Unlocks == 6 && !Locked);
  assert(Sleeps == 6);

  /* Failed bank selection during cleanup must not write 0xee to bank 0x81. */
  FailOnTransfer = Transfers + 7;
  assert(Lt9611ReadChipId(&Instances[1], 0x2b, Id) == EFI_TIMEOUT);
  assert(Transfers == 39 && Unlocks == 6 && Locked && Page == 0x81);
  assert(Sleeps == 6);

  FailOnTransfer = 0;
  Page = 0x80;
  Locked = FALSE;
  PageD0[0x16] = 0xa7;
  PageD0[0x18] = 0xb8;
  PageD0[0x1a] = 0xc9;
  mBootServicesLive=TRUE;
  assert(Lt9611ConfigureVideo (&mLt9611Bridge, &Timing) == EFI_SUCCESS);
  assert(!Locked && Unlocks == 7 && Sleeps == 7);
  assert(PageD0[0x0d] == 0x04 && PageD0[0x0e] == 0x65);
  assert(PageD0[0x0f] == 0x04 && PageD0[0x10] == 0x38);
  assert(PageD0[0x11] == 0x08 && PageD0[0x12] == 0x98);
  assert(PageD0[0x13] == 0x07 && PageD0[0x14] == 0x80);
  assert(PageD0[0x15] == 5 && PageD0[0x17] == 44);
  assert(PageD0[0x19] == 4 && PageD0[0x1b] == 88);
  assert(PageD0[0x16] == 0xa0 && PageD0[0x18] == 0xb0 &&
         PageD0[0x1a] == 0xc0);
  Transfers = 0;
  Timing.DsiLanes = 2;
  assert(Lt9611ConfigureVideo (&mLt9611Bridge, &Timing) == EFI_UNSUPPORTED);
  assert(Transfers == 0);
  Timing.DsiLanes = 4;
  Timing.HBackPorch = MAX_UINT32;
  assert(Lt9611ConfigureVideo (&mLt9611Bridge, &Timing) == EFI_UNSUPPORTED);
  assert(Transfers == 0);
  Timing.HBackPorch = 148;
  FailOnTransfer = 6;
  assert(Lt9611ConfigureVideo (&mLt9611Bridge, &Timing) == EFI_TIMEOUT);
  assert(!Locked && Unlocks == 8);

  BootServices.InstallMultipleProtocolInterfaces = InstallBridge;
  TraceGpio = TRUE;
  FailOnTransfer = 0;
  Transfers = 0;
  EventCount = 0;
  GpioOperations = 0;
  FailGpioOperation = 5;
  assert(Lt9611EntryPoint(NULL, NULL) == EFI_DEVICE_ERROR);
  assert(Transfers == 0 && GpiTransfers == 8 && RegisteredIrqs == 0);
  assert(EventCount == 7 && EventKinds[6] == 'S');

  EventCount = 0;
  GpioOperations = 0;
  FailGpioOperation = 0;
  assert(Lt9611EntryPoint(NULL, NULL) == EFI_SUCCESS);
  /* EntryPoint only performs the FIFO probe and firmware check.  The GPI
     round-trip is deliberately deferred to the ReadyToBoot event so it
     cannot block MDSS dispatch. */
  assert(EventCount == 12 && Transfers == 14 && GpiTransfers == 8);
  assert(EventKinds[0] == 'C' && EventValues[0] == ((44U << 8) | GPIO_VALUE_UNCHANGE));
  assert(EventKinds[1] == 'C' && EventValues[1] == ((9U << 8) | 1));
  assert(EventKinds[2] == 'D' && EventValues[2] == 1000);
  assert(EventKinds[3] == 'C' && EventValues[3] == ((109U << 8) | 1));
  assert(EventKinds[4] == 'C' && EventValues[4] == ((107U << 8) | 1));
  assert(EventKinds[5] == 'D' && EventValues[5] == 20000);
  assert(EventKinds[6] == 'S' && EventValues[6] == (107U << 8));
  assert(EventKinds[7] == 'D' && EventValues[7] == 20000);
  assert(EventKinds[8] == 'S' && EventValues[8] == ((107U << 8) | 1));
  assert(EventKinds[9] == 'D' && EventValues[9] == 300000);
  assert(EventKinds[10] == 'D' && EventValues[10] == 50000);
  assert(EventKinds[11] == 'D' && EventValues[11] == 50000);
  assert(RegisteredIrqs == 1 && InstalledBridges == 1);
  TraceGpio=FALSE;
  assert(CreatedEvents==3 && !ClosedEvents && TimerNotify && ExitNotify && ReadyNotify);
  assert(TimerArmed && TimerDeadline > TimerTime);
  /* The independent GPI self-test runs only when ReadyToBoot is signalled. */
  {
    UINTN BeforeTransfers = Transfers;
    UINTN BeforeGpiTransfers = GpiTransfers;
    CurrentTpl = TPL_CALLBACK;
    ReadyNotify((VOID *)3, NULL);
    CurrentTpl = TPL_APPLICATION;
    assert(Transfers == BeforeTransfers + 16);
    assert(GpiTransfers == BeforeGpiTransfers + 8);
    assert(mI2cTestEvent == NULL);
    assert(ClosedEvents == 1);
  }
  /* Raw ISR only marks work. Callback drains chip IRQ outside raw context. */
  { UINTN Before=Transfers;
    InRawIrq=TRUE; Lt9611IrqHandler(NULL); InRawIrq=FALSE;
    assert(Transfers==Before && mIrqPending);
    mMcuBusy=TRUE; CurrentTpl=TPL_CALLBACK; TimerNotify((VOID *)1,NULL);
    assert(Transfers==Before && mIrqPending); mMcuBusy=FALSE;
    CurrentTpl=TPL_APPLICATION; WorkerDuringConfigure=TRUE;
    assert(Lt9611ConfigureVideo(&mLt9611Bridge,&Timing)==EFI_SUCCESS);
    assert(mIrqPending); CurrentTpl=TPL_CALLBACK;
    IrqStatus=3; HpdStatus=3; TimerNotify((VOID *)1,NULL);
    assert(!mIrqPending && !IrqStatus && !Locked && mLastHpdStatus==3);
    assert(TimerDeadline == TimerTime + 100000 && TimerArmed);
    Lt9611IrqHandler(NULL); FailOnTransfer=Transfers+4;
    TimerNotify((VOID *)1,NULL); assert(mIrqPending && !Locked);
    FailOnTransfer=0; TimerNotify((VOID *)1,NULL); assert(!mIrqPending);
    Lt9611IrqHandler(NULL); EdgeDuringService=TRUE;
    TimerNotify((VOID *)1,NULL); assert(mIrqPending);
    TimerNotify((VOID *)1,NULL); assert(!mIrqPending);
    /* Every failed service takes the 50 ms unlock delay. Model repeated
       expirations: each returns with a future deadline, never another
       queued callback that would trap DXE in its TPL_CALLBACK dispatcher. */
    for (UINTN attempt=0; attempt<3; attempt++) {
      UINT64 Started;
      UINTN Arms=TimerArms;
      Lt9611IrqHandler(NULL);
      TimerTime=TimerDeadline; TimerArmed=FALSE; Started=TimerTime;
      FailOnTransfer=Transfers+4;
      TimerNotify((VOID *)1,NULL);
      assert(mIrqPending && !Locked && TimerTime >= Started+500000);
      assert(TimerArms==Arms+1 && TimerArmed && TimerDeadline==TimerTime+100000);
    }
    FailOnTransfer=0;
    TimerNotify((VOID *)1,NULL); assert(!mIrqPending);
    CurrentTpl=TPL_NOTIFY; ExitNotify((VOID *)2,NULL); CurrentTpl=TPL_APPLICATION;
    assert(ClosedEvents==3 && TimerCancels==1 && !mBootServicesLive && !TimerArmed);
    Before=Transfers; InRawIrq=TRUE; Lt9611IrqHandler(NULL); InRawIrq=FALSE;
    TimerNotify((VOID *)1,NULL); assert(Transfers==Before);
  }
  FirmwareVersion=0;
  assert(Lt9611EntryPoint(NULL,NULL)==EFI_UNSUPPORTED);
  assert(RegisteredIrqs==1 && InstalledBridges==1 && !Locked);
  FirmwareVersion=0x20; /* pre-HPD firmware is still usable, as in Linux */
  mHandoffFrozen=FALSE; /* Independent pre-EBS low-level transport case. */
  assert(Lt9611CheckFirmware(&BridgeConfig)==EFI_SUCCESS && !Locked);
  for(UINTN index=1;index<=6;index++) {
    Page=0x80; Locked=FALSE; FailOnTransfer=Transfers+index;
    assert(Lt9611CheckFirmware(&BridgeConfig)==EFI_TIMEOUT);
    if(index<5) assert(!Locked);
  }
  FailOnTransfer=0; Page=0x80; Locked=FALSE;
  { UINTN Before=RegisteredIrqs;
    FailCreate=TRUE; assert(Lt9611EntryPoint(NULL,NULL)==EFI_OUT_OF_RESOURCES); FailCreate=FALSE;
    assert(RegisteredIrqs==Before);
    FailTimer=TRUE; assert(Lt9611EntryPoint(NULL,NULL)==EFI_DEVICE_ERROR); FailTimer=FALSE;
    assert(RegisteredIrqs==Before && !mIrqTimer && !mExitBootEvent);
    FailInstall=TRUE; assert(Lt9611EntryPoint(NULL,NULL)==EFI_DEVICE_ERROR); FailInstall=FALSE;
    assert(RegisteredIrqs==Before && !mIrqTimer && !mExitBootEvent);
    FailIrqRegister=TRUE; assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS); FailIrqRegister=FALSE;
    assert(RegisteredIrqs==Before+1 && mBootServicesLive && mIrqTimer);
    ExitNotify((VOID *)2,NULL);
    assert(CreatedEvents==ClosedEvents);
  }
  /* Every transfer in the FIFO probe and firmware check must fail entry
     before event setup, IRQ registration or bridge publication.  The version
     read has already completed for cleanup failures at transfers 5 and 6. */
  for(UINTN index=1;index<=6;index++) {
    UINTN BeforeFirmware=FirmwareReads, BeforeInstall=InstalledBridges;
    UINTN BeforeEvents=CreatedEvents, BeforeIrqs=RegisteredIrqs;
    Page=0x80; Locked=FALSE; FailOnTransfer=Transfers+8+index;
    assert(Lt9611EntryPoint(NULL,NULL)==EFI_TIMEOUT);
    assert(FirmwareReads==BeforeFirmware + ((index >= 5) ? 1 : 0) &&
           InstalledBridges==BeforeInstall);
    assert(CreatedEvents==BeforeEvents && RegisteredIrqs==BeforeIrqs);
  }
  FailOnTransfer=0; Page=0x80; Locked=FALSE;
  {
    UINTN BeforeFirmware=FirmwareReads, BeforeInstall=InstalledBridges;
    /* A failed FIFO revision read must still prevent bridge publication. */
    FailOnTransfer=Transfers+6; /* Third-byte read in the FIFO probe. */
    assert(Lt9611EntryPoint(NULL,NULL)==EFI_TIMEOUT);
    assert(FirmwareReads==BeforeFirmware && InstalledBridges==BeforeInstall && !Locked);
    FailOnTransfer=0;

    /* The deferred GPI self-test reports failures from its callback while
       preserving the already-published FIFO bridge for MDSS. */
    assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS);
    CorruptGpiId=TRUE;
    {
      UINTN BeforeTransfers=Transfers, BeforeGpi=GpiTransfers;
      ReadyNotify((VOID *)3,NULL);
      assert(Transfers==BeforeTransfers+8 && GpiTransfers==BeforeGpi+8);
      assert(mI2cTestEvent==NULL && !mMcuBusy);
    }
    CorruptGpiId=FALSE;
    ExitNotify((VOID *)2,NULL);

    assert(Lt9611EntryPoint(NULL,NULL)==EFI_SUCCESS);
    FailGpiTransfer=TRUE;
    {
      UINTN BeforeTransfers=Transfers, BeforeGpi=GpiTransfers;
      ReadyNotify((VOID *)3,NULL);
      assert(Transfers==BeforeTransfers+8 && GpiTransfers==BeforeGpi+1);
      assert(mI2cTestEvent==NULL && !mMcuBusy);
    }
    FailGpiTransfer=FALSE;
    ExitNotify((VOID *)2,NULL);
    assert(FirmwareReads >= BeforeFirmware && InstalledBridges >= BeforeInstall+2 && !Locked);
  }
  puts("LT9611: FIFO/GPI cleanup, firmware/timing, deferred IRQ and 50 ms timer starvation checks passed");
  HandoffTests(&Timing);
  NativeRouteTests(&Timing);
  return 0;
}
