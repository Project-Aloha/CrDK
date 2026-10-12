/* Actual native panel supply + RPMh DXE + selected SM8250 supply graph.
 * CmdDB/TCS and GPIO pins are models; no physical bias observation. MIT */
#define CR_MDSS_PANEL_RETAINED_TEST
#define main RpmhPreviousMain
#include "RpmhCrDxeTest.c"
#undef main
#include "../Driver/MDSSCrDxe/MdpPanelPower.h"
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
static MDP_PANEL_POWER Panel;
static EFI_GPIO_CR_PROTOCOL Gpio;
static UINT32 PinCount,Delays;
static UINT8 ResetLevel;
static EFI_STATUS Ready(VOID *Context){assert(Context==&Panel);return EFI_SUCCESS;}
static EFI_STATUS Delay(VOID *Context,UINT32 Us){assert(Context==&Panel&&Us==20000);++Delays;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI PinAcquire(EFI_GPIO_CR_PROTOCOL *This,VOID *Owner,CONST GpioConfigParams *C,BOOLEAN *Owned){
  assert(This==&Gpio&&Owner==&Panel&&(C->PinNumber==75||C->PinNumber==66));
  *Owned=TRUE;++PinCount;if(C->PinNumber==75)ResetLevel=C->OutputValue;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PinSet(EFI_GPIO_CR_PROTOCOL *This,VOID *Owner,UINT16 Pin,UINT8 Value){
  assert(This==&Gpio&&Owner==&Panel&&Pin==75);ResetLevel=Value;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PinRead(EFI_GPIO_CR_PROTOCOL *This,VOID *Owner,UINT16 Pin,UINT8 *Value){
  assert(This==&Gpio&&Owner==&Panel&&Pin==75);*Value=ResetLevel;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PanelAddress(EFI_CMD_DB_PROTOCOL *This,CONST CHAR8 *Name,UINT32 *Address){
  assert(This==&mCmdDbProtocol);CONST CR_DAL_RPMH_RAILS *Rails=CrTestGetPanelRails();
  for(UINT32 I=0;I<Rails->Count;++I)if(!strcmp(Name,Rails->Names[I])){
    *Address=0xa0000+I*0x100;return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
static void Start(void){
  ResetMocks();mPanelBoard=TRUE;mNoPcie=TRUE;
  assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234,NULL)==EFI_SUCCESS);
  mCmdDbProtocol.GetEntryAddressByName=PanelAddress;
  memset(&Panel,0,sizeof(Panel));PinCount=Delays=0;
  Gpio=(EFI_GPIO_CR_PROTOCOL){.Revision=EFI_GPIO_CR_PROTOCOL_REVISION,.AcquirePin=PinAcquire,.SetPinValue=PinSet,.ReadPinValue=PinRead};
  MDP_PANEL_POWER_CONFIG C;
  assert(MdpPanelPowerR66451Config(&C)==EFI_SUCCESS);
  assert(MdpPanelPowerInit(&Panel,&gRpmhCrProtocol,&Gpio,&C,&Panel,Ready,Delay)==EFI_SUCCESS);
}
int main(void){
  /* Storage owns BOB3296 before display, via the actual selected graph. */
  Start();BOOLEAN Storage=FALSE,Owned=FALSE;
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"ldoc9",2952,7,&Storage)==EFI_SUCCESS&&Storage);
  RPMH_CR_RAIL *Bob=FindRail("bobc1");assert(Bob&&Bob->ReferenceCount==1&&Bob->VoltageMv==3296&&Bob->Mode==6);
  UINTN Before=mWriteCalls;
  assert(MdpPanelPowerSet(&Panel,TRUE,&Owned)==EFI_SUCCESS&&Owned);
  assert(Bob->ReferenceCount==2&&RetainedFloor(Bob)==1&&mWriteCalls==Before+2);
  assert(mWrites[Before].Command.addr==FindRail("ldoa14")->Address+4&&mWrites[Before].Command.data==1);
  assert(mWrites[Before+1].Command.addr==FindRail("ldoc11")->Address+4&&mWrites[Before+1].Command.data==1);
  assert(PinCount==2&&Delays==2&&Panel.PowerReady);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"ldoc9",FALSE)==EFI_SUCCESS);
  assert(Bob->ReferenceCount==1&&RetainedFloor(Bob)==1);
  Before=mWriteCalls;
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"bobc1",FALSE)==EFI_ACCESS_DENIED);
  assert(ProtocolRpmhSetVregVoltage(&gRpmhCrProtocol,"bobc1",3300)==EFI_ACCESS_DENIED);
  RpmhTcsCmd Raw={.addr=Bob->Address,.data=0};
  assert(ProtocolRpmhWrite(&gRpmhCrProtocol,&Raw,1)==EFI_ACCESS_DENIED);
  assert(MdpPanelPowerReset(&Panel,FALSE)==EFI_SUCCESS);
  assert(MdpPanelPowerSet(&Panel,FALSE,&Owned)==EFI_UNSUPPORTED&&Owned);
  assert(MdpPanelPowerClose(&Panel)==EFI_UNSUPPORTED&&Panel.Initialized&&mWriteCalls==Before);
  /* Actual DXE order: display first, then UFS, Wi-Fi, touch and SD. Their
   * existing selected parent contracts must coexist with the panel floor. */
  Start();assert(MdpPanelPowerSet(&Panel,TRUE,&Owned)==EFI_SUCCESS&&Owned);
  CONST CHAR8 *Names[]={"ldoa17","ldoc5","ldoa13","ldoc9"};
  CONST UINT32 Millivolts[]={2504,1800,1800,2952};
  for(UINT32 I=0;I<ARRAY_SIZE(Names);++I){Before=mWriteCalls;Storage=FALSE;
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,Names[I],Millivolts[I],7,&Storage)==EFI_SUCCESS&&Storage);
    assert(mWriteCalls==Before+3&&FindRail(Names[I])->ParentOwned);
  }
  Bob=FindRail("bobc1");assert(Bob->ReferenceCount==5&&RetainedFloor(Bob)==1);
  for(UINT32 I=0;I<ARRAY_SIZE(Names);++I)
    assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,Names[I],FALSE)==EFI_SUCCESS);
  assert(Bob->ReferenceCount==1&&Bob->Ready&&Bob->AppliedEnable==1);
  Before=mWriteCalls;
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"bobc1",3304,6,&Storage)==CR_BUSY&&!Storage);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"bobc1",FALSE)==EFI_ACCESS_DENIED&&mWriteCalls==Before);
  assert(MdpPanelPowerSet(&Panel,TRUE,&Owned)==EFI_SUCCESS&&mWriteCalls==Before);
  /* A partial permanent vote must retain one exact owner and retry positive
   * votes; no failed BOB/LDO request may withdraw another supplier. */
  for(UINT32 Warning=0;Warning<2;++Warning)for(UINTN Fail=1;Fail<=5;++Fail){
    Start();mFailWriteAt=Fail;mFailStatus=Warning?1:CR_DEVICE_ERROR;
    assert(MdpPanelPowerSet(&Panel,TRUE,&Owned)!=EFI_SUCCESS&&Owned);
    mFailWriteAt=0;
    assert(MdpPanelPowerSet(&Panel,TRUE,&Owned)==EFI_SUCCESS&&Owned);
    Bob=FindRail("bobc1");assert(Bob->Ready&&Bob->ReferenceCount==1&&RetainedFloor(Bob)==1);
    for(UINTN I=0;I<mWriteCalls;++I)assert(mWrites[I].Command.data!=0);
    Before=mWriteCalls;
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"ldoa17",2504,7,&Storage)==EFI_SUCCESS&&Storage);
    assert(mWriteCalls==Before+3&&Bob->ReferenceCount==2);
    assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"ldoa17",FALSE)==EFI_SUCCESS&&Bob->ReferenceCount==1);
  }
  puts("PASS: actual panel/RPMh/SM8250 graph in both dispatch orders; UFS/Wi-Fi/touch/SD coexist, conflicting votes and floor withdrawal refused");
  return 0;
}
