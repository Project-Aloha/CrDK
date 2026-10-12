/* Actual selected native target + portable ClockLib display graphs. MIT */
#define main ClockPriorOwnershipRegression
#include "ClockOwnershipTest.c"
#undef main
#include <Library/ClockExternalLib.h>
#ifdef CR_DISPLAY_TARGET_PATH
#include CR_DISPLAY_TARGET_PATH
#elif TEST_SOC==0x8250
#include "../Target/Sm8250/CrClockTarget.c"
#else
#include "../Target/Sm8350/CrStorageClockTarget.c"
#endif
static UINTN Maps,Unmaps;
CR_STATUS MapDeviceIORegion(UINTN Address,UINTN Size) {
  assert((Address==0x100000&&Size==0x1f0000)||(Address==0xaf00000&&Size==0x20000));
  ++Maps;return CR_SUCCESS;
}
CR_STATUS UnMapMemRegion(UINTN Address,UINTN Size) {assert(Address&&Size);++Unmaps;return CR_SUCCESS;}
static ClockNode *DisplayNode(ClockDriverContext *C,CONST CHAR8 *Name) {
  ClockNode *N=NULL;assert(GetClockNode(C,Name,NULL,NULL,&N)==CR_SUCCESS);return N;
}
static VOID PllState(UINTN B) {
  *Reg(B+0x1000)=BIT(31)|BIT(2)|BIT(0);
  *Reg(B+0x1004)=31;*Reg(B+0x1040)=0x4000;
  *Reg(B+0x100c)=7;*Reg(B+0x1038)=1;
}
#ifndef CR_CLOCK_TARGET_DISPLAY_MAIN
#define CR_CLOCK_TARGET_DISPLAY_MAIN main
#endif
int CR_CLOCK_TARGET_DISPLAY_MAIN(void) {
  Reset();ClockDriverContext *C=CrTargetGetClockContext();
  assert(C&&C->ClockControllerCount==CC_TYPE_DISP_CC+1);
#ifdef _KERNEL_MODE
  C->ClockControllers[CC_TYPE_GCC].MemMapped=TRUE;
  C->ClockControllers[CC_TYPE_DISP_CC].MemMapped=TRUE;
  assert(ClockLibInit(&C)==CR_SUCCESS&&Maps==0);Active=C;
#else
  assert(ClockLibInit(&C)==CR_SUCCESS&&Maps==2);Active=C;
#endif
  UINTN W=Writes,R=Reads;assert(CrTargetClockInit(C)==CR_SUCCESS&&Writes==W);
  assert(Reads-R==(TEST_SOC==0x8250?3U:1U));
  UINTN B=C->ClockControllers[CC_TYPE_DISP_CC].Address;
  CR_CLOCK_EXTERNAL E={0};W=Writes;R=Reads;
  assert(CrClockExternalInitForSoc(&E,C,TEST_SOC)==CR_SUCCESS&&E.SecondPresent&&Reads==R&&Writes==W);
  ClockNode *Mdp=DisplayNode(C,"disp_cc_mdss_mdp_clk"),*Lut=DisplayNode(C,"disp_cc_mdss_mdp_lut_clk");
  ClockNode *Pll=DisplayNode(C,"disp_cc_pll1");
  CR_STATUS Missing=ClockEnable(C,Mdp,200000000,TRUE);
  if(Missing!=CR_NOT_READY || Writes!=W || Pll->ReferenceCount)
    fprintf(stderr,"unavailable inherited PLL status=%llx writes=%llu initial=%llu refs=%u\n",
      (unsigned long long)Missing,(unsigned long long)Writes,(unsigned long long)W,Pll->ReferenceCount);
  assert(Missing==CR_NOT_READY&&Writes==W&&Pll->ReferenceCount==0);
  PllState(B);assert(ClockEnable(C,Mdp,200000000,TRUE)==CR_SUCCESS);
  assert(Pll->ReferenceCount==1&&(*Reg(B+(TEST_SOC==0x8250?0x20cc:0x20c8))&0x71f)==0x405);
  assert(ClockEnable(C,Lut,200000000,TRUE)==CR_SUCCESS&&Pll->ReferenceCount==2);
  W=Writes;*Reg(B+0x1040)=0;
  assert(ClockEnable(C,Lut,200000000,TRUE)==CR_NOT_READY&&Writes==W&&Lut->ReferenceCount==1);
  assert(ClockEnable(C,Lut,200000000,FALSE)==CR_SUCCESS);
  assert(ClockEnable(C,Mdp,200000000,FALSE)==CR_SUCCESS&&Pll->ReferenceCount==0);
  PllState(B);W=Writes;
  *Reg(B+(TEST_SOC==0x8250?0x1000:0x100c))|=TEST_SOC==0x8250?BIT(20):BIT(21);
  assert(ClockEnable(C,Mdp,200000000,TRUE)==CR_NOT_READY&&Writes==W);
  PllState(B);
  for(UINT32 I=0;I<5;++I) {
    UINT32 Offsets[]={0x1000,0x1004,0x1040,0x100c,0x1038};UINT32 Old=*Reg(B+Offsets[I]);
    *Reg(B+Offsets[I])=0;W=Writes;
    assert(ClockEnable(C,Mdp,200000000,TRUE)==CR_NOT_READY&&Writes==W&&Pll->ReferenceCount==0);
    *Reg(B+Offsets[I])=Old;
  }
  W=Writes;assert(ClockEnable(C,Mdp,300000000,TRUE)==CR_UNSUPPORTED&&Pll->ReferenceCount==0&&Writes==W);
  int Owner;assert(CrClockExternalReserve(&E,&Owner)==CR_SUCCESS);
  for(UINT32 I=0;I<3;++I)E.Access[I]->ReferenceCount=1;E.Domain->ReferenceCount=1;
  /* Actual selected graph: reset before every new private source write,
     retire inherited functional requests while preserving PLL registers. */
  UINT32 PllRegisters[5],PllOffsets[]={0x1000,0x1004,0x1040,0x100c,0x1038};
  for(UINT32 I=0;I<5;++I)PllRegisters[I]=*Reg(B+PllOffsets[I]);
  assert(CrClockExternalResetCore(&E,&Owner,7,TRUE)==CR_SUCCESS);
  assert(*Reg(B+0x2000)&BIT(0));
  assert(CrClockExternalResetCore(&E,&Owner,7,FALSE)==CR_SUCCESS);
  assert(CrClockExternalRetireInheritedFunctional(&E,&Owner,7)==CR_SUCCESS);
  for(UINT32 I=0;I<5;++I)assert(PllRegisters[I]==*Reg(B+PllOffsets[I]));
  assert(ClockEnable(C,Mdp,200000000,TRUE)==CR_SUCCESS);
  assert(ClockEnable(C,Lut,200000000,TRUE)==CR_SUCCESS);
  W=Writes;assert(CrClockExternalResetCore(&E,&Owner,7,TRUE)==CR_SUCCESS&&Writes==W);
  assert(CrClockExternalRetireInheritedFunctional(&E,&Owner,7)==CR_SUCCESS&&Writes==W);
  assert(ClockEnable(C,Lut,200000000,FALSE)==CR_SUCCESS);
  assert(ClockEnable(C,Mdp,200000000,FALSE)==CR_SUCCESS);
  CR_CLOCK_DSI_PLAN Plan={.Revision=1,.Controller=0,.PixelHz=148500000,.ByteHz=111375000,
    .PixelSourceHz=148500000,.PixelM=1,.PixelN=1,.ByteInterfaceDivider=2};
  if(E.Leaf[1]->DividerReadOnly)*Reg(B+E.Leaf[1]->DividerRegister)=1;
  for(UINT32 I=0;I<2;++I) {
    Plan.Controller=I;ClockNode *D=I?E.SecondLeaf[1]:E.Leaf[1];
    if(D->DividerReadOnly)*Reg(B+D->DividerRegister)=1;
    assert(CrClockExternalSetPlan(&E,&Owner,I,&Plan,TRUE)==CR_SUCCESS);
    assert(CrClockExternalSetPlan(&E,&Owner,I,NULL,FALSE)==CR_SUCCESS);
  }
  assert(CrClockExternalReleaseOwner(&E,&Owner)==CR_SUCCESS);
  for(UINT32 I=0;I<3;++I)E.Access[I]->ReferenceCount=0;E.Domain->ReferenceCount=0;
  assert(CrClockExternalUninit(&E)==CR_SUCCESS);
#ifdef _KERNEL_MODE
  assert(ClockDeinit(C)==CR_SUCCESS&&Unmaps==0);
#else
  assert(ClockDeinit(C)==CR_SUCCESS&&Unmaps==2);
#endif
  puts("Selected native display clock target: real graph, sparse map, pure admission, inherited PLL refusals/drift, MDP/LUT and both DSI plans PASS");
  return 0;
}
