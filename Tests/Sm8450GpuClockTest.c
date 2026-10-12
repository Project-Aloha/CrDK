/* Actual clock core and SM8450 graph integration. SPDX-License-Identifier: MIT */
#define main ExistingClockOwnershipMain
#include "ClockOwnershipTest.c"
#undef main

static ClockDriverContext *GpuCore(VOID **Storage) {
  Reset();
  *Storage=calloc(1,CrSm8450ClockStateSize());assert(*Storage);
  ClockDriverContext *C=NULL;
  assert(CrSm8450ClockCreate(*Storage,CrSm8450ClockStateSize(),&C)==CR_SUCCESS);
  CR_CLOCK_MAPPING Maps[CC_TYPE_MAX];
  for(UINTN I=0;I<CC_TYPE_MAX;++I){UINT64 P;UINT32 S;
    assert(CrSm8450ClockResource((ClockControllerType)I,&P,&S)==CR_SUCCESS);
    Maps[I]=(CR_CLOCK_MAPPING){0x30000000U+I*0x1000000U,S};
  }
  assert(CrSm8450ClockBind(C,Maps,CC_TYPE_MAX)==CR_SUCCESS);
  CrLockInit(&C->Lock);C->LockInitialized=TRUE;C->PrepareSource=Sm8450ClockPrepareSource;Active=C;
  UINTN G=C->ClockControllers[CC_TYPE_GCC].Address;
  *Reg(G)=BIT(31);*Reg(G+0x10)=31;*Reg(G+0x14)=0x4000;
  *Reg(G+0x18)=BIT(25)|(1U<<10);*Reg(G+0x62018)=BIT(19);
  *Reg(G+0x62000)=BIT(7);
  return C;
}
static ClockNode *Named(ClockDriverContext *C,CONST CHAR8 *Name) {
  ClockNode *N=NULL;assert(GetClockNode(C,Name,NULL,NULL,&N)==CR_SUCCESS);return N;
}
static void FreeCore(ClockDriverContext *C,VOID *S) {
  assert(CrSm8450ClockRelease(C)==CR_SUCCESS);
  for(UINTN I=0;I<((SM8450_CLOCK_STATE *)S)->NodeCount;++I)
    assert(((SM8450_CLOCK_STATE *)S)->Nodes[I].ReferenceCount==0);
  free(S);Active=NULL;
}
static void BootGraph(void) {
  VOID *S;ClockDriverContext *C=GpuCore(&S);
  UINTN G=C->ClockControllers[CC_TYPE_GCC].Address,U=C->ClockControllers[CC_TYPE_GPU_CC].Address;
  *Reg(U+0x9430)=1; /* Firmware supplied AHB /2: 150 MHz input -> 75 MHz. */
  ClockNode *Gmu=Named(C,"gpu_cc_cx_gmu_clk"),*Hub=Named(C,"gpu_cc_hub_cx_int_clk");
  ClockNode *Ahb=Named(C,"gpu_cc_ahb_clk"),*Cxo=Named(C,"gpu_cc_cxo_clk");
  ClockNode *Demet=Named(C,"gpu_cc_demet_clk"),*Pll=Named(C,"gcc_gpll0");
  ClockNode *Div=Named(C,"gcc_gpu_gpll0_div_clk_src"),*HubSrc=Named(C,"gpu_cc_hub_clk_src");
  CR_CLOCK_VOTER V={0};CR_CLOCK_VOTE_ENTRY E[8];
  assert(CrClockVotesInit(&V,C,E,8)==CR_SUCCESS);UINTN W=Writes,R=Reads;
  assert(CrClockVotesValidate(&V,Gmu,200000000)==CR_SUCCESS);
  assert(CrClockVotesValidate(&V,Hub,150000000)==CR_SUCCESS);
  assert(CrClockVotesValidate(&V,Ahb,75000000)==CR_SUCCESS);
  assert(CrClockVotesValidate(&V,Gmu,500000000)==CR_UNSUPPORTED);
  assert(CrClockVotesValidate(&V,Hub,149999999)==CR_UNSUPPORTED);
  assert(W==Writes&&R==Reads); /* Qualification never samples MMIO. */
  assert(CrClockVotesUninit(&V)==CR_SUCCESS);
  ClockNode *Raw=Named(C,"gpu_cc_gmu_clk_src");
  assert(ClockRcg2SetRate(C,Raw,200000000,CLOCK_RCG2_POLICY_CEIL)==CR_NOT_READY);
  assert(W==Writes); /* No RCG programming before actual parent ownership. */
  assert(ClockEnable(C,Gmu,200000000,TRUE)==CR_SUCCESS);
  assert((*Reg(U+0x931c)&0x10071fU)==(BIT(20)|0x602U));
  assert((*Reg(G+0x62000)&(BIT(7)|BIT(15)|BIT(16)))==(BIT(7)|BIT(16)));
  assert(Pll->ReferenceCount==1&&Div->ReferenceCount==1);
  assert(Pll->ActiveRateHz==600000000&&Div->ActiveRateHz==300000000);
  assert(ClockEnable(C,Hub,150000000,TRUE)==CR_SUCCESS);
  assert(ClockEnable(C,Ahb,75000000,TRUE)==CR_SUCCESS);
  assert(HubSrc->ReferenceCount==2&&Div->ReferenceCount==3);
  assert((*Reg(U+0x93f0)&0x10071fU)==(BIT(20)|0x603U));
  assert(*Reg(U+0x9430)==1&&*Reg(U+0x942c)==0);
  assert(ClockEnable(C,Cxo,19200000,TRUE)==CR_SUCCESS);
  assert(ClockEnable(C,Demet,19200000,TRUE)==CR_SUCCESS);
  assert(Named(C,"gpu_cc_xo_clk_src")->ReferenceCount==2);
  assert((*Reg(U+0x9014)&0x10071fU)==(BIT(20)|1));
  assert(ClockEnable(C,Hub,150000000,TRUE)==CR_SUCCESS&&Hub->ReferenceCount==2);
  *Reg(U+0x942c)=1;
  assert(CR_ERROR(ClockEnable(C,Hub,150000000,TRUE))&&Hub->ReferenceCount==2);
  *Reg(U+0x942c)=0;
  assert(ClockEnable(C,Hub,150000000,FALSE)==CR_SUCCESS&&Hub->ReferenceCount==1);
  assert(ClockEnable(C,Hub,150000000,FALSE)==CR_SUCCESS);
  assert(*Reg(U+0x9148)&BIT(0)); /* AON has no disable control operation. */
  assert(HubSrc->ReferenceCount==1&&Div->ReferenceCount==2);
  assert(ClockEnable(C,Ahb,75000000,FALSE)==CR_SUCCESS);
  assert(!HubSrc->ReferenceCount&&Div->ReferenceCount==1);
  assert(ClockEnable(C,Gmu,200000000,FALSE)==CR_SUCCESS);
  assert((*Reg(U+0x913c)&BIT(0))&&Div->ReferenceCount==0&&Pll->ReferenceCount==0);
  assert((*Reg(G+0x62000)&(BIT(7)|BIT(15)|BIT(16)))==BIT(7));
  assert((*Reg(G+0x62018)&(BIT(0)|BIT(19)))==BIT(19));
  FreeCore(C,S);
}
static void FailureAndIsolation(void) {
  VOID *S;ClockDriverContext *C=GpuCore(&S);UINTN U=C->ClockControllers[CC_TYPE_GPU_CC].Address;
  ClockNode *Hub=Named(C,"gpu_cc_hub_cx_int_clk"),*Div=Named(C,"gcc_gpu_gpll0_div_clk_src");
  *Reg(U+0x942c)=1; /* Requested150 MHz cannot be produced by actual /2. */
  assert(CR_ERROR(ClockEnable(C,Hub,150000000,TRUE)));
  assert(!Hub->ReferenceCount&&!Div->ReferenceCount&&(*Reg(U+0x9148)&BIT(0))==0);
  *Reg(U+0x942c)=0;
  BlockEnable=U+0x9148;assert(CR_ERROR(ClockEnable(C,Hub,150000000,TRUE)));
  assert(!Hub->ReferenceCount&&!Div->ReferenceCount);
  BlockEnable=0;
  assert(ClockEnable(C,Hub,150000000,TRUE)==CR_SUCCESS);
  *Reg(U+0x942c)=15; /* Cleanup must not depend on current divider/readiness. */
  assert(ClockEnable(C,Hub,150000000,FALSE)==CR_SUCCESS);
  assert(!Div->ReferenceCount);
  ClockNode *Gmu=Named(C,"gpu_cc_cx_gmu_clk");
  assert(ClockEnable(C,Gmu,200000000,TRUE)==CR_SUCCESS);
  VOID *Other=calloc(1,CrSm8450ClockStateSize());ClockDriverContext *B=NULL;
  assert(CrSm8450ClockCreate(Other,CrSm8450ClockStateSize(),&B)==CR_SUCCESS);
  assert(Named(B,"gpu_cc_cx_gmu_clk")!=Gmu&&!Named(B,"gpu_cc_cx_gmu_clk")->ReferenceCount);
  assert(Named(B,"gpu_cc_cx_gmu_clk")->Parents[0]!=Div);
  assert(Named(B,"gpu_cc_cx_gmu_clk")->ParentRates[0]==300000000);
  free(Other);FreeCore(C,S);
}
int main(void){BootGraph();FailureAndIsolation();puts("SM8450 GPU clock graph PASS: real parents, rates, shared refs, RO divider, AON and cleanup");return 0;}
