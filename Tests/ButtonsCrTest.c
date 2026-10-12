/* Actual shared SPMI/OSKAL/PDC/Buttons/CC composition. Test boundaries: UEFI services, HardwareInterrupt2 and MMIO. SPDX-License-Identifier: MIT */
/* Actual SPMI, PMIC GPIO, button core and UEFI input adapter.
   Independent arbiter/PMIC and event model. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#define NULL ((void *)0)
#include "../Driver/ButtonsCrDxe/ButtonsCrDxe.c"


#define Acquire SP_Acquire
#define Add SP_Add
#define Exact SP_Exact
#define Exit SP_Exit
#define GetTypeC SP_GetTypeC
#define Release SP_Release
#define Stop SP_Stop
#define Work SP_Work
#define mConfig SP_mConfig
#define mExit SP_mExit
#define mFault SP_mFault
#define mHandle SP_mHandle
#define mLease SP_mLease
#define mPdcConfig SP_mPdcConfig
#define mPdcEnable SP_mPdcEnable
#define mProtocol SP_mProtocol
#define mPublished SP_mPublished
#define mQueued SP_mQueued
#define mRaw SP_mRaw
#define mRawOwned SP_mRawOwned
#define mRegistered SP_mRegistered
#define mRoute SP_mRoute
#define mRunning SP_mRunning
#define mSaved SP_mSaved
#define mSealed SP_mSealed
#define mShared SP_mShared
#define mSource SP_mSource
#define mSpiConfig SP_mSpiConfig
#define mTypeC SP_mTypeC
#define mTypeCStatus SP_mTypeCStatus
#define mWork SP_mWork
#ifndef CR_SPMI_DXE_SOURCE
#define CR_SPMI_DXE_SOURCE "../Driver/SpmiCrDxe/SpmiCrDxe.c"
#endif
#include CR_SPMI_DXE_SOURCE
#undef Acquire
#undef Add
#undef Exact
#undef Exit
#undef GetTypeC
#undef Release
#undef Stop
#undef Work
#undef mConfig
#undef mExit
#undef mFault
#undef mHandle
#undef mLease
#undef mPdcConfig
#undef mPdcEnable
#undef mProtocol
#undef mPublished
#undef mQueued
#undef mRaw
#undef mRawOwned
#undef mRegistered
#undef mRoute
#undef mRunning
#undef mSaved
#undef mSealed
#undef mShared
#undef mSource
#undef mSpiConfig
#undef mTypeC
#undef mTypeCStatus
#undef mWork


#define Data CC_Data
#define Exact CC_Exact
#define Exit CC_Exit
#define Get CC_Get
#define Power CC_Power
#define Service CC_Service
#define Stop CC_Stop
#define Wake CC_Wake
#define mBus CC_mBus
#define mClient CC_mClient
#define mExit CC_mExit
#define mHandle CC_mHandle
#define mPort CC_mPort
#define mProtocol CC_mProtocol
#define mProvider CC_mProvider
#define mPublished CC_mPublished
#define mQueued CC_mQueued
#define mRetry CC_mRetry
#define mRunning CC_mRunning
#define mStopped CC_mStopped
#define mTimer CC_mTimer
#define mWork CC_mWork
#include "../Driver/PmicCcCrDxe/PmicCcCrDxe.c"
#undef Data
#undef Exact
#undef Exit
#undef Get
#undef Power
#undef Service
#undef Stop
#undef Wake
#undef mBus
#undef mClient
#undef mExit
#undef mHandle
#undef mPort
#undef mProtocol
#undef mProvider
#undef mPublished
#undef mQueued
#undef mRetry
#undef mRunning
#undef mStopped
#undef mTimer
#undef mWork

EFI_GUID gEfiUsbTypeCCrProtocolGuid;
EFI_GUID gEfiCrSpmiProtocolGuid,gEfiCrFdtProtocolGuid;
STATIC BOOLEAN mPdcSaved;
EFI_GUID gEfiSimpleTextInProtocolGuid, gEfiSimpleTextInputExProtocolGuid;
EFI_GUID gEfiDevicePathProtocolGuid, gMsButtonServicesProtocolGuid, gEfiCrButtonStateProtocolGuid;
EFI_GUID gEfiEventExitBootServicesGuid;
STATIC EFI_BOOT_SERVICES mServices;
EFI_BOOT_SERVICES *gBS = &mServices;
STATIC SpmiDeviceContext Arb;
STATIC PdcDeviceContext Pdc;
STATIC UINT8 Regs[16][65536];
STATIC UINT16 Ppid[16];
STATIC UINT32 Channel[16][8], Observer[16][8], Pic[16];
STATIC UINT32 PdcCfg, PdcEn, SpiCfg;

STATIC UINT64 Time;
STATIC EFI_TPL Tpl;
STATIC UINT32 Reads, Writes, Notifies, Creates, Closes, FailCreate;
STATIC BOOLEAN FailUnregister, FailRegisterRetained, FailBusRead;
STATIC BOOLEAN ProviderCached, FailProvider;
STATIC UINT16 FailWriteAddress;
typedef struct {
  EFI_EVENT_NOTIFY Callback;
  VOID *Context;
  UINT32 Type;
  BOOLEAN Open, Signaled, Timed;
  UINT64 Due;
} EVENT;
STATIC EVENT Events[16];

VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D, 0, N); }
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) { return memset(D, V, N); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A, CONST CHAR8 *B) { return strcmp(A, B); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
VOID EFIAPI DebugAssert(CONST CHAR8 *F, UINTN L, CONST CHAR8 *D) {
  fprintf(stderr, "%s:%zu: %s\n", F, (size_t)L, D); assert(0);
}
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *Lock) { *Lock = 0; return Lock; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *Lock) { assert(*Lock == 0); *Lock = 1; return Lock; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *Lock) { assert(*Lock == 1); *Lock = 0; return Lock; }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P, UINT32 C, UINT32 E) {
  UINT32 Old = *P; if (Old == C) *P = E; return Old;
}
UINTN EFIAPI MicroSecondDelay(UINTN Delay) { return Delay; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return Time; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks) { return Ticks * 1000; }

STATIC UINT32 IrqStatus(UINT16 Apid) {
  return Regs[Ppid[Apid] >> 8][((Ppid[Apid] & 255) << 8) + 0x18] &
         Regs[Ppid[Apid] >> 8][((Ppid[Apid] & 255) << 8) + 0x15];
}
STATIC VOID Transfer(UINT16 Apid, BOOLEAN Read, UINT32 Command) {
  UINT16 Address = (UINT16)(((Ppid[Apid] & 255) << 8) | ((Command >> 4) & 255));
  UINT8 Sid = (UINT8)(Ppid[Apid] >> 8);
  UINT8 Length = (UINT8)((Command & 7) + 1);
  UINT8 Data[8];
  UINT32 *Ch = Read ? Observer[Apid] : Channel[Apid];
  assert(Tpl == TPL_HIGH_LEVEL);
  if ((Read && FailBusRead && (Address & 255) == 0x10) || (!Read && Address == FailWriteAddress)) {
    Ch[2] = 3; return;
  }
  if (Read) {
    Reads++; memset(Data, 0, sizeof(Data));
    memcpy(Data, &Regs[Sid][Address], Length);
    memcpy(&Ch[6], Data, 8);
  } else {
    Writes++; memcpy(Data, &Ch[4], 8);
    for (UINT8 I = 0; I < Length; I++) {
      UINT16 A = Address + I;
      if ((A & 255) == 0x14) Regs[Sid][(A & 0xff00) + 0x18] &= (UINT8)~Data[I];
      else if ((A & 255) == 0x15) Regs[Sid][A] |= Data[I];
      else if ((A & 255) == 0x16) Regs[Sid][A - 1] &= (UINT8)~Data[I];
      else Regs[Sid][A] = Data[I];
    }
  }
  Ch[2] = 1;
}

STATIC BOOLEAN V5Active,FailPdcRestore,WarnUnpublish;
STATIC UINT32 DropPdc;
STATIC UINTN MapV5(UINTN A){
 if(!V5Active)return A;
 if(A>=0xc440000&&A<0xc441100){UINTN O=A-0xc440000;return 0xc400000+(O>=0x900?O-0x900+0x2000:O);}
 if(A>=0xe600000&&A<0xe601000){UINTN O=A-0xe600000;return 0xc440000+(O/0x80)*32+O%0x80;}
 if(A>=0xc600000&&A<0xc700000){UINTN O=A-0xc600000;return 0xc500000+(O/0x10000)*4096+O%0x10000;}
 if(A>=0xe700000&&A<0xe700040)return 0xc4c0000+A-0xe700000;
 if(A>=0xc40a700&&A<0xc40a740)return 0xc42d000+A-0xc40a700;
 return A;
}

UINT32 EFIAPI MmioRead32(UINTN Address) {
 Address=MapV5(Address);assert(Address>=0x10000);
  UINTN Offset;
  if (Address == Pdc.BaseAddress + 0x114) return PdcCfg;
  if (Address == Pdc.BaseAddress + 0x10) return PdcEn;
  if (Address == Pdc.SPIConfigBase + 60) return SpiCfg;
  if (Address == 0xc400000) return V5Active?0x50000000:0x70010000;
  if (Address == 0xc400004) return 16;
  if (Address >= 0xc402000 && Address < 0xc402040) {
    UINT16 Id = (UINT16)((Address - 0xc402000) / 4);
    return Ppid[Id] == 0 ? 0 : ((UINT32)Ppid[Id] << 8) | (1U << 24);
  }
  if (Address >= 0xc42d000 && Address < 0xc42d040) return 0;
  if (Address == 0xc4c0000) {
    UINT32 Bits = 0;
    for (UINT16 Id = 0; Id < 16; Id++) if ((Pic[Id] & 1) && IrqStatus(Id)) Bits |= 1U << Id;
    return Bits;
  }
  if (Address >= 0xc440000 && Address < 0xc440200) {
    Offset = Address - 0xc440000; return Observer[Offset / 32][(Offset % 32) / 4];
  }
  if (Address >= 0xc500000 && Address < 0xc510000) {
    Offset = Address - 0xc500000;
    UINT16 Id = (UINT16)(Offset / 4096);
    if (Offset % 4096 == 0x100) return Pic[Id];
    if (Offset % 4096 == 0x104) return IrqStatus(Id);
    assert(Offset % 4096 < 32); return Channel[Id][(Offset % 4096) / 4];
  }
  fprintf(stderr, "unmodeled MMIO read %zx\n", (size_t)Address); assert(0); return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value) {
 Address=MapV5(Address);assert(Address>=0x10000);
  if(DropPdc==1&&Address==Pdc.BaseAddress+0x114&&(Value&7)==4)return Value;
  if(DropPdc==2&&Address==Pdc.BaseAddress+0x10&&(Value&2))return Value;
  if(DropPdc==3&&Address==Pdc.BaseAddress+0x10&&!(Value&2))return Value;
  if(DropPdc==4&&Address==Pdc.SPIConfigBase+60&&(Value&2))return Value;
  if (Address == Pdc.BaseAddress + 0x114) {if(!FailPdcRestore)PdcCfg=Value;}
  else if (Address == Pdc.BaseAddress + 0x10) PdcEn = Value;
  else if (Address == Pdc.SPIConfigBase + 60) SpiCfg = Value;
  else if (Address >= 0xc440000 && Address < 0xc440200) {
    UINTN Offset = Address - 0xc440000;
    Observer[Offset / 32][(Offset % 32) / 4] = Value;
    if (Offset % 32 == 0) Transfer((UINT16)(Offset / 32), TRUE, Value);
  } else if (Address >= 0xc500000 && Address < 0xc510000) {
    UINTN Offset = Address - 0xc500000;
    UINT16 Id = (UINT16)(Offset / 4096);
    if (Offset % 4096 == 0x100) Pic[Id] = Value;
    else if (Offset % 4096 == 0x108) { /* PIC clear follows QPNP latch clear. */ }
    else {
      assert(Offset % 4096 < 32); Channel[Id][(Offset % 4096) / 4] = Value;
      if (Offset % 4096 == 0) Transfer(Id, FALSE, Value);
    }
  } else { fprintf(stderr, "unmodeled MMIO write %zx\n", (size_t)Address); assert(0); }
  return Value;
}
UINT32 EFIAPI MmioAndThenOr32(UINTN A, UINT32 And, UINT32 Or) {
  return MmioWrite32(A, (MmioRead32(A) & And) | Or);
}
#include <Protocol/HardwareInterrupt2.h>
EFI_GUID gHardwareInterrupt2ProtocolGuid;
STATIC HARDWARE_INTERRUPT_HANDLER HwHandler;
STATIC BOOLEAN HwEnabled;
STATIC UINTN EoiCount;
STATIC EFI_HARDWARE_INTERRUPT2_PROTOCOL Hw;
STATIC EFI_STATUS EFIAPI HwRegister(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source,HARDWARE_INTERRUPT_HANDLER Handler){
 assert(This==&Hw&&Source==513&&Tpl==TPL_HIGH_LEVEL);
 if(!Handler&&FailUnregister)return EFI_DEVICE_ERROR;
 assert(!Handler||!HwHandler);HwHandler=Handler;HwEnabled=Handler!=NULL;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI HwEnable(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source){
 assert(This==&Hw&&Source==513&&Tpl==TPL_HIGH_LEVEL);HwEnabled=TRUE;return FailRegisterRetained?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI HwDisable(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source){
 assert(This==&Hw&&Source==513&&Tpl==TPL_HIGH_LEVEL);if(FailUnregister)return EFI_DEVICE_ERROR;HwEnabled=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI HwState(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source,BOOLEAN *State){
 assert(This==&Hw&&Source==513&&Tpl==TPL_HIGH_LEVEL);*State=HwEnabled;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI HwEoi(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source){
 assert(This==&Hw&&Source==513&&Tpl==TPL_HIGH_LEVEL);EoiCount++;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI HwTrigger(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source,EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE Type){
 assert(This==&Hw&&Source==513&&Tpl==TPL_HIGH_LEVEL&&Type==EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_HIGH);return EFI_SUCCESS;
}
STATIC EFI_HARDWARE_INTERRUPT2_PROTOCOL Hw={HwRegister,HwEnable,HwDisable,HwState,HwEoi,NULL,HwTrigger};
STATIC VOID Fire(VOID){assert(HwHandler&&HwEnabled);UINTN Count=EoiCount;EFI_SYSTEM_CONTEXT C={0};HwHandler(513,C);assert(EoiCount==Count+1&&Tpl==TPL_APPLICATION&&!HwEnabled);}

STATIC UINT32 IoRead(VOID *C, UINTN A) { (VOID)C; return MmioRead32(A); }
STATIC VOID IoWrite(VOID *C, UINTN A, UINT32 V) { (VOID)C; (VOID)MmioWrite32(A, V); }
STATIC VOID IoStall(VOID *C, UINT32 Us) { (VOID)C; (VOID)Us; }
STATIC CONST PmicGpioTargetController GpioController = {"pm8350", "qcom,pm8350-gpio", 1, 0x8800, 10};
STATIC CONST PmicGpioTargetContext GpioTarget = {&GpioController, 1};
STATIC ButtonTarget Buttons[] = {
  {"power", BUTTON_SOURCE_PON, BUTTON_CODE_POWER, "pmk8350", 0, 0x13, 0, 7, FALSE, 15000, CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"up", BUTTON_SOURCE_PMIC_GPIO, BUTTON_CODE_VOLUME_UP, "pm8350", 1, 0x8d, 6, 0, TRUE, 15000, CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"down", BUTTON_SOURCE_PON, BUTTON_CODE_VOLUME_DOWN, "pmk8350", 0, 0x13, 0, 6, FALSE, 15000, CR_INTERRUPT_TRIGGER_EDGE_BOTH},
};
STATIC CONST ButtonsTargetContext ButtonTargetData = {Buttons, ARRAY_SIZE(Buttons)};
SpmiDeviceContext *CrDalGetSpmiContext(VOID) { return &Arb; }
PdcDeviceContext *CrDalGetPdcContext(VOID) { return &Pdc; }
CONST ButtonsTargetContext *CrDalGetButtonsContext(VOID) { return &ButtonTargetData; }
CONST PmicGpioTargetContext *CrDalGetPmicGpioContext(VOID) { return &GpioTarget; }
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New) { EFI_TPL Old = Tpl; assert(New >= Old); Tpl = New; return Old; }
STATIC VOID EFIAPI Restore(EFI_TPL Old) { assert(Old <= Tpl); Tpl = Old; }
STATIC EFI_STATUS EFIAPI Create(UINT32 Type, EFI_TPL NotifyTpl, EFI_EVENT_NOTIFY Callback,
                                VOID *Context, EFI_EVENT *Event) {
  assert(NotifyTpl == TPL_CALLBACK || NotifyTpl == TPL_NOTIFY);
  if (++Creates == FailCreate) return EFI_OUT_OF_RESOURCES;
  for (UINTN I = 0; I < ARRAY_SIZE(Events); I++) if (!Events[I].Open) {
    Events[I] = (EVENT){.Callback = Callback, .Context = Context, .Type = Type, .Open = TRUE};
    *Event = &Events[I]; return EFI_SUCCESS;
  }
  assert(0); return EFI_OUT_OF_RESOURCES;
}
STATIC EFI_STATUS EFIAPI CreateEx(UINT32 Type, EFI_TPL L, EFI_EVENT_NOTIFY N, CONST VOID *C,
                                 CONST EFI_GUID *Group, EFI_EVENT *E) {
  assert(Group == &gEfiEventExitBootServicesGuid); return Create(Type, L, N, (VOID *)C, E);
}
STATIC EFI_STATUS EFIAPI Signal(EFI_EVENT Event) { EVENT *E = Event; assert(E->Open); E->Signaled = TRUE; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT Event) { EVENT *E = Event; assert(E->Open); E->Open = FALSE; Closes++; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Timer(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Due) {
  EVENT *E = Event; assert(E->Open && (E->Type & EVT_TIMER));
  E->Timed = Type != TimerCancel; E->Due = Time + Due / 10; return EFI_SUCCESS;
}

typedef struct {EFI_GUID *Guid;VOID *Object;} PUB;
STATIC PUB Published[16];
STATIC EFI_STATUS EFIAPI Publish(EFI_HANDLE *Handle,...){
 va_list A;va_start(A,Handle);EFI_GUID *G;
 while((G=va_arg(A,EFI_GUID *))){VOID *P=va_arg(A,VOID *);UINTN I;for(I=0;I<16&&Published[I].Guid;I++){}assert(I<16);Published[I]=(PUB){G,P};}
 va_end(A);*Handle=Handle;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Unpublish(EFI_HANDLE Handle,...){
 if(WarnUnpublish){WarnUnpublish=FALSE;return EFI_WARN_DELETE_FAILURE;}
 va_list A;va_start(A,Handle);EFI_GUID *G;
 while((G=va_arg(A,EFI_GUID *))){VOID *P=va_arg(A,VOID *);UINTN I;for(I=0;I<16;I++)if(Published[I].Guid==G&&Published[I].Object==P)break;assert(I<16);Published[I]=(PUB){0};}
 va_end(A);return EFI_SUCCESS;
}

STATIC VOID *FdtBlob;STATIC UINTN FdtBytes;
STATIC EFI_STATUS EFIAPI BlobGet(EFI_CR_FDT_PROTOCOL *P,CONST VOID **B,UINTN *N){(VOID)P;*B=FdtBlob;*N=FdtBytes;return EFI_SUCCESS;}
STATIC EFI_CR_FDT_PROTOCOL FdtProvider={.Revision=EFI_CR_FDT_PROTOCOL_REVISION,.Size=sizeof(EFI_CR_FDT_PROTOCOL),.GetBlob=BlobGet};
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){
 (VOID)R;if(G==&gHardwareInterrupt2ProtocolGuid){assert(Tpl<=TPL_NOTIFY);if(FailProvider)return EFI_NOT_FOUND;*P=&Hw;return EFI_SUCCESS;}if(G==&gEfiCrFdtProtocolGuid&&FdtBlob){*P=&FdtProvider;return EFI_SUCCESS;}for(UINTN I=0;I<16;I++)if(Published[I].Guid==G){*P=Published[I].Object;return EFI_SUCCESS;}return EFI_NOT_FOUND;
}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
STATIC VOID Pump(VOID) {
  for (UINTN Pass = 0; Pass < 10; Pass++) {
    BOOLEAN Ran = FALSE;
    for (UINTN I = 0; I < ARRAY_SIZE(Events); I++) {
      EVENT *E = &Events[I];
      if (!E->Open || (E->Type & EVT_NOTIFY_WAIT)) continue;
      if (E->Timed && Time >= E->Due) { E->Timed = FALSE; E->Signaled = TRUE; }
      if (E->Signaled) {
        E->Signaled = FALSE; Tpl = TPL_CALLBACK;
        E->Callback(E, E->Context); Tpl = TPL_APPLICATION; Ran = TRUE;
      }
    }
    if (!Ran) return;
  }
  assert(0);
}
STATIC VOID Advance(UINT64 Us) { Time += Us; Pump(); }
STATIC VOID Press(UINTN Index, BOOLEAN Down) {
  CONST ButtonTarget *B = &Buttons[Index]; UINT16 Base = (UINT16)(B->Peripheral << 8);
  if (B->Source == BUTTON_SOURCE_PON) {
    if (Down) Regs[B->Sid][Base + 0x10] |= (UINT8)(1U << B->Irq);
    else Regs[B->Sid][Base + 0x10] &= (UINT8)~(1U << B->Irq);
  } else Regs[B->Sid][Base + 0x10] = !Down;
  Regs[B->Sid][Base + 0x18] |= (UINT8)(1U << B->Irq);
  assert(HwHandler != NULL && (PdcEn & 2));
  Fire();
  Pump();
  assert((Regs[B->Sid][Base + 0x18] & (1U << B->Irq)) == 0);
}
STATIC VOID Setup(VOID) {
  assert(HwHandler == NULL);
  memset(&Arb, 0, sizeof(Arb)); memset(&mButtons, 0, sizeof(mButtons));
  memset(Regs, 0, sizeof(Regs)); memset(Events, 0, sizeof(Events));
  memset(Observer, 0, sizeof(Observer)); memset(Channel, 0, sizeof(Channel)); memset(Pic, 0, sizeof(Pic));
  memset(mKeyNotifies, 0, sizeof(mKeyNotifies));
  mPdcSaved = mPublished = mServicing = mNotifying = FALSE;
  mStopped = TRUE; mNotifyHead = mNotifyCount = 0;
  mBootUp = mBootDown = FALSE;
  Time = Reads = Writes = Creates = Closes = FailCreate = 0; Tpl = TPL_APPLICATION;
  FailUnregister = FailRegisterRetained = FailBusRead = FALSE; FailWriteAddress = 0;
  ProviderCached = FailProvider = FALSE;
  Pdc.Size=0x10000;Pdc.SPIConfigSize=0x64;Pdc.PdcRangeCount=1;Pdc.PdcRegion[0]=(PdcRange){0,480,94};Pdc.BaseAddress = 0xb220000; Pdc.SPIConfigBase = 0x174000f0;
  PdcCfg = 0xa; PdcEn = 0x10; SpiCfg = 8;
  Arb.Regions[SPMI_MEMORY_REGION_TYPE_CORE] = (SpmiMemoryRegion){0xc400000, 0x3000};
  Arb.Regions[SPMI_MEMORY_REGION_TYPE_CH_SLAVES] = (SpmiMemoryRegion){0xc500000, 0x400000};
  Arb.Regions[SPMI_MEMORY_REGION_TYPE_OBSERVER] = (SpmiMemoryRegion){0xc440000, 0x80000};
  Arb.Regions[SPMI_MEMORY_REGION_TYPE_INTERRUPT] = (SpmiMemoryRegion){0xc4c0000, 0x10000};
  Arb.Regions[SPMI_MEMORY_REGION_TYPE_CONFIG] = (SpmiMemoryRegion){0xc42d000, 0x10000};
  Arb.PdcPinNumber = 1; Arb.HasInterrupt = TRUE;
  Arb.Io = (SpmiIoOps){IoRead, IoWrite, IoStall, NULL};
  memset(Ppid, 0, sizeof(Ppid)); Ppid[1] = 0x13;
  Regs[0][0x1305] = 9; Regs[0][0x1311] = 1; Regs[0][0x1312] = 2;
  Regs[0][0x1313] = 4; Regs[0][0x1315] = 1;
  Regs[0][0x135a] = 0xf1; Regs[0][0x135b] = 0x80; Regs[0][0x1370] = 3; Regs[0][0x1371] = 7;
  for (UINT16 I = 0; I < 10; I++) {
    UINT16 Base = (UINT16)(0x8800 + I * 0x100); Ppid[2 + I] = (UINT16)(0x188 + I);
    /* GPIO_LV_VIN2 supports the board's VIN1 selection. */
    Regs[1][Base + 4] = 0x10; Regs[1][Base + 5] = 0x12;
    Regs[1][Base + 0x10] = 1; Regs[1][Base + 0x41] = 1;
    Regs[1][Base + 0x42] = 5; Regs[1][Base + 0x46] = 0x80;
  }
  mServices = (EFI_BOOT_SERVICES){.RaiseTPL = Raise, .RestoreTPL = Restore,
    .CreateEvent = Create, .CreateEventEx = CreateEx, .SignalEvent = Signal,
    .CloseEvent = Close, .SetTimer = Timer, .InstallMultipleProtocolInterfaces = Publish,
    .UninstallMultipleProtocolInterfaces = Unpublish,.LocateProtocol=Locate};
}
STATIC EFI_STATUS EFIAPI KeyNotification(EFI_KEY_DATA *Key) {
  assert(Tpl == TPL_CALLBACK && Key->Key.ScanCode == SCAN_UP);
  assert(ButtonsCrUnload(NULL) == EFI_ACCESS_DENIED && HwHandler != NULL && Closes == 0);
  Notifies++; return EFI_SUCCESS;
}
STATIC VOID Expect(UINT16 Scan, CHAR16 Unicode) {
  EFI_INPUT_KEY Key;
  assert(mInput.ReadKeyStroke(&mInput, &Key) == EFI_SUCCESS);
  assert(Key.ScanCode == Scan && Key.UnicodeChar == Unicode);
}

STATIC VOID CheckUi(VOID){
 EFI_KEY_DATA Key={0};EFI_INPUT_KEY Plain;VOID *Handle,*Duplicate;UINT32 IdleReads;
  assert(mInput.ReadKeyStroke(&mInput, &Plain) == EFI_NOT_READY);
  UINT32 Raw=0xfeed; UINT16 OldCount=mButtons.Count;
  assert(RawState(&mButtonState,&Raw)==EFI_SUCCESS && Raw==0 && mButtons.Count==OldCount);
  Press(0,TRUE); /* direct level available before debounce, without key/repeat */
  assert(RawState(&mButtonState,&Raw)==EFI_SUCCESS && Raw==CR_BUTTON_STATE_POWER);
  assert(mButtons.Count==OldCount);Press(0,FALSE);Advance(20000);
  FailBusRead=TRUE;Raw=0xfeed;
  assert(RawState(&mButtonState,&Raw)==EFI_DEVICE_ERROR && Raw==0xfeed);FailBusRead=FALSE;
  Tpl=TPL_NOTIFY;assert(RawState(&mButtonState,&Raw)==EFI_NOT_READY);Tpl=TPL_APPLICATION;
  EFI_CR_BUTTON_SNAPSHOT Snapshot={0};
  assert(RawSnapshot(&mButtonState,&Snapshot)==EFI_SUCCESS && Snapshot.Pressed==0);
  UINT64 BeforeTap=Snapshot.PowerPressSequence;
  UINT16 BeforeQueue=mButtons.Count;
  /* Entire 5ms press/release occurs between UI samples. IRQ-woken service
     observes it, while debounce correctly emits no navigation keystroke. */
  Press(0,TRUE);Advance(5000);Press(0,FALSE);Advance(20000);
  assert(RawSnapshot(&mButtonState,&Snapshot)==EFI_SUCCESS && Snapshot.Pressed==0 &&
         Snapshot.PowerPressSequence==BeforeTap+1 && mButtons.Count==BeforeQueue);
  UINT64 AfterTap=Snapshot.PowerPressSequence;
  assert(RawSnapshot(&mButtonState,&Snapshot)==EFI_SUCCESS && Snapshot.PowerPressSequence==AfterTap);
  Press(0,TRUE);assert(RawSnapshot(&mButtonState,&Snapshot)==EFI_SUCCESS);
  UINT64 HeldSequence=Snapshot.PowerPressSequence;
  for(UINTN I=0;I<10;++I)assert(RawSnapshot(&mButtonState,&Snapshot)==EFI_SUCCESS && Snapshot.PowerPressSequence==HeldSequence);
  Press(0,FALSE);Advance(20000);
  FailBusRead=TRUE;memset(&Snapshot,0xa5,sizeof(Snapshot));EFI_CR_BUTTON_SNAPSHOT SavedSnapshot=Snapshot;
  assert(RawSnapshot(&mButtonState,&Snapshot)==EFI_DEVICE_ERROR && !memcmp(&Snapshot,&SavedSnapshot,sizeof(Snapshot)));
  FailBusRead=FALSE;
  IdleReads = Reads;
  for (UINTN I = 0; I < 1000; I++) Advance(1000000);
  assert(Reads == IdleReads && !((EVENT *)mTimer)->Timed); /* No idle polling. */
  Key.Key.ScanCode = SCAN_UP;
  assert(RegisterNotify(&mInputEx, &Key, KeyNotification, &Handle) == EFI_SUCCESS);
  assert(RegisterNotify(&mInputEx, &Key, KeyNotification, &Duplicate) == EFI_SUCCESS && Duplicate == Handle);
  Press(1, TRUE); Advance(5000); Press(1, FALSE); Advance(20000);
  assert(ReadKey(&mInput, &Plain) == EFI_NOT_READY); /* Short bounce rejected. */
  Press(1, TRUE); Advance(20000); Expect(SCAN_UP, 0); assert(Notifies == 1);
  WaitKey(mInput.WaitForKey, NULL); assert(!((EVENT *)mInput.WaitForKey)->Signaled);
  Advance(480000); assert(ReadKey(&mInput, &Plain) == EFI_NOT_READY);
  Advance(20000); Expect(SCAN_UP, 0);
  Advance(3000000); Expect(SCAN_UP, 0); assert(ReadKey(&mInput, &Plain) == EFI_NOT_READY);
  Press(1, FALSE); Advance(20000);
  assert(UnregisterNotify(&mInputEx, Handle) == EFI_SUCCESS);
  assert(UnregisterNotify(&mInputEx, Handle) == EFI_INVALID_PARAMETER);
  Press(2, TRUE); Advance(20000); Expect(SCAN_DOWN, 0); Press(2, FALSE); Advance(20000);
  Press(0, TRUE); Advance(20000);
  WaitKey(mInputEx.WaitForKeyEx, NULL); assert(((EVENT *)mInputEx.WaitForKeyEx)->Signaled);
  assert(ReadEx(&mInputEx, &Key) == EFI_SUCCESS && Key.Key.ScanCode == 0 && Key.Key.UnicodeChar == '\r');
  assert(Key.KeyState.KeyShiftState == EFI_SHIFT_STATE_VALID);
  Advance(5000000); assert(ReadKey(&mInput, &Plain) == EFI_NOT_READY); /* No power repeat. */
  Press(0, FALSE); Advance(20000);
  Press(1, TRUE); Advance(20000); assert(Reset(&mInput, FALSE) == EFI_SUCCESS);
  Advance(1000000); assert(ReadKey(&mInput, &Plain) == EFI_NOT_READY); /* Reset-held suppression. */
  Press(1, FALSE); Advance(20000);
  for (UINTN I = 0; I < 30; I++) { Press(0, TRUE); Advance(20000); Press(0, FALSE); Advance(20000); }
  for (UINTN I = 0; I < CR_BUTTON_QUEUE_SIZE; I++) Expect(0, '\r');
  assert(ReadKey(&mInput, &Plain) == EFI_NOT_READY);
  FailBusRead = TRUE; Press(2, TRUE); Advance(20000);
  assert(ReadKey(&mInput, &Plain) == EFI_NOT_READY); FailBusRead = FALSE;
  Advance(1000000); Advance(20000); Expect(SCAN_DOWN, 0); Press(2, FALSE); Advance(20000);

}

int main(int argc,char **argv){
 assert(argc==3);int mode=atoi(argv[1]);Setup();V5Active=TRUE;
 BOOLEAN PriorExternal=(mode==29),PriorParentScan=(mode==29);
 if(mode==30){Buttons[0].Irq=0;Buttons[2].Irq=1;Regs[0][0x1305]=4;Regs[0][0x1315]=0;}
 if(mode==31)Pdc.SPIConfigBase=0;
 Arb.InterruptExternallyManaged=PriorExternal;Arb.SharedParentIrqScan=PriorParentScan;
 FILE *F=fopen(argv[2],"rb");assert(F);fseek(F,0,SEEK_END);long Bytes=ftell(F);rewind(F);
 FdtBlob=malloc((size_t)Bytes);assert(FdtBlob&&fread(FdtBlob,1,(size_t)Bytes,F)==(size_t)Bytes);fclose(F);FdtBytes=(UINTN)Bytes;
 UINTN R[]={0xc440000,0x1100,0xc600000,0x2000000,0xe600000,0x100000,0xe700000,0xa0000,0xc40a000,0x26000};
 for(UINTN I=0;I<5;I++)Arb.Regions[I]=(SpmiMemoryRegion){R[I*2],R[I*2+1]};
 Ppid[12]=0x201;Ppid[13]=0x215;Ppid[14]=0x217;Regs[2][0x103]=2;Regs[2][0x104]=0x51;Regs[2][0x105]=0x20;
 
 if(mode>=17&&mode<=20){DropPdc=mode-16;if(mode==19)PdcEn|=2;
  assert(SpmiCrEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR&&!SP_mPublished&&!HwHandler&&!Arb.Initialized&&!SP_mSaved);
  free(FdtBlob);puts("dropped startup PDC/SPI write refuses publication and retires safely PASS");return 0;}
 if(mode==6){FailProvider=TRUE;assert(SpmiCrEntryPoint(NULL,NULL)==EFI_NOT_FOUND&&!SP_mWork&&!SP_mExit&&!Arb.Initialized&&Closes==2);free(FdtBlob);puts("pre-hardware provider refusal closes events PASS");return 0;}
 if(mode==7){Pdc.Size=0x114;assert(SpmiCrEntryPoint(NULL,NULL)==EFI_INVALID_PARAMETER&&!SP_mSaved&&!Arb.Initialized&&Closes==2);free(FdtBlob);puts("PDC bounds before snapshot PASS");return 0;}
 if(mode==12){FailRegisterRetained=FailUnregister=TRUE;assert(SpmiCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&!SP_mPublished&&HwHandler&&Arb.Initialized);FailRegisterRetained=FailUnregister=FALSE;
 if(mode==11){FailPdcRestore=TRUE;assert(SpmiCrUnload(NULL)==EFI_DEVICE_ERROR&&SP_mSaved&&!HwHandler&&!Arb.Initialized);FailPdcRestore=FALSE;}
 assert(SpmiCrUnload(NULL)==EFI_SUCCESS&&!HwHandler&&!Arb.Initialized);
 assert(PdcCfg==0xa&&PdcEn==0x10&&SpiCfg==8);
 assert(Arb.InterruptExternallyManaged==PriorExternal&&Arb.SharedParentIrqScan==PriorParentScan);
 free(FdtBlob);puts("real OSKAL partial registration retained until retirement PASS");return 0;}
 Ppid[15]=0x216;
 assert(SpmiCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&SP_mPublished&&SP_mTypeCStatus==EFI_SUCCESS);
 assert(Arb.InterruptExternallyManaged&&Arb.SharedParentIrqScan);
 if(mode==10){Pic[15]=1;Regs[2][0x1615]=Regs[2][0x1618]=1;Fire();Pump();assert(SP_mSealed&&!HwEnabled&&Regs[2][0x1618]==1);assert(SpmiCrUnload(NULL)==EFI_SUCCESS);free(FdtBlob);puts("unsubscribed parent source seals without PMIC latch loss PASS");return 0;}
 
 assert(Arb.Version==SPMI_ARB_VERSION_5);
 
 if(mode>=21&&mode<=26){FailCreate=Creates+(UINT32)(mode-20);assert(ButtonsCrEntryPoint(NULL,NULL)==EFI_OUT_OF_RESOURCES&&!mPublished&&!mClient.Provider);assert(SpmiCrUnload(NULL)==EFI_SUCCESS);free(FdtBlob);puts("each button event creation partial failure retires PASS");return 0;}
 if(mode==14){FailWriteAddress=0x8d42;assert(ButtonsCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&!mPublished&&mButtons.Initialized);FailWriteAddress=0;assert(ButtonsCrUnload(NULL)==EFI_SUCCESS);assert(SpmiCrUnload(NULL)==EFI_SUCCESS);free(FdtBlob);puts("retained GPIO restoration PASS");return 0;}
 if(mode==15)Regs[1][0x8d10]=0;
 assert(ButtonsCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&mPublished);
 if(mode==15){BOOLEAN Held;EFI_INPUT_KEY Key;assert(BootUp(&mButtonServices,&Held)==EFI_SUCCESS&&Held);assert(ReadKey(&mInput,&Key)==EFI_NOT_READY);assert(BootClear(&mButtonServices)==EFI_SUCCESS);assert(BootUp(&mButtonServices,&Held)==EFI_SUCCESS&&!Held);Press(1,FALSE);Advance(20000);Press(1,TRUE);Advance(20000);Expect(SCAN_UP,0);Press(1,FALSE);Advance(20000);}

 assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&CC_mPublished);
 if(mode==32){
  /* Parent is masked with a queued, undrained event. Failed guard admission
     must revoke cached clients without touching PMIC latches or IRQ owners. */
  Regs[2][0x1508]=8;Regs[2][0x1509]=Regs[2][0x150a]=0x20;Regs[2][0x150b]=0x69;Regs[2][0x1518]=0x20;Fire();Pump();
  CR_TYPEC_NON_PD_HOST_WITNESS W;assert(Witness(&CC_mProtocol,&W)==EFI_SUCCESS);
  Regs[2][0x1518]=0x20;Fire();assert(SP_mQueued&&!HwEnabled);
  UINT32 R=Reads,V=Writes;gBS=NULL;SP_Work(SP_mWork,NULL);gBS=&mServices;
  assert(SP_mSealed&&SP_mShared.Closed&&SP_mQueued&&!HwEnabled&&HwHandler&&Reads==R&&Writes==V&&Regs[2][0x1518]==0x20);
  assert(Witness(&CC_mProtocol,&W)==EFI_NOT_READY);Pump();
  assert(PmicCcCrUnload(NULL)==EFI_SUCCESS&&ButtonsCrUnload(NULL)==EFI_SUCCESS&&SpmiCrUnload(NULL)==EFI_SUCCESS);
  free(FdtBlob);puts("actual shared SPMI guard failure seals cached CC witness, retaining masked IRQ, exact claims and PMIC latch PASS");return 0;
 }
 if(mode==13||mode==30||mode==31)CheckUi();
 EFI_USB_TYPEC_STATE State;assert(CC_Get(&CC_mProtocol,&State)==EFI_SUCCESS&&!State.Connected);
 Regs[2][0x1508]=8;Regs[2][0x1509]=0x20;Regs[2][0x150b]=0x49;Regs[2][0x1518]=0x20;
 Fire();Pump();
 assert(CC_Get(&CC_mProtocol,&State)==EFI_SUCCESS&&State.Connected&&State.DataRole==UsbTypeCHost);
 assert(CC_Data(&CC_mProtocol,UsbTypeCDevice)==EFI_UNSUPPORTED&&CC_Power(&CC_mProtocol,FALSE)==EFI_UNSUPPORTED);
 if(mode==16){
  WarnUnpublish=TRUE;assert(ButtonsCrUnload(NULL)==EFI_NOT_READY&&mPublished&&!mClient.Provider);
  VOID *Still=NULL;assert(Locate(&gEfiSimpleTextInProtocolGuid,NULL,&Still)==EFI_SUCCESS&&Still==&mInput);
 }else if(mode==27){
  SP_Exit(NULL,NULL);CC_Exit(NULL,NULL);ExitBootServices(NULL,NULL);
  EFI_INPUT_KEY Key;assert(ReadKey(&mInput,&Key)==EFI_NOT_READY&&CC_Get(&CC_mProtocol,&State)==EFI_NOT_READY);
  assert(PmicCcCrUnload(NULL)==EFI_ACCESS_DENIED&&CC_mClient.Provider&&CC_mPort.Stopping&&!CC_mPort.WitnessFlags);
  free(FdtBlob);puts("actual shared provider/PMIC EBS closes cached admission and retains exact observer claims PASS");return 0;
 }else if(mode==28){
  FailWriteAddress=0x1514;Regs[2][0x1518]=0x20;Fire();Pump();
  assert(SP_mSealed&&!HwEnabled&&Arb.IrqAckFailures==1&&Regs[2][0x1518]==0x20);
  assert(CC_Get(&CC_mProtocol,&State)==EFI_NOT_READY);FailWriteAddress=0;
 }else if(mode==8){
  FailUnregister=TRUE;assert(SpmiCrUnload(NULL)==EFI_DEVICE_ERROR&&HwHandler&&SP_mRegistered);
  assert(Arb.InterruptExternallyManaged&&Arb.SharedParentIrqScan);
  assert(CC_Get(&CC_mProtocol,&State)==EFI_NOT_READY);FailUnregister=FALSE;
 }else if(mode==9){
  Pic[15]=1;Regs[2][0x1615]=Regs[2][0x1618]=1;Fire();Pump();
  assert(SP_mSealed&&!HwEnabled&&Regs[2][0x1618]==1&&CC_Get(&CC_mProtocol,&State)==EFI_NOT_READY);
 }else if(mode==1){
  assert(ButtonsCrUnload(NULL)==EFI_SUCCESS&&HwHandler);
  Regs[2][0x150b]=0;Regs[2][0x1509]=0;Regs[2][0x1518]=0x20;
  Fire();Pump();
  assert(CC_Get(&CC_mProtocol,&State)==EFI_SUCCESS&&!State.Connected);
 }else if(mode==2){
  assert(PmicCcCrUnload(NULL)==EFI_SUCCESS&&HwHandler);
  Press(0,TRUE);Advance(20000);Expect(0,'\r');Press(0,FALSE);Advance(20000);
 }else if(mode==3){
  Regs[2][0x150b]=0x89;Regs[2][0x1518]=4;
  Fire();Pump();
  for(UINTN I=0;I<4;I++)Advance(1000);
  assert(CC_Get(&CC_mProtocol,&State)==EFI_NOT_READY&&!((EVENT *)CC_mTimer)->Timed);
 }else if(mode==4){
  assert(SpmiCrUnload(NULL)==EFI_ACCESS_DENIED);
  assert(CC_Get(&CC_mProtocol,&State)==EFI_NOT_READY);
 }else if(mode==5){
  Regs[2][0x1518]=4;Fire();
  assert(SpmiCrUnload(NULL)==EFI_NOT_READY);Pump();
 }else{
  UINTN Before=Reads;for(UINTN I=0;I<100;I++)Advance(1000000);assert(Reads==Before);
 }
 assert(PmicCcCrUnload(NULL)==EFI_SUCCESS);assert(ButtonsCrUnload(NULL)==EFI_SUCCESS);
 
 if(mode==11){FailPdcRestore=TRUE;assert(SpmiCrUnload(NULL)==EFI_DEVICE_ERROR&&SP_mSaved&&!HwHandler&&!Arb.Initialized);FailPdcRestore=FALSE;}
 assert(SpmiCrUnload(NULL)==EFI_SUCCESS&&!HwHandler&&!Arb.Initialized);
 assert(PdcCfg==0xa&&PdcEn==0x10&&SpiCfg==8);
 assert(Arb.InterruptExternallyManaged==PriorExternal&&Arb.SharedParentIrqScan==PriorParentScan);
 
 free(FdtBlob);puts("actual FDT -> SpmiCr V5 -> shared buttons/CC -> TypeC protocol PASS");return 0;
}
