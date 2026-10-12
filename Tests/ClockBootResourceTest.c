/* Actual selected targets and shared boot lease/retirement. Hardware votes and
 * domain-dependent AHB acknowledgements are independent MMIO models. MIT */
#include <stdint.h>
static void BootWrite(uintptr_t Address,uint32_t Old);
#define CR_CLOCK_TEST_WRITE_CALLBACK(A,O) BootWrite((A),(O))
#define CR_CLOCK_TARGET_DISPLAY_MAIN PriorTargetDisplay
#include "ClockTargetDisplayTest.c"
#include <Library/ClockExternalLease.h>
static UINTN Power,Ahb,Lut,Fault,Source,AliveRoots[4],AliveRootCount,Events[64],EventCount;
static BOOLEAN Collapsed,VotedAlive,LocalLoss,OrdinaryAlive,RcgPending;
static VOID BootWrite(uintptr_t Address,uint32_t Old) {
  if(Address==Power) {
    *Reg(Address)=(*Reg(Address)&~BIT(31))|(Old&BIT(31));
    if(AutoPower){if(*Reg(Address)&BIT(0))*Reg(Address)&=~BIT(31);else *Reg(Address)|=BIT(31);}
    if(((Old^*Reg(Address))&(BIT(0)|BIT(31)))&&EventCount<ARRAY_SIZE(Events))Events[EventCount++]=(*Reg(Address)&BIT(0))?4:1;
  }
  if(Address==Ahb) {
    if((*Reg(Address)&BIT(0))&&Collapsed&&!(*Reg(Power)&BIT(31)))*Reg(Address)|=BIT(31);
    if(((Old^*Reg(Address))&BIT(0))&&EventCount<ARRAY_SIZE(Events))Events[EventCount++]=(*Reg(Address)&BIT(0))?2:3;
  }
  if(Address==Lut&&VotedAlive)*Reg(Address)&=~BIT(31);
  if(Address==Fault&&LocalLoss)*Reg(Address)=Old;
  if(Address==Fault&&OrdinaryAlive)*Reg(Address)&=~BIT(31);
  if(Address==Source&&RcgPending)*Reg(Address)|=BIT(0);
  /* A hardware consumer keeps aggregate ROOT_OFF clear independently of
     this owner's local force request. Do not synthesize that status from
     the software enable bit as the base portable fixture does. */
  for(UINTN I=0;I<AliveRootCount;++I)if(Address==AliveRoots[I])*Reg(Address)&=~BIT(31);
}
static CONST CHAR8 *Names[]={"disp_cc_mdss_mdp_clk","disp_cc_mdss_mdp_lut_clk","disp_cc_mdss_vsync_clk",
  "disp_cc_mdss_esc0_clk","disp_cc_mdss_esc1_clk","disp_cc_mdss_mdp_clk_src",
  "disp_cc_mdss_vsync_clk_src","disp_cc_mdss_esc0_clk_src","disp_cc_mdss_esc1_clk_src"};
static VOID BootCheckStatus(CR_STATUS Got,CR_STATUS Expected,CONST CHAR8 *Case){
  if(Got!=Expected)fprintf(stderr,"%s status=%llx expected=%llx\n",Case,(unsigned long long)Got,(unsigned long long)Expected);
  assert(Got==Expected);
}
int main(int argc,char **argv) {
  assert(argc==2);Reset();ClockDriverContext *C=CrTargetGetClockContext();
#ifdef _KERNEL_MODE
  C->ClockControllers[CC_TYPE_GCC].MemMapped=TRUE;C->ClockControllers[CC_TYPE_DISP_CC].MemMapped=TRUE;
#endif
  assert(ClockLibInit(&C)==CR_SUCCESS);Active=C;assert(CrTargetClockInit(C)==CR_SUCCESS);
  UINTN B=C->ClockControllers[CC_TYPE_DISP_CC].Address;PllState(B);
  CR_CLOCK_EXTERNAL_LEASE L={0};assert(CrClockExternalLeaseInitForSoc(&L,C,TEST_SOC)==CR_SUCCESS);
  Power=B+L.External.Domain->GdscRegister;Ahb=B+L.External.Access[2]->EnableRegister;Lut=B+0x201c;
  Collapsed=strcmp(argv[1],"collapsed")==0||strcmp(argv[1],"ahb-failure")==0;
  *Reg(Power)=Collapsed||strstr(argv[1],"release")||strcmp(argv[1],"shared-votes")==0?BIT(0)|BIT(10):BIT(31)|BIT(10);
  if(strcmp(argv[1],"domain-failure")==0){*Reg(Power)=0;AutoPower=FALSE;}
  if(strcmp(argv[1],"ahb-failure")==0)BlockEnable=BlockDisable=Ahb;
  int Owner;BOOLEAN Owned=FALSE;CR_STATUS S=CrClockExternalLeaseReserve(&L,&Owner,&Owned);
  if(strcmp(argv[1],"domain-failure")==0||strcmp(argv[1],"ahb-failure")==0){
    BootCheckStatus(S,CR_TIMEOUT,argv[1]);assert(Owned&&L.CleanupPending&&!L.Ready);
    assert(L.External.Owner==&Owner&&L.DomainOwned);
    if(strcmp(argv[1],"domain-failure")==0)assert(!L.AccessOwned[2]);
    else assert(L.AccessOwned[2]);
    AutoPower=TRUE;BlockEnable=BlockDisable=0;assert(CrClockExternalLeaseRelease(&L,&Owner)==CR_SUCCESS);
    assert(!L.External.Owner&&!L.DomainOwned);puts("retained failed lease cleanup PASS");return 0;
  }
  BootCheckStatus(S,CR_SUCCESS,argv[1]);assert(Owned&&L.Ready&&L.DomainOwned&&L.AccessOwned[2]);
  if(strcmp(argv[1],"shared-votes")==0){
    for(UINT32 I=0;I<3;++I)assert(ClockAcquire(C,L.External.Access[I],I==2?19200000:0,&Owned)==CR_SUCCESS&&Owned);
    assert(ClockGdscAcquire(C,L.External.Domain,&Owned)==CR_SUCCESS&&Owned);UINTN W=Writes;
    assert(CrClockExternalLeaseRelease(&L,&Owner)==CR_SUCCESS&&Writes==W);
    for(UINT32 I=0;I<3;++I)assert(L.External.Access[I]->ReferenceCount==1);
    assert(L.External.Domain->ReferenceCount==1);
    assert(ClockEnable(C,L.External.Access[2],0,FALSE)==CR_SUCCESS);
    assert(ClockGdscSet(C,L.External.Domain,FALSE)==CR_SUCCESS);
    for(UINT32 I=2;I-- >0;)assert(ClockEnable(C,L.External.Access[I],0,FALSE)==CR_SUCCESS);
    puts("foreign shared votes preserved PASS");return 0;
  }
  if(strcmp(argv[1],"child-release-failure")==0||strcmp(argv[1],"domain-release-failure")==0){
    BOOLEAN Child=strcmp(argv[1],"child-release-failure")==0;
    if(Child)BlockDisable=Ahb;else AutoPower=FALSE;
    assert(CrClockExternalLeaseRelease(&L,&Owner)==CR_TIMEOUT);
    assert(L.External.Owner==&Owner&&L.DomainOwned&&L.AccessOwned[0]&&L.AccessOwned[1]);
    assert(L.AccessOwned[2]==Child&&L.CleanupPending&&!L.Ready);
    if(Child)assert(L.External.Domain->ReferenceCount==1&&!L.External.Domain->ReleasePending);
    else assert(L.External.Domain->ReleasePending);
    BlockDisable=0;AutoPower=TRUE;assert(CrClockExternalLeaseRelease(&L,&Owner)==CR_SUCCESS);
    assert(!L.DomainOwned&&!L.External.Owner);puts("inverse release failure retains prerequisite suppliers PASS");return 0;
  }
  if(strcmp(argv[1],"collapsed")==0||strcmp(argv[1],"release-order")==0){
    assert(EventCount>=2&&Events[0]==1&&Events[1]==2);EventCount=0;
    assert(CrClockExternalLeaseRelease(&L,&Owner)==CR_SUCCESS);
    assert(EventCount>=2&&Events[0]==3&&Events[1]==4);puts("domain-before-AHB and reverse release PASS");return 0;
  }
  ClockNode *Functions[9];for(UINT32 I=0;I<9;++I){Functions[I]=DisplayNode(C,Names[I]);
    UINT32 Offset=I<5?Functions[I]->EnableRegister:Functions[I]->CmdRegister;
    *Reg(B+Offset)=BIT(22)|(I<5?BIT(0):BIT(1));}
  UINT32 Config[4];for(UINT32 I=0;I<4;++I){Config[I]=0x22000400U+I;*Reg(B+Functions[I+5]->CmdRegister+4)=Config[I];}
  UINT32 Rscc=BIT(22)|BIT(0);*Reg(B+0x4008)=Rscc;
  assert(Functions[1]->HaltCheckFlag&CLOCK_NODE_BRANCH_VOTED);
  assert(CrClockExternalLeaseResetCore(&L,&Owner,7,TRUE)==CR_SUCCESS);
  assert(CrClockExternalLeaseResetCore(&L,&Owner,7,FALSE)==CR_SUCCESS);
  if(strcmp(argv[1],"descriptor-drift")==0){
    Functions[6]->CmdRegister+=4;UINTN W=Writes;
    BootCheckStatus(CrClockExternalLeaseRetireInheritedFunctional(&L,&Owner,7),CR_UNSUPPORTED,argv[1]);
    assert(Writes==W&&!L.External.FunctionalTouched&&!L.External.FunctionalComplete);
    Functions[6]->CmdRegister-=4;assert(CrClockExternalLeaseRetireInheritedFunctional(&L,&Owner,7)==CR_SUCCESS);
    assert(CrClockExternalLeaseRelease(&L,&Owner)==CR_SUCCESS);puts("unqualified source rejected before withdrawal PASS");return 0;
  }
  BOOLEAN RootSuccess=FALSE,CmdZero=strcmp(argv[1],"vsync-cmd-zero")==0;
  if(strcmp(argv[1],"voted-alive")==0)VotedAlive=TRUE;
  else if(strcmp(argv[1],"local-loss")==0){Fault=Lut;LocalLoss=TRUE;}
  else if(strcmp(argv[1],"ordinary-alive")==0){Fault=B+0x200c;OrdinaryAlive=TRUE;}
  else if(strcmp(argv[1],"rcg-pending")==0){Source=B+Functions[6]->CmdRegister;RcgPending=TRUE;}
  else if(strcmp(argv[1],"rcg-force-loss")==0){Fault=B+Functions[6]->CmdRegister;LocalLoss=TRUE;}
  else if(strcmp(argv[1],"roots-active")==0){
    for(UINT32 I=0;I<4;++I)AliveRoots[AliveRootCount++]=B+Functions[I+5]->CmdRegister;RootSuccess=TRUE;
  }
  else if(CmdZero){AliveRoots[AliveRootCount++]=B+Functions[6]->CmdRegister;*Reg(AliveRoots[0])=0;RootSuccess=TRUE;}
  else if(strncmp(argv[1],"root-active-",12)==0){
    UINT32 I=(UINT32)(argv[1][12]-'0');assert(I<4&&argv[1][13]==0);
    AliveRoots[AliveRootCount++]=B+Functions[I+5]->CmdRegister;RootSuccess=TRUE;
  }
  else assert(0);
  S=CrClockExternalLeaseRetireInheritedFunctional(&L,&Owner,7);
  if(VotedAlive||RootSuccess){BootCheckStatus(S,CR_SUCCESS,argv[1]);assert(L.External.FunctionalComplete);
    if(VotedAlive)assert(!(*Reg(Lut)&(BIT(0)|BIT(31))));
    for(UINTN I=0;I<AliveRootCount;++I)assert(!(*Reg(AliveRoots[I])&(BIT(0)|BIT(1)|BIT(31))));
    if(CmdZero)assert(*Reg(AliveRoots[0])==0);
  }
  else {BootCheckStatus(S,LocalLoss?CR_DEVICE_ERROR:CR_TIMEOUT,argv[1]);assert(!L.External.FunctionalComplete);
    assert(CrClockExternalLeaseRelease(&L,&Owner)==CR_NOT_READY&&L.Ready&&L.DomainOwned);
    assert(L.External.Owner==&Owner&&L.External.FunctionalTouched&&L.AccessOwned[0]&&L.AccessOwned[1]&&L.AccessOwned[2]);
    if(RcgPending||Fault==B+Functions[6]->CmdRegister)assert(*Reg(B+Functions[7]->CmdRegister)&BIT(1));
    else assert(*Reg(B+Functions[5]->CmdRegister)&BIT(1));
    LocalLoss=OrdinaryAlive=RcgPending=FALSE;
    assert(CrClockExternalLeaseRetireInheritedFunctional(&L,&Owner,7)==CR_SUCCESS);}
  for(UINT32 I=0;I<9;++I){UINT32 Offset=I<5?Functions[I]->EnableRegister:Functions[I]->CmdRegister;
    if(!CmdZero||I!=6)assert(*Reg(B+Offset)&BIT(22));assert(!(*Reg(B+Offset)&(I<5?BIT(0):BIT(1))));}
  for(UINT32 I=0;I<4;++I)assert(*Reg(B+Functions[I+5]->CmdRegister+4)==Config[I]);
  assert(*Reg(B+0x4008)==Rscc);
  assert(CrClockExternalLeaseRelease(&L,&Owner)==CR_SUCCESS);puts("functional retirement with independent fault model PASS");return 0;
}
