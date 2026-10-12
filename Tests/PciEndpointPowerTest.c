/* HDK WCN6855 endpoint power, sharing and fault regressions. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#define NULL ((VOID *)0)
#include "../Library/PciHostBridgeLib/PciHostBridgeLib.c"
#include "../Driver/RpmhCrDxe/RpmhCrDxe.c"

/* Use both production adapters: only the GPIO and RPMh wire operations below
   are mocked. This catches parent-supply conflicts hidden by PCIe core mocks. */
STATIC CONST CHAR8 *CONST RailNames[] = {
  "smpb10", "smpb11", "smpb12", "smpe2", "smpc1", "ldob6", "ldob1",
  "ldob5", "ldoh2", "ldob7", "ldob9"
};
STATIC CONST UINT32 WcnMillivolts[] = {1800, 968, 1352, 852, 1856};
STATIC UINT32 RailVoltage[ARRAY_SIZE(RailNames)];
STATIC UINT8 RailMode[ARRAY_SIZE(RailNames)];
STATIC BOOLEAN RailEnabled[ARRAY_SIZE(RailNames)];
STATIC GpioConfigParams Pins[211];
STATIC UINT64 NowUs, XoRiseUs, WlanRiseUs;
STATIC UINTN RailCalls, FailRail, GpioWrites, FailGpio, GpioReads, FailRead;
STATIC UINTN WlanRises, XoRises;
STATIC BOOLEAN LostGpioReadback;
STATIC RpmhDeviceContext Backend;
STATIC EFI_CMD_DB_PROTOCOL CmdDb;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (VOID)Level; return TRUE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A, CONST CHAR8 *B) { return strcmp(A,B); }
VOID *EFIAPI ZeroMem(VOID *Buffer, UINTN Size) { return memset(Buffer,0,Size); }
VOID *EFIAPI SetMem(VOID *Buffer, UINTN Size, UINT8 Value) { return memset(Buffer,Value,Size); }
UINT64 EFIAPI LShiftU64(UINT64 Value, UINTN Shift) { assert(Shift<64); return Value<<Shift; }
VOID EFIAPI EfiAcquireLock(EFI_LOCK *Lock) {
  assert(Lock->Lock==EfiLockReleased); Lock->Lock=EfiLockAcquired;
}
VOID EFIAPI EfiReleaseLock(EFI_LOCK *Lock) {
  assert(Lock->Lock==EfiLockAcquired); Lock->Lock=EfiLockReleased;
}

STATIC UINTN RailIndex(UINT32 Address) {
  assert(Address>=0x70000 && (Address&0xFFF)==0);
  UINTN Index=(Address-0x70000)/0x1000;
  assert(Index<ARRAY_SIZE(RailNames)); return Index;
}
STATIC EFI_STATUS EFIAPI GetAddress(EFI_CMD_DB_PROTOCOL *This, CONST CHAR8 *Name, UINT32 *Address) {
  assert(This==&CmdDb);
  for(UINTN I=0;I<ARRAY_SIZE(RailNames);++I) {
    if(strcmp(Name,RailNames[I])==0) { *Address=0x70000+(UINT32)I*0x1000; return EFI_SUCCESS; }
  }
  return EFI_NOT_FOUND;
}
CR_STATUS RpmhWrite(RpmhDeviceContext *Context, RpmhTcsCmd *Cmds, UINT32 Count) {
  (VOID)Context; (VOID)Cmds; (VOID)Count; assert(FALSE); return EFI_UNSUPPORTED;
}
STATIC CR_STATUS RailResult(VOID) {
  return ++RailCalls==FailRail ? CR_DEVICE_ERROR : CR_SUCCESS;
}
CR_STATUS RpmhSetVregVoltage(RpmhDeviceContext *Context, UINT32 Address, UINT32 Millivolts) {
  assert(Context==&Backend); RailVoltage[RailIndex(Address)]=Millivolts;
  return RailResult(); /* failure may follow a hardware side effect */
}
CR_STATUS RpmhSetVregMode(RpmhDeviceContext *Context, UINT32 Address, UINT8 Mode) {
  assert(Context==&Backend); RailMode[RailIndex(Address)]=Mode; return RailResult();
}
CR_STATUS RpmhEnableVreg(RpmhDeviceContext *Context, UINT32 Address, BOOLEAN Enable) {
  assert(Context==&Backend);
  if(!Enable) assert(Pins[80].OutputValue==GPIO_VALUE_LOW);
  RailEnabled[RailIndex(Address)]=Enable; return RailResult();
}
STATIC VOID Delay(VOID *Cookie, UINT32 Us) { (VOID)Cookie; NowUs+=Us; }
STATIC EFI_STATUS EFIAPI InitPin(EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config) {
  (VOID)This; assert(Config->PinNumber==80 || Config->PinNumber==204); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetPin(EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config) {
  GpioConfigParams Old=Pins[Config->PinNumber];
  (VOID)This;
  assert(Config->PinNumber==80 || Config->PinNumber==204);
  assert(Config->OutputEnable && Config->FunctionSel==0 &&
         Config->Pull==GPIO_PULL_DOWN && Config->DriveStrength==GPIO_DRIVE_STRENGTH_16MA);
  Pins[Config->PinNumber]=*Config;
  if(Old.OutputValue==GPIO_VALUE_LOW && Config->OutputValue==GPIO_VALUE_HIGH) {
    for(UINTN I=0;I<5;++I) assert(RailEnabled[I] && RailVoltage[I]==WcnMillivolts[I] && RailMode[I]==6);
    if(Config->PinNumber==80) {
      assert(Pins[204].OutputValue==GPIO_VALUE_HIGH && NowUs-XoRiseUs>=100);
      WlanRiseUs=NowUs; ++WlanRises;
    } else {
      assert(NowUs>=100000); XoRiseUs=NowUs; ++XoRises;
    }
  }
  if(Config->PinNumber==204 && Old.OutputValue==GPIO_VALUE_HIGH &&
     Config->OutputValue==GPIO_VALUE_LOW && Pins[80].OutputValue==GPIO_VALUE_HIGH)
    assert(NowUs-WlanRiseUs>=2000);
  return ++GpioWrites==FailGpio ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetPin(EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config) {
  (VOID)This; assert(Config->PinNumber==80 || Config->PinNumber==204);
  *Config=Pins[Config->PinNumber];
  if(LostGpioReadback && GpioWrites!=0) Config->OutputValue^=1;
  return ++GpioReads==FailRead ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ReadPin(EFI_GPIO_CR_PROTOCOL *This, UINT16 Pin, UINT8 *Value) {
  (VOID)This; assert(Pin==81 || Pin==82); *Value=(UINT8)Pins[Pin].OutputValue;
  return EFI_SUCCESS;
}
STATIC EFI_GPIO_CR_PROTOCOL Gpio = {
  .Revision=EFI_GPIO_CR_PROTOCOL_REVISION,
  .InitGpioConfigParams=InitPin, .ConfigGpio=SetPin,
  .GetGpioConfig=GetPin, .ReadIoValue=ReadPin
};
STATIC PcieIoOps Io = {
  .DelayUs=Delay, .SetSupply=CranePcieSetSupply, .SetGpio=CranePcieSetGpio
};
STATIC CONST PcieTargetController *Controller;

STATIC VOID Fixture(BOOLEAN InheritedWlan, BOOLEAN Bluetooth) {
  ZeroMem(&mPcieResources,sizeof(mPcieResources));
  ZeroMem(&Backend,sizeof(Backend)); Backend.Initialized=TRUE; mRpmh=&Backend;
  ZeroMem(&mRailLock,sizeof(mRailLock)); mRailLock.Lock=EfiLockReleased;
  ZeroMem(&CmdDb,sizeof(CmdDb)); CmdDb.GetEntryAddressByName=GetAddress; mCmdDb=&CmdDb;
  ZeroMem(RailVoltage,sizeof(RailVoltage)); ZeroMem(RailMode,sizeof(RailMode));
  ZeroMem(RailEnabled,sizeof(RailEnabled)); ZeroMem(Pins,sizeof(Pins));
  mPcieTarget=CrTargetGetPcieContext(); Controller=&mPcieTarget->Controllers[0];
  mPcieResources.Gpio=&Gpio; mPcieResources.Rpmh=&gRpmhCrProtocol;
  assert(InitializeRailDescriptions(mPcieTarget)==EFI_SUCCESS);
  /* USB's L1B is published by CrDAL separately from the PCI/UFS target. */
  mRails[mRailCount++]=(RPMH_CR_RAIL){.Name="ldob1", .Address=0x76000};
  FindRail("ldob1")->Parent=FindRail("smpb11");
  FindRail("ldob1")->ParentMv=968; FindRail("ldob1")->ParentMode=6;
  FindRail("ldob6")->Parent=FindRail("smpb12");
  FindRail("ldob6")->ParentMv=1352; FindRail("ldob6")->ParentMode=6;
  Pins[80]=(GpioConfigParams){.PinNumber=80,.OutputEnable=TRUE,.OutputValue=InheritedWlan};
  Pins[204].PinNumber=204;
  Pins[81]=(GpioConfigParams){.PinNumber=81,.OutputEnable=TRUE,.OutputValue=Bluetooth,
                            .FunctionSel=GPIO_FUNC_1,.DriveStrength=GPIO_DRIVE_STRENGTH_4MA};
  Pins[82]=(GpioConfigParams){.PinNumber=82,.OutputEnable=FALSE,.OutputValue=GPIO_VALUE_LOW,
                            .Pull=GPIO_PULL_KEEPER};
  NowUs=XoRiseUs=WlanRiseUs=RailCalls=FailRail=GpioWrites=FailGpio=GpioReads=FailRead=0;
  WlanRises=XoRises=0; LostGpioReadback=FALSE;
}
STATIC VOID Empty(VOID) {
  CRANE_PCIE_ENDPOINT_STATE *State=&mPcieResources.Endpoints[0];
  assert(!State->Ready && !State->WlanManaged && !State->XoManaged && State->SupplyMask==0);
  assert(Pins[80].OutputValue==GPIO_VALUE_LOW && Pins[204].OutputValue==GPIO_VALUE_LOW);
  for(UINTN I=0;I<mRailCount;++I)
    assert(mRails[I].ReferenceCount==0 && !mRails[I].EnableRequested &&
           !mRails[I].VoltageRequested && !mRails[I].ModeRequested && !mRails[I].ParentOwned);
  for(UINTN I=0;I<ARRAY_SIZE(RailNames);++I)
    assert(!RailEnabled[I] && RailVoltage[I]==0 && RailMode[I]==0);
  for(UINTN I=0;I<CRANE_PCIE_RESOURCE_REFS;++I) assert(mPcieResources.SupplyRefs[I].Count==0);
}

int main(VOID) {
  /* Independent schematic pin/rail identities, including non-PCIe parents. */
  Fixture(FALSE,FALSE);
  assert(Controller->EndpointPower->SupplyCount==5 && Controller->EndpointPower->EnableGpio.Pin==80 &&
         Controller->EndpointPower->XoGpio.Pin==204 && Controller->EndpointPower->BtEnablePin==81 &&
         Controller->EndpointPower->SwControlPin==82);
  for(UINTN I=0;I<5;++I) {
    CONST PcieTargetSupply *S=&mPcieTarget->Supplies[Controller->EndpointPower->SupplyOffset+I];
    assert(strcmp(S->Id,RailNames[I])==0 && S->VoltageMv==WcnMillivolts[I] && S->Mode==6);
  }
  for(UINTN Inherited=0;Inherited<2;++Inherited) for(UINTN Bluetooth=0;Bluetooth<2;++Bluetooth) {
    Fixture(Inherited,Bluetooth);
    GpioConfigParams Bt=Pins[81], Sw=Pins[82];
    assert(CranePcieSetEndpointPower(&Io,&mPcieTarget->Controllers[1],TRUE)==CR_SUCCESS);
    assert(RailCalls==0 && GpioWrites==0);
    assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_SUCCESS);
    assert(mPcieResources.Endpoints[0].Ready && Pins[80].OutputValue==GPIO_VALUE_HIGH);
    assert(Inherited ? WlanRises==0 && XoRises==0 : WlanRises==1 && XoRises==1 && NowUs>=152100);
    UINTN Writes=GpioWrites, Votes=RailCalls;
    assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_SUCCESS && GpioWrites==Writes && RailCalls==Votes);
    assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_SUCCESS); Empty();
    assert(memcmp(&Bt,&Pins[81],sizeof(Bt))==0 && memcmp(&Sw,&Pins[82],sizeof(Sw))==0);
  }
  /* Endpoint off must not remove PCIe1 S10B, PCIe0 PHY S12B, or USB S11B. */
  Fixture(FALSE,TRUE);
  CONST PcieTargetSupply M2={"m2","rpmh","smpb10",1800,0,6};
  CONST PcieTargetSupply Pll={"pll","rpmh","ldob6",1200,0,7};
  BOOLEAN Owned=FALSE;
  assert(CranePcieSetSupply(NULL,&M2,TRUE)==CR_SUCCESS);
  assert(CranePcieSetSupply(NULL,&Pll,TRUE)==CR_SUCCESS);
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"ldob1",880,7,&Owned)==EFI_SUCCESS && Owned);
  assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_SUCCESS);
  assert(FindRail("smpb11")->ReferenceCount==2 && FindRail("smpb12")->ReferenceCount==2);
  assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_SUCCESS);
  for(UINTN I=0;I<3;++I) assert(RailEnabled[I] && RailVoltage[I]==WcnMillivolts[I]);
  assert(FindRail("smpb11")->ReferenceCount==1 && FindRail("smpb12")->ReferenceCount==1);
  assert(CranePcieSetSupply(NULL,&Pll,FALSE)==CR_SUCCESS);
  assert(CranePcieSetSupply(NULL,&M2,FALSE)==CR_SUCCESS);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"ldob1",FALSE)==EFI_SUCCESS); Empty();

  /* Every regulator wire step can fail after a side effect. */
  for(UINTN Inherited=0;Inherited<2;++Inherited) for(UINTN I=1;I<=15;++I) {
    Fixture(Inherited,FALSE); FailRail=I;
    assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_DEVICE_ERROR);
    FailRail=0;
    assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_SUCCESS); Empty();
  }
  for(UINTN I=1;I<=5;++I) {
    Fixture(FALSE,FALSE); FailGpio=I;
    assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_DEVICE_ERROR);
    FailGpio=0;
    assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_SUCCESS); Empty();
  }
  Fixture(FALSE,FALSE); FailRead=1;
  assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_DEVICE_ERROR);
  assert(RailCalls==0 && GpioWrites==0); Empty();
  Fixture(FALSE,FALSE); LostGpioReadback=TRUE;
  assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_DEVICE_ERROR);
  LostGpioReadback=FALSE;
  assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_SUCCESS); Empty();
  for(UINTN I=1;I<=15;++I) {
    Fixture(FALSE,FALSE); assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_SUCCESS);
    FailRail=RailCalls+I;
    assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_DEVICE_ERROR);
    assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_BUSY);
    FailRail=0;
    assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_SUCCESS); Empty();
  }
  for(UINTN I=1;I<=2;++I) {
    Fixture(FALSE,FALSE); assert(CranePcieSetEndpointPower(&Io,Controller,TRUE)==CR_SUCCESS);
    FailGpio=GpioWrites+I;
    assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_DEVICE_ERROR);
    for(UINTN R=0;R<5;++R) assert(RailEnabled[R]);
    FailGpio=0;
    assert(CranePcieSetEndpointPower(&Io,Controller,FALSE)==CR_SUCCESS); Empty();
  }
  puts("WCN6855 endpoint: GPIO/XO timing, inherited WLAN, untouched BT/SWCTRL, RPMh parent sharing and every partial-vote/release retry passed");
  return 0;
}
