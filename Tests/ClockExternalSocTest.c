/* Actual shared ClockLib admission against public DISPCC layout fixtures. MIT */
static unsigned long long WatchedDivider,DividerWrites;
#define CR_CLOCK_TEST_WRITE_CALLBACK(A,O) do{if((A)==WatchedDivider)++DividerWrites;}while(0)
#define CR_CLOCK_EXTERNAL_FIXTURE_ONLY
#include "ClockExternalTest.c"
#include <Library/ClockExternalLease.h>

/* The baseline run deliberately exercises the former actual admission,
   which rejects the public non-SM8450 graph before any hardware access. */
#ifdef CR_CLOCK_SOC_API_BASELINE
#define CrClockExternalInitForSoc(E,C,S) CrClockExternalInit(E,C)
#endif

static VOID *Fixture(CR_CLOCK_EXTERNAL *E,UINT32 Soc,BOOLEAN Second) {
  VOID *Storage=ExternalFixture(E);
  ClockDriverContext *Core=E->Core;
  /* Source facts: Android kona/lahaina DISPCC. ClockLib algorithms and
     mapped graph storage are real; these are eligible synthetic graphs,
     not claims that the native targets currently publish DISPCC. */
  const UINT32 Sources[2][2]={{Soc==0x8250?0x2110:0x210c,Soc==0x8250?0x2098:0x2094},
    {Soc==0x8250?0x212c:0x2128,Soc==0x8250?0x20b0:0x20ac}};
  const UINT32 Gates[2][3]={{0x2028,0x202c,0x2004},{0x2030,0x2034,0x2008}};
  for(UINT32 I=0;I<2;++I) {
    ClockNode **L=I?E->SecondLeaf:E->Leaf,**S=I?E->SecondSource:E->Source;
    for(UINT32 J=0;J<3;++J)L[J]->EnableRegister=L[J]->HaltRegister=Gates[I][J];
    L[1]->DividerRegister=(Soc==0x8250?0x2128:0x2124)+I*0x1c;
    L[1]->DividerWidth=Soc==0x8250?2:4;
    for(UINT32 J=0;J<2;++J)S[J]->CmdRegister=Sources[I][J];
    if(I&&!Second) {
      for(UINT32 J=0;J<3;++J)L[J]->Name=NULL;
      for(UINT32 J=0;J<2;++J)S[J]->Name=NULL;
    }
  }
  E->Domain->GdscRegister=0x3000;
  E->Access[2]->EnableRegister=E->Access[2]->HaltRegister=Soc==0x8250?0x2080:0x207c;
  memset(E,0,sizeof(*E));E->Core=Core;return Storage;
}
static VOID InitCase(UINT32 Soc,BOOLEAN Second) {
  CR_CLOCK_EXTERNAL E;VOID *Storage=Fixture(&E,Soc,Second);ClockDriverContext *Core=E.Core;
  memset(&E,0,sizeof(E));UINTN R=Reads,W=Writes;
  assert(CrClockExternalInitForSoc(&E,Core,Soc)==CR_SUCCESS);
  assert(E.Initialized&&E.SecondPresent==Second&&Reads==R&&Writes==W);
  assert(CrClockExternalInitForSoc(&E,Core,Soc)==CR_ALREADY_STARTED&&Reads==R&&Writes==W);
  int Owner;assert(CrClockExternalReserve(&E,&Owner)==CR_SUCCESS);
  Dispose(&E,Storage,&Owner);
}
static VOID Refusals(UINT32 Soc) {
  CR_CLOCK_EXTERNAL E;VOID *Storage=Fixture(&E,Soc,TRUE);ClockDriverContext *Core=E.Core;
  ClockNode *Byte=NULL;assert(GetClockNode(Core,"disp_cc_mdss_byte0_clk",NULL,NULL,&Byte)==CR_SUCCESS);
  UINTN R=Reads,W=Writes;memset(&E,0,sizeof(E));
  UINT32 Gate=Byte->EnableRegister;Byte->EnableRegister+=4;
  assert(CrClockExternalInitForSoc(&E,Core,Soc)==CR_UNSUPPORTED&&!E.Initialized&&Reads==R&&Writes==W);
  Byte->EnableRegister=Gate;
  CONST CHAR8 *Name=Byte->Name;Byte->Name=NULL;
  assert(CrClockExternalInitForSoc(&E,Core,Soc)==CR_NOT_FOUND&&!E.Initialized&&Reads==R&&Writes==W);
  Byte->Name=Name;
  ClockNode *Pixel1=NULL;assert(GetClockNode(Core,"disp_cc_mdss_pclk1_clk",NULL,NULL,&Pixel1)==CR_SUCCESS);
  Name=Pixel1->Name;Pixel1->Name=NULL;
  assert(CrClockExternalInitForSoc(&E,Core,Soc)==CR_NOT_FOUND&&!E.Initialized&&Reads==R&&Writes==W);
  Pixel1->Name=Name;
  ClockController *Cc=&Core->ClockControllers[CC_TYPE_DISP_CC];UINT32 Size=Cc->Size;Cc->Size=0x3003;
  assert(CrClockExternalInitForSoc(&E,Core,Soc)!=CR_SUCCESS&&!E.Initialized&&Reads==R&&Writes==W);
  Cc->Size=Size;
  /* An explicit target selection never silently falls back to another SoC. */
  assert(CrClockExternalInitForSoc(&E,Core,0x8450)!=CR_SUCCESS&&!E.Initialized&&Reads==R&&Writes==W);
  free(Storage);Active=NULL;
}
static VOID Plans(UINT32 Soc,BOOLEAN ReadOnly) {
  CR_CLOCK_EXTERNAL E;VOID *Storage=Fixture(&E,Soc,TRUE);ClockDriverContext *Core=E.Core;
  ClockNode *Interface=NULL;assert(GetClockNode(Core,"disp_cc_mdss_byte0_intf_clk",NULL,NULL,&Interface)==CR_SUCCESS);
  Interface->DividerReadOnly=ReadOnly;
  memset(&E,0,sizeof(E));assert(CrClockExternalInitForSoc(&E,Core,Soc)==CR_SUCCESS);
  int Owner;assert(CrClockExternalReserve(&E,&Owner)==CR_SUCCESS);Access(&E,TRUE);
  UINTN Base=E.Source[0]->ParentController->Address;
  CR_CLOCK_DSI_PLAN Plan={.Revision=1,.Controller=0,.PixelHz=148500000,.ByteHz=111375000,
    .PixelSourceHz=148500000,.PixelM=1,.PixelN=1,.ByteInterfaceDivider=2};
  if(ReadOnly) {
    WatchedDivider=Base+E.Leaf[1]->DividerRegister;DividerWrites=0;
    *Reg(WatchedDivider)=0xabc00000;
    UINTN W=Writes;assert(CrClockExternalSetPlan(&E,&Owner,0,&Plan,TRUE)==CR_UNSUPPORTED);
    assert(Writes==W&&!E.PlanValid[0]&&!E.RecoveryPending);
    *Reg(WatchedDivider)=0xabc00001;
  }
  assert(CrClockExternalSetPlan(&E,&Owner,0,&Plan,TRUE)==CR_SUCCESS);
  assert((*Reg(Base+E.Source[0]->CmdRegister+4)&0x371f)==0x101);
  assert((*Reg(Base+E.Source[1]->CmdRegister+4)&0x371f)==0x101);
  if(ReadOnly)assert(!E.DividerSaved&&DividerWrites==0&&*Reg(WatchedDivider)==0xabc00001);
  if(ReadOnly) {
    UINTN W=Writes;CR_CLOCK_DSI_PLAN Q;BOOLEAN On,Recovery;
    *Reg(WatchedDivider)=0xabc00000;
    assert(CrClockExternalSetPlan(&E,&Owner,0,&Plan,TRUE)==CR_UNSUPPORTED&&Writes==W);
    assert(CrClockExternalGetPlan(&E,&Owner,0,&Q,&On,&Recovery)==CR_UNSUPPORTED&&Writes==W);
    /* Foreign divider drift must never prevent release of our leaf votes. */
    assert(CrClockExternalSetPlan(&E,&Owner,0,NULL,FALSE)==CR_SUCCESS&&DividerWrites==0);
    *Reg(WatchedDivider)=0xabc00001;
    assert(CrClockExternalSetPlan(&E,&Owner,0,&Plan,TRUE)==CR_SUCCESS);
  }
  UINT32 Value;assert(CrClockExternalRead(&E,&Owner,0x3000,&Value)==CR_SUCCESS);
  UINTN R=Reads;assert(CrClockExternalRead(&E,&Owner,0x9000,&Value)==CR_UNSUPPORTED&&Reads==R);
  assert(CrClockExternalNativeRetire(&E,&Owner,1,NULL)==CR_UNSUPPORTED&&Reads==R);
  BlockDisable=Base+E.Leaf[2]->EnableRegister;
  assert(CrClockExternalSetPlan(&E,&Owner,0,NULL,FALSE)==CR_TIMEOUT&&E.RecoveryPending&&E.Held[2]);
  BlockDisable=0;assert(CrClockExternalSetPlan(&E,&Owner,0,NULL,FALSE)==CR_SUCCESS);
  if(ReadOnly)assert(DividerWrites==0&&*Reg(WatchedDivider)==0xabc00001);
  WatchedDivider=0;
  Plan.Controller=1;
  assert(CrClockExternalSetPlan(&E,&Owner,1,&Plan,TRUE)==CR_SUCCESS);
  assert((*Reg(Base+E.SecondSource[0]->CmdRegister+4)&0x371f)==0x201);
  assert((*Reg(Base+E.SecondSource[1]->CmdRegister+4)&0x371f)==0x201);
  assert(CrClockExternalSetPlan(&E,&Owner,1,NULL,FALSE)==CR_SUCCESS);
  Dispose(&E,Storage,&Owner);
}
static VOID LeaseAdmission(UINT32 Soc) {
  CR_CLOCK_EXTERNAL E;VOID *Storage=Fixture(&E,Soc,FALSE);ClockDriverContext *Core=E.Core;
  CR_CLOCK_EXTERNAL_LEASE L={0};UINTN R=Reads,W=Writes;
  assert(CrClockExternalLeaseInitForSoc(&L,Core,Soc)==CR_SUCCESS&&Reads==R&&Writes==W);
  assert(CrClockExternalLeaseInitForSoc(&L,Core,Soc)==CR_ALREADY_STARTED&&Reads==R&&Writes==W);
  assert(CrClockExternalUninit(&L.External)==CR_SUCCESS);
  free(Storage);Active=NULL;
}
#ifndef CR_CLOCK_EXTERNAL_SOC_MAIN
#define CR_CLOCK_EXTERNAL_SOC_MAIN main
#endif
int CR_CLOCK_EXTERNAL_SOC_MAIN(void) {
  InitCase(0x8250,FALSE);InitCase(0x8250,TRUE);
  InitCase(0x8350,FALSE);InitCase(0x8350,TRUE);
  Refusals(0x8250);Refusals(0x8350);
  Plans(0x8250,FALSE);Plans(0x8250,TRUE);Plans(0x8350,FALSE);
  LeaseAdmission(0x8250);LeaseAdmission(0x8350);
  puts("Clock external SoC: public graph admission, pure init, missing/partial/mismatched graph refusals passed");
  return 0;
}
