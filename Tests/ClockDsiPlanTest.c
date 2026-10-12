/* Actual DSI plan -> ClockLib/DISPCC ownership integration. MIT */
#include <stdint.h>
static uintptr_t DropWriteAddress;
#define CR_CLOCK_TEST_WRITE_CALLBACK(A,Old) do {if((A)==DropWriteAddress)*Reg(A)=(Old);}while(0)
#define CR_CLOCK_EXTERNAL_FIXTURE_ONLY
#include "ClockExternalTest.c"
#include <Library/MdssDsiCrLib.h>
#ifndef _KERNEL_MODE
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
static CR_STATUS Admission=CR_SUCCESS;
static CR_STATUS ReadyOwner(VOID *C){assert(C);return Admission;}
static CR_MDSS_DSI_MODE TestMode(UINT32 Cphy,UINT32 Count,UINT32 First,UINT32 Lanes,UINT64 Hz) {
  CR_MDSS_DSI_MODE M={0};M.Revision=1;M.Controllers=Count;M.FirstController=First;
  M.PhyType=Cphy?CrMdssCphy:CrMdssDphy;M.PixelHz=Hz;M.ContinuousClock=TRUE;
  M.Raster=(CR_MDSS_MODE){.Soc=0x8450,.Width=1920,.Height=1080,.Stride=7680,
    .HSync=44,.HBackPorch=148,.HFrontPorch=88,.VSync=5,.VBackPorch=36,.VFrontPorch=4,
    .RefreshQ16=(UINT32)(Hz*65536/(2200U*1125U)),.Controller=First,.Lanes=Lanes};
  for(UINT32 I=0;I<4;++I)M.LaneMap[I]=(UINT8)(1U<<I);return M;
}
static VOID CheckRate(CR_CLOCK_EXTERNAL *E,UINT32 I,CONST CR_CLOCK_DSI_PLAN *P) {
  ClockNode **L=I?E->SecondLeaf:E->Leaf;ClockNode **S=I?E->SecondSource:E->Source;
  UINTN B=S[0]->ParentController->Address;UINT32 Byte=*Reg(B+S[0]->CmdRegister+4);
  UINT32 Pixel=*Reg(B+S[1]->CmdRegister+4);
  assert((Byte&0x371fU)==((2U+I*2U)<<8|1U));
  assert((Pixel&0x371fU)==((1U+I*2U)<<8|1U|(P->PixelM==P->PixelN?0U:0x2000U)));
  assert((*Reg(B+L[1]->DividerRegister)&15U)==P->ByteInterfaceDivider-1);
  if(P->PixelM!=P->PixelN) {
    assert((*Reg(B+S[1]->CmdRegister+8)&255U)==P->PixelM);
    assert((*Reg(B+S[1]->CmdRegister+12)&255U)==((~(P->PixelN-P->PixelM))&255U));
    assert((*Reg(B+S[1]->CmdRegister+16)&255U)==((~P->PixelN)&255U));
  }
}
int main(void) {
  UINT32 Vectors=0;int Owner,Foreign;CR_CLOCK_EXTERNAL E;BOOLEAN Owned,On,Pending;
  VOID *Storage=ExternalFixture(&E);assert(E.SecondPresent);
  UINTN R=Reads,W=Writes;
  assert(CrClockExternalGroupConflict(&E,E.SecondLeaf[2])==CR_BUSY && Reads==R&&Writes==W);
  E.SecondSource[1]->ReferenceCount=1;
  assert(CrClockExternalReserve(&E,&Owner)==CR_BUSY && Reads==R&&Writes==W);
  E.SecondSource[1]->ReferenceCount=0;
  assert(CrClockExternalReserve(&E,&Owner)==CR_SUCCESS);Access(&E,TRUE);
  CR_MDSS_DSI_CLOCK_OWNER C={&E,&Owner,&Owner,ReadyOwner};
  for(UINT32 Phy=0;Phy<2;++Phy)for(UINT32 Count=1;Count<=2;++Count)
  for(UINT32 First=0;First<=2-Count;++First)for(UINT32 Lanes=1;Lanes<=(Phy?3U:4U);++Lanes)
  for(UINT64 Hz=16000001;Hz<=300000000;Hz+=7100000) {
    CR_MDSS_DSI_MODE M=TestMode(Phy,Count,First,Lanes,Hz);CR_MDSS_DSI_PLAN P;
    if(CrMdssDsiBuildPlan(&M,&P)!=CR_SUCCESS)continue;
    CR_CLOCK_DSI_PLAN Plan[2];
    for(UINT32 I=First;I<First+Count;++I) {
      assert(CrMdssDsiClockPlan(&P,I,&Plan[I])==CR_SUCCESS);
      assert(CrMdssDsiClockOwnerSet(&C,I,&P,TRUE,&Owned)==CR_SUCCESS&&Owned);
      CheckRate(&E,I,&Plan[I]);
      CR_CLOCK_DSI_PLAN Q;
      assert(CrClockExternalGetPlan(&E,&Owner,I,&Q,&On,&Pending)==CR_SUCCESS&&On&&!Pending);
      assert(CrClockDsiPlanEqual(&Q,&Plan[I]));
      W=Writes;assert(CrMdssDsiClockOwnerSet(&C,I,&P,TRUE,&Owned)==CR_SUCCESS&&Owned&&Writes==W);
      Q.ByteHz+=2;
      assert(CrClockExternalSetPlan(&E,&Owner,I,&Q,TRUE)==CR_BUSY&&Writes==W);
      assert(CrClockExternalSetPlan(&E,&Foreign,I,NULL,FALSE)==CR_BUSY&&Writes==W);
    }
    for(UINT32 I=First+Count;I-- >First;)assert(CrMdssDsiClockOwnerSet(&C,I,&P,FALSE,&Owned)==CR_SUCCESS&&!Owned);
    ++Vectors;
  }
  Dispose(&E,Storage,&Owner);
  /* Posted UPDATE completion alone is insufficient: dropped selector/M/N
     writes and failed full snapshot restoration must retain actual ownership. */
  for(UINT32 I=0;I<2;++I)for(UINT32 Offset=4;Offset<=16;Offset+=4) {
    Storage=ExternalFixture(&E);assert(CrClockExternalReserve(&E,&Owner)==CR_SUCCESS);Access(&E,TRUE);
    C=(CR_MDSS_DSI_CLOCK_OWNER){&E,&Owner,&Owner,ReadyOwner};
    ClockNode **Sources=I?E.SecondSource:E.Source;UINTN B=Sources[1]->ParentController->Address;
    CR_CLOCK_DSI_PLAN Plan={1,I,74250000,55687500,111375000,2,3,1,0};
    DropWriteAddress=B+Sources[1]->CmdRegister+Offset;
    assert(CrClockExternalSetPlan(&E,&Owner,I,&Plan,TRUE)==CR_DEVICE_ERROR&&E.PlanValid[I]);
    W=Writes;assert(CrClockExternalLegacyGuard(&E,E.Access[0])==CR_BUSY&&Writes==W);
    DropWriteAddress=0;assert(CrClockExternalSetPlan(&E,&Owner,I,NULL,FALSE)==CR_SUCCESS);
    /* Seed a distinct original image, then lose exactly its restore write. */
    *Reg(B+Sources[1]->CmdRegister+Offset)=0x554400a5;
    assert(CrClockExternalSetPlan(&E,&Owner,I,&Plan,TRUE)==CR_SUCCESS);
    DropWriteAddress=B+Sources[1]->CmdRegister+Offset;
    assert(CrClockExternalSetPlan(&E,&Owner,I,NULL,FALSE)==CR_DEVICE_ERROR&&E.PlanValid[I]);
    assert(CrClockExternalReleaseOwner(&E,&Owner)==CR_DEVICE_ERROR&&E.Owner==&Owner);
    DropWriteAddress=0;assert(CrClockExternalSetPlan(&E,&Owner,I,NULL,FALSE)==CR_SUCCESS);
    assert(*Reg(B+Sources[1]->CmdRegister+Offset)==0x554400a5);Dispose(&E,Storage,&Owner);
  }
  CR_MDSS_DSI_MODE M=TestMode(1,2,0,2,148500000);CR_MDSS_DSI_PLAN P;
  assert(CrMdssDsiBuildPlan(&M,&P)==CR_SUCCESS);
  for(UINT32 I=0;I<2;++I)for(UINT32 Fail=0;Fail<3;++Fail) {
    Storage=ExternalFixture(&E);assert(CrClockExternalReserve(&E,&Owner)==CR_SUCCESS);Access(&E,TRUE);
    C=(CR_MDSS_DSI_CLOCK_OWNER){&E,&Owner,&Owner,ReadyOwner};
    ClockNode **L=I?E.SecondLeaf:E.Leaf;
    BlockEnable=L[Fail]->ParentController->Address+L[Fail]->EnableRegister;
    assert(CrMdssDsiClockOwnerSet(&C,I,&P,TRUE,&Owned)==CR_TIMEOUT&&Owned);
    W=Writes;assert(CrClockExternalLegacyGuard(&E,E.Access[0])==CR_BUSY&&Writes==W);
    Admission=CR_NOT_READY;
    assert(CrMdssDsiClockOwnerSet(&C,I,&P,FALSE,&Owned)==CR_NOT_READY&&Owned&&Writes==W);
    Admission=CR_SUCCESS;BlockEnable=0;
    assert(CrMdssDsiClockOwnerSet(&C,I,&P,FALSE,&Owned)==CR_SUCCESS&&!Owned);
    Dispose(&E,Storage,&Owner);
  }
  Storage=ExternalFixture(&E);assert(CrClockExternalReserve(&E,&Owner)==CR_SUCCESS);Access(&E,TRUE);
  C=(CR_MDSS_DSI_CLOCK_OWNER){&E,&Owner,&Owner,ReadyOwner};
  assert(CrMdssDsiClockOwnerSet(&C,0,&P,TRUE,&Owned)==CR_SUCCESS&&Owned);
  assert(CrMdssDsiClockOwnerSet(&C,1,&P,TRUE,&Owned)==CR_SUCCESS&&Owned);
  BlockDisable=E.SecondLeaf[2]->ParentController->Address+E.SecondLeaf[2]->EnableRegister;
  assert(CrClockExternalReleaseOwner(&E,&Owner)==CR_TIMEOUT&&E.Owner==&Owner&&E.Enabled&&E.SecondHeld[2]);
  assert(CrClockExternalGet(&E,&Owner,&On,&Pending)==CR_SUCCESS&&On&&Pending);
  BlockDisable=0;assert(CrClockExternalReleaseOwner(&E,&Owner)==CR_SUCCESS&&!E.Owner&&!E.Enabled&&!E.SecondEnabled);
  Access(&E,FALSE);assert(CrClockExternalUninit(&E)==CR_SUCCESS);free(Storage);Active=NULL;
  printf("Actual DSI/CPHY neutral plan + dual DISPCC provider: %u rate vectors, 6 partial enable phases, 16 dropped selector/MND/restore phases, retained dual release PASS\n",Vectors);
  return 0;
}
