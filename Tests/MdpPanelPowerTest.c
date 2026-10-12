/* Actual shared MDSS native panel supplier against protocol-boundary models.
 * No SPMI bias control or physical AB/IBB OFF proof is simulated.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Driver/MDSSCrDxe/MdpPanelPower.h"
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
typedef struct {
  UINT32 Calls,Fail,PinCalls,RetainCalls,Delays,Elapsed;
  BOOLEAN Posted,Dropped,InputLag;EFI_STATUS Failure;
  UINT32 InputReads;
  VOID *Owners[2];GpioConfigParams Pins[2];UINT8 Values[2];
  BOOLEAN Floors[3];UINT32 FloorWrites[3];
} MODEL;
static MODEL H;static EFI_RPMH_CR_PROTOCOL R;static EFI_GPIO_CR_PROTOCOL G;
static MDP_PANEL_POWER P;
static EFI_STATUS Step(void){return ++H.Calls==H.Fail?H.Failure:EFI_SUCCESS;}
static UINT32 Rail(CONST CHAR8 *Name){
  if(!strcmp(Name,"bobc1"))return 0;if(!strcmp(Name,"ldoa14"))return 1;
  assert(!strcmp(Name,"ldoc11"));return 2;
}
static EFI_STATUS EFIAPI Retain(EFI_RPMH_CR_PROTOCOL *This,CONST CHAR8 *Name){
  assert(This==&R);EFI_STATUS S=Step();++H.RetainCalls;
  if(S==EFI_SUCCESS||H.Posted){UINT32 I=Rail(Name);
    if(!H.Floors[I]){H.Floors[I]=TRUE;++H.FloorWrites[I];}}
  return S;
}
static EFI_STATUS EFIAPI RetainFixed(EFI_RPMH_CR_PROTOCOL *This,CONST CHAR8 *Name,UINT32 Mv,UINT8 Mode){
  assert(!strcmp(Name,"bobc1")&&Mv==3296&&Mode==6);return Retain(This,Name);
}
static EFI_STATUS EFIAPI ForbiddenAcquire(EFI_RPMH_CR_PROTOCOL *T,CONST CHAR8 *N,UINT32 M,UINT8 Mode,BOOLEAN *O){abort();}
static EFI_STATUS EFIAPI ForbiddenEnable(EFI_RPMH_CR_PROTOCOL *T,CONST CHAR8 *N,BOOLEAN On){abort();}
static EFI_STATUS EFIAPI ForbiddenVoltage(EFI_RPMH_CR_PROTOCOL *T,CONST CHAR8 *N,UINT32 M){abort();}
static EFI_STATUS EFIAPI ForbiddenMode(EFI_RPMH_CR_PROTOCOL *T,CONST CHAR8 *N,UINT8 M){abort();}
static UINT32 Pin(UINT16 N){assert(N==75||N==66);return N==75?0:1;}
static EFI_STATUS EFIAPI AcquirePin(EFI_GPIO_CR_PROTOCOL *T,VOID *Owner,CONST GpioConfigParams *C,BOOLEAN *Owned){
  assert(T==&G&&Owner==&P);UINT32 I=Pin(C->PinNumber);EFI_STATUS S=Step();++H.PinCalls;
  *Owned=FALSE;if(S==EFI_SUCCESS||H.Posted){
    assert(!H.Owners[I]||H.Owners[I]==Owner);H.Owners[I]=Owner;H.Pins[I]=*C;*Owned=TRUE;
    H.Values[I]=(UINT8)C->OutputValue;
  }return S;
}
static EFI_STATUS EFIAPI SetPin(EFI_GPIO_CR_PROTOCOL *T,VOID *Owner,UINT16 N,UINT8 V){
  assert(T==&G&&Owner==&P&&H.Owners[Pin(N)]==Owner&&N==75);EFI_STATUS S=Step();
  if((S==EFI_SUCCESS||H.Posted)&&!H.Dropped)H.Values[Pin(N)]=V;
  /* Real SetPinValue calls GpioApplyConfigChecked: dropped latch writes
     cannot return SUCCESS. ReadPinValue separately samples the input. */
  return S==EFI_SUCCESS&&H.Values[Pin(N)]!=V?EFI_DEVICE_ERROR:S;
}
static EFI_STATUS EFIAPI ReadPin(EFI_GPIO_CR_PROTOCOL *T,VOID *Owner,UINT16 N,UINT8 *V){
  assert(T==&G&&Owner==&P&&H.Owners[Pin(N)]==Owner);EFI_STATUS S=Step();
  ++H.InputReads;if(S==EFI_SUCCESS)*V=H.Values[Pin(N)]^(H.InputLag?1U:0U);return S;
}
static EFI_STATUS EFIAPI ForbiddenRelease(EFI_GPIO_CR_PROTOCOL *T,VOID *Owner,UINT16 N){abort();}
static EFI_STATUS NativeReady(VOID *C){assert(C==&H);return Step();}
static EFI_STATUS Delay(VOID *C,UINT32 Us){assert(C==&H&&Us==20000);EFI_STATUS S=Step();
  if(S==EFI_SUCCESS||H.Posted){++H.Delays;H.Elapsed+=Us;}return S;}
static MDP_PANEL_POWER_CONFIG Config(void){
  MDP_PANEL_POWER_CONFIG C={0};C.Revision=MDP_PANEL_POWER_CONFIG_REVISION;C.Size=sizeof(C);
  C.RailCount=3;strcpy(C.Rails[0].Name,"bobc1");strcpy(C.Rails[1].Name,"ldoa14");strcpy(C.Rails[2].Name,"ldoc11");
  for(UINT32 I=0;I<3;++I)C.Rails[I].Policy=MdpPanelRetainInheritedEnable;
  C.Rails[0].Policy=MdpPanelRetainFixedVote;C.Rails[0].NominalMillivolts=3296;C.Rails[0].NominalMode=6;
  C.Rails[1].NominalMillivolts=1800;C.Rails[1].NominalMode=7;C.Rails[1].PostOnDelayUs=20000;
  C.Rails[2].NominalMillivolts=3300;C.Rails[2].NominalMode=7;
  C.Reset=(GpioConfigParams){75,TRUE,GPIO_FUNC_NORMAL,GPIO_PULL_NONE,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_LOW};
  C.HasTe=TRUE;C.Te=(GpioConfigParams){66,FALSE,GPIO_FUNC_1,GPIO_PULL_DOWN,GPIO_DRIVE_STRENGTH_2MA,GPIO_VALUE_LOW};
  C.BiasPolicy=MdpPanelSwirePermanentRetention;C.BiasSid=5;C.AbBase=0xde00;C.IbbBase=0xdc00;
  C.PostBiasDelayUs=20000;return C;
}
static void Init(void){
  memset(&H,0,sizeof(H));memset(&P,0,sizeof(P));H.Failure=EFI_DEVICE_ERROR;
  R=(EFI_RPMH_CR_PROTOCOL){.Revision=EFI_RPMH_CR_PROTOCOL_REVISION,.AcquireVreg=ForbiddenAcquire,
    .RpmhEnableVreg=ForbiddenEnable,.RpmhSetVregVoltage=ForbiddenVoltage,.RpmhSetVregMode=ForbiddenMode,
    .EnsureRetainedVregEnabled=Retain,.EnsureRetainedVreg=RetainFixed};
  G=(EFI_GPIO_CR_PROTOCOL){.Revision=EFI_GPIO_CR_PROTOCOL_REVISION,.AcquirePin=AcquirePin,
    .SetPinValue=SetPin,.ReadPinValue=ReadPin,.ReleasePin=ForbiddenRelease};
  MDP_PANEL_POWER_CONFIG C=Config();
  assert(MdpPanelPowerInit(&P,&R,&G,&C,&H,NativeReady,Delay)==EFI_SUCCESS&&!H.Calls);
  memset(&C,0xff,sizeof(C)); /* Captured target data must not borrow this array. */
}
static void Retained(void){
  BOOLEAN Owned=FALSE;UINT32 Calls=H.Calls;
  assert(MdpPanelPowerSet(&P,FALSE,&Owned)==EFI_UNSUPPORTED&&Owned);
  assert(MdpPanelPowerClose(&P)==EFI_UNSUPPORTED&&P.Initialized&&H.Calls==Calls);
  assert(H.Floors[0]&&H.Floors[1]&&H.Floors[2]&&H.Owners[0]&&H.Owners[1]);
  for(UINT32 I=0;I<3;++I)assert(H.FloorWrites[I]==1);
}
int main(void){
  Init();BOOLEAN Owned=FALSE;
  assert(MdpPanelPowerSet(&P,TRUE,&Owned)==EFI_SUCCESS&&Owned&&P.PowerReady);
  assert(H.Elapsed==40000&&H.Delays==2&&H.PinCalls==2);
  UINT32 OnCalls=H.Calls;
  assert(H.Pins[0].PinNumber==75&&H.Pins[0].OutputEnable&&H.Pins[0].DriveStrength==GPIO_DRIVE_STRENGTH_8MA);
  assert(H.Pins[1].PinNumber==66&&!H.Pins[1].OutputEnable&&H.Pins[1].FunctionSel==GPIO_FUNC_1);
  assert(MdpPanelPowerReset(&P,TRUE)==EFI_SUCCESS&&H.Values[0]==1);
  H.Dropped=TRUE;assert(MdpPanelPowerReset(&P,FALSE)==EFI_DEVICE_ERROR&&H.Values[0]==1);
  H.Dropped=FALSE;assert(MdpPanelPowerReset(&P,FALSE)==EFI_SUCCESS&&!H.Values[0]);
  H.InputLag=TRUE;UINT32 Reads=H.InputReads;
  assert(MdpPanelPowerReset(&P,TRUE)==EFI_SUCCESS&&H.Values[0]==1);
  assert(MdpPanelPowerReset(&P,FALSE)==EFI_SUCCESS&&H.Values[0]==0);
  assert(H.InputReads==Reads); /* Input propagation is not a latch ACK. */
  H.InputLag=FALSE;
  Retained();assert(MdpPanelPowerSet(&P,TRUE,&Owned)==EFI_NOT_READY&&Owned);
  /* Every actual adapter provider/delay position, before/after effects and
   * warning returns. Retry must preserve the same GPIO owner and each floor. */
  for(UINT32 Kind=0;Kind<2;++Kind)for(UINT32 Posted=0;Posted<2;++Posted)for(UINT32 I=1;I<=OnCalls;++I){
    Init();H.Fail=I;H.Posted=Posted;H.Failure=Kind?1:EFI_DEVICE_ERROR;
    assert(MdpPanelPowerSet(&P,TRUE,&Owned)!=EFI_SUCCESS);
    H.Fail=0;assert(MdpPanelPowerSet(&P,TRUE,&Owned)==EFI_SUCCESS&&Owned);Retained();
  }
  /* Reset keeps the same permanent GPIO owner at every provider boundary,
     including posted failures and warning acknowledgements, in both directions. */
  UINT32 ResetFaults=0;
  for(UINT32 High=0;High<2;++High){
    Init();assert(MdpPanelPowerBindPins(&P,&Owned)==EFI_SUCCESS&&Owned);
    H.Calls=0;H.InputLag=TRUE;
    assert(MdpPanelPowerReset(&P,(BOOLEAN)High)==EFI_SUCCESS);
    UINT32 ResetCalls=H.Calls;
    for(UINT32 Kind=0;Kind<2;++Kind)for(UINT32 Posted=0;Posted<2;++Posted)for(UINT32 I=1;I<=ResetCalls;++I){
      Init();assert(MdpPanelPowerBindPins(&P,&Owned)==EFI_SUCCESS&&Owned);
      H.Calls=0;H.Fail=I;H.Posted=Posted;H.Failure=Kind?1:EFI_DEVICE_ERROR;H.InputLag=TRUE;
      assert(MdpPanelPowerReset(&P,(BOOLEAN)High)!=EFI_SUCCESS);
      assert(P.PermanentHold&&P.PinOwned[0]&&H.Owners[0]==&P);
      H.Fail=0;assert(MdpPanelPowerReset(&P,(BOOLEAN)High)==EFI_SUCCESS);
      ++ResetFaults;
    }
  }
  Init();assert(MdpPanelPowerBindPins(&P,&Owned)==EFI_SUCCESS&&Owned);
  assert(MdpPanelPowerSet(&P,FALSE,&Owned)==EFI_UNSUPPORTED&&Owned);
  assert(MdpPanelPowerReset(&P,TRUE)==EFI_NOT_READY);
  assert(MdpPanelPowerReset(&P,FALSE)==EFI_SUCCESS); /* Cleanup can still hold reset. */
  Init();MDP_PANEL_POWER Saved=P;UINT32 Calls=H.Calls;
  assert(MdpPanelPowerSet(&P,TRUE,&P.PermanentHold)==EFI_INVALID_PARAMETER&&!memcmp(&P,&Saved,sizeof(P))&&H.Calls==Calls);
  MDP_PANEL_POWER Copy=P;assert(MdpPanelPowerSet(&Copy,TRUE,&Owned)==EFI_INVALID_PARAMETER&&H.Calls==Calls);
  R.Revision=0x10004;assert(MdpPanelPowerSet(&P,TRUE,&Owned)==EFI_NOT_READY&&!Owned&&H.Calls==Calls);
  R.Revision=EFI_RPMH_CR_PROTOCOL_REVISION;
  G.Revision=0x10001;assert(MdpPanelPowerSet(&P,TRUE,&Owned)==EFI_NOT_READY&&!Owned&&H.Calls==Calls);
  G.Revision=EFI_GPIO_CR_PROTOCOL_REVISION;
  assert(MdpPanelPowerClose(&P)==EFI_SUCCESS&&!P.Initialized&&!H.Calls);
  MDP_PANEL_POWER_CONFIG C=Config();C.Rails[2].Policy=99;
  assert(MdpPanelPowerInit(&P,&R,&G,&C,&H,NativeReady,Delay)==EFI_UNSUPPORTED&&!P.Initialized);
  C=Config();C.Rails[0].NominalMillivolts=0;
  assert(MdpPanelPowerInit(&P,&R,&G,&C,&H,NativeReady,Delay)==EFI_INVALID_PARAMETER&&!P.Initialized);
  C=Config();C.Rails[0].NominalMode=1;
  assert(MdpPanelPowerInit(&P,&R,&G,&C,&H,NativeReady,Delay)==EFI_INVALID_PARAMETER&&!P.Initialized);
  C=Config();R.EnsureRetainedVreg=NULL;
  assert(MdpPanelPowerInit(&P,&R,&G,&C,&H,NativeReady,Delay)==EFI_UNSUPPORTED&&!P.Initialized);
  R.EnsureRetainedVreg=RetainFixed;
  C=Config();C.Te.PinNumber=C.Reset.PinNumber;
  assert(MdpPanelPowerInit(&P,&R,&G,&C,&H,NativeReady,Delay)==EFI_INVALID_PARAMETER&&!P.Initialized);
  Init();assert(MdpPanelPowerBindPins(&P,&Owned)==EFI_SUCCESS&&Owned);
  assert(MdpPanelPowerReset(&P,FALSE)==EFI_SUCCESS&&!H.Values[0]);
  Calls=H.Calls;assert(MdpPanelPowerSet(&P,FALSE,&Owned)==EFI_UNSUPPORTED&&Owned&&H.Calls==Calls);
  printf("PASS: actual shared panel supplier, %u on positions x4, retained floors/pins, checked reset output latch and permanent OFF refusal; %u reset fault cases\n",OnCalls,ResetFaults);
  return 0;
}
