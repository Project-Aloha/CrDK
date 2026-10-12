/* Fixed PLL qualification and retained local vote. SPDX-License-Identifier: MIT */
#include <Library/FixedPllVoteLib.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_time.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_mutex.h>
CR_STATUS CrInheritedPllCheck(CONST CR_INHERITED_PLL_DESCRIPTION *D,UINT64 RateHz) {
  /* Both public direct Lucid layouts use MODE/L/ALPHA/USER/OPMODE here.
     SM8250 FSM is MODE.bit20; SM8350 LUCID_5LPE is USER.bit21. */
  STATIC CONST UINT32 Offsets[]={0,4,0x40,0x0c,0x38};
  if(!D || !D->Base || (D->Base&3U) || D->Size<0x44 ||
      D->Base>(UINTN)-1-D->Size || !D->ParentHz ||
      D->ExpectedL>0xffffU || D->ExpectedAlpha>0xffffU ||
      D->ParentHz>~((UINT64)0)/0x10000U ||
      D->ParentHz>~((UINT64)0)/(D->ExpectedL+1ULL) ||
      D->FsmInModeRegister>1)return CR_INVALID_PARAMETER;
  UINT64 Nominal=D->ParentHz*D->ExpectedL+((D->ParentHz*D->ExpectedAlpha)>>16);
  if(Nominal!=D->FrequencyHz)return CR_INVALID_PARAMETER;
  if(RateHz!=Nominal)return CR_UNSUPPORTED;
  UINT32 Before[5],After[5];
  for(UINT32 I=0;I<ARRAY_SIZE(Offsets);++I)Before[I]=CrMmioRead32(D->Base+Offsets[I]);
  CrMemoryFence();
  for(UINT32 I=0;I<ARRAY_SIZE(Offsets);++I)After[I]=CrMmioRead32(D->Base+Offsets[I]);
  UINT32 Refusal=0;
  for(UINT32 I=0;I<ARRAY_SIZE(Offsets);++I)if(Before[I]!=After[I])Refusal|=BIT(6);
  if((Before[0]&(BIT(31)|BIT(2)|BIT(0)))!=(BIT(31)|BIT(2)|BIT(0)))Refusal|=BIT(0);
  if(D->FsmInModeRegister?(Before[0]&BIT(20)):(Before[3]&BIT(21)))Refusal|=BIT(1);
  if((Before[1]&0xffffU)!=D->ExpectedL)Refusal|=BIT(2);
  if((Before[2]&0xffffU)!=D->ExpectedAlpha)Refusal|=BIT(3);
  /* Public SM8250/SM8350 MDP parent maps consume OUT_MAIN. EVEN and ODD
     gates are independent USER outputs and are not prerequisites for MAIN.
     Observe the required gate without changing any inherited output. */
  if(!(Before[3]&BIT(0)))Refusal|=BIT(4);
  if(Before[4]!=1U)Refusal|=BIT(5);
  if(Refusal){
    log_err("Inherited PLL base=" CR_LOG_HEX64_FMT " rate=" CR_LOG_UINT64_FMT
      " expected=" CR_LOG_UINT64_FMT " reject=0x%x FSM-in-MODE=%u MODE=0x%x L=0x%x ALPHA=0x%x USER=0x%x OPMODE=0x%x MAIN=%u",
      (UINT64)D->Base,RateHz,D->FrequencyHz,Refusal,(UINT32)D->FsmInModeRegister,
      Before[0],Before[1],Before[2],Before[3],Before[4],Before[3]&1U);
    if(Refusal&BIT(6))log_err("Inherited PLL second snapshot MODE=0x%x L=0x%x ALPHA=0x%x USER=0x%x OPMODE=0x%x",
      After[0],After[1],After[2],After[3],After[4]);
    return CR_NOT_READY;
  }
  return CR_SUCCESS;
}
STATIC BOOLEAN Fits(CONST CR_FIXED_PLL_DESCRIPTION *D,UINT32 Offset) {
  return !(Offset&3U) && Offset<=D->Size && D->Size-Offset>=4;
}
CR_STATUS CrFixedPllInit(CR_FIXED_PLL_VOTE *P,CONST CR_FIXED_PLL_DESCRIPTION *D) {
  if(P==NULL || D==NULL)return CR_INVALID_PARAMETER;
  if(P->Initialized || P->Owned)return CR_ALREADY_STARTED;
  if(!D->Base || (D->Base&3U) || D->Size<4 || D->Base>~((UINTN)0)-D->Size ||
     !D->VoteMask || (D->VoteMask&(D->VoteMask-1)) || !D->FsmMask || !D->LockMask ||
     (D->PostDividerValue&~D->PostDividerMask) ||
     D->FsmInModeRegister>1 ||
     (!D->FsmInModeRegister && (D->PostDividerMask&D->FsmMask)) ||
     D->ExpectedL>0xffffU || D->ExpectedAlpha>0xffffU ||
     (D->LockMask&D->CalibrationPendingMask) ||
     D->VoteRegister==D->ModeRegister || D->VoteRegister==D->UserRegister ||
     D->VoteRegister==D->LRegister || D->VoteRegister==D->AlphaRegister ||
     D->ModeRegister==D->UserRegister || D->ModeRegister==D->LRegister ||
     D->ModeRegister==D->AlphaRegister || D->UserRegister==D->LRegister ||
     D->UserRegister==D->AlphaRegister || D->LRegister==D->AlphaRegister ||
     !D->ParentHz || !D->FrequencyHz || D->ParentHz>~((UINT64)0)/0x10000U ||
     D->ParentHz>~((UINT64)0)/(D->ExpectedL+1ULL) ||
     !Fits(D,D->VoteRegister) || !Fits(D,D->ModeRegister) || !Fits(D,D->UserRegister) ||
     !Fits(D,D->LRegister) || !Fits(D,D->AlphaRegister))return CR_INVALID_PARAMETER;
  UINT64 Rate=D->ParentHz*D->ExpectedL+((D->ParentHz*D->ExpectedAlpha)>>16);
  if(Rate!=D->FrequencyHz)return CR_INVALID_PARAMETER;
  P->Description=*D;P->Ready=FALSE;P->RestoreVote=FALSE;P->Initialized=TRUE;
  return CR_SUCCESS;
}
STATIC UINT32 Read(CR_FIXED_PLL_VOTE *P,UINT32 Offset) {
  return CrMmioRead32(P->Description.Base+Offset);
}
STATIC CR_STATUS Configuration(CR_FIXED_PLL_VOTE *P) {
  CONST CR_FIXED_PLL_DESCRIPTION *D=&P->Description;
  UINT32 User=Read(P,D->UserRegister);
  /* Linux/Android fixed LUCID reads MODE; EVO reads USER. Direct/non-FSM
   * analogue startup belongs to a separate owner in either case. */
  UINT32 Fsm=D->FsmInModeRegister?Read(P,D->ModeRegister):User;
  if((Fsm&D->FsmMask)!=D->FsmMask)return CR_UNSUPPORTED;
  if((User&D->PostDividerMask)!=D->PostDividerValue ||
     (Read(P,D->LRegister)&0xffffU)!=D->ExpectedL ||
     (Read(P,D->AlphaRegister)&0xffffU)!=D->ExpectedAlpha)return CR_NOT_READY;
  if(Read(P,D->ModeRegister)&D->CalibrationPendingMask)return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrFixedPllCheck(CR_FIXED_PLL_VOTE *P) {
  if(P==NULL || !P->Initialized)return CR_INVALID_PARAMETER;
  if(!P->Owned || !P->Ready)return CR_NOT_READY;
  CR_STATUS S=Configuration(P);
  if(CR_ERROR(S))return S;
  if(!(Read(P,P->Description.VoteRegister)&P->Description.VoteMask) ||
     (Read(P,P->Description.ModeRegister)&P->Description.LockMask)!=P->Description.LockMask)
    return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrFixedPllAcquire(CR_FIXED_PLL_VOTE *P) {
  if(P==NULL || !P->Initialized)return CR_INVALID_PARAMETER;
  if(P->Owned)return CrFixedPllCheck(P);
  CR_STATUS S=Configuration(P);
  if(CR_ERROR(S))return S;
  CONST CR_FIXED_PLL_DESCRIPTION *D=&P->Description;
  P->RestoreVote=(Read(P,D->VoteRegister)&D->VoteMask)!=0;
  /* An attempted write may have committed even when subsequent readback
   * fails. Publish cleanup ownership before changing the vote. */
  P->Owned=TRUE;P->Ready=FALSE;
  CrMmioUpdateBits32(D->Base+D->VoteRegister,D->VoteMask,D->VoteMask);
  CrMemoryFence();
  if(!(Read(P,D->VoteRegister)&D->VoteMask))return CR_DEVICE_ERROR;
  /* Same bounded lock observation as current upstream wait_for_pll. */
  for(UINT32 I=0;I<1500;++I) {
    if((Read(P,D->ModeRegister)&D->LockMask)==D->LockMask) {
      S=Configuration(P);
      if(CR_ERROR(S))return S;
      P->Ready=TRUE;return CrFixedPllCheck(P);
    }
    cr_sleep(1);
  }
  return CR_TIMEOUT;
}
CR_STATUS CrFixedPllRelease(CR_FIXED_PLL_VOTE *P) {
  if(P==NULL || !P->Initialized)return CR_INVALID_PARAMETER;
  if(!P->Owned)return CR_SUCCESS;
  CONST CR_FIXED_PLL_DESCRIPTION *D=&P->Description;
  P->Ready=FALSE;
  UINT32 Value=P->RestoreVote?D->VoteMask:0;
  CrMmioUpdateBits32(D->Base+D->VoteRegister,D->VoteMask,Value);
  CrMemoryFence();
  if((Read(P,D->VoteRegister)&D->VoteMask)!=Value)return CR_DEVICE_ERROR;
  /* PLL lock may remain asserted for another voter. Restoring our exact vote
   * is the retirement acknowledgement; no global output/PLL stop is inferred. */
  P->Owned=FALSE;P->RestoreVote=FALSE;return CR_SUCCESS;
}

/* Public Android e93f69dc83e69e5139e6a7911041f357b5e2b304
 * clk-alpha-pll.c Lucid5LPE and dispcc-lahaina.c PLL1. Unlike SM8250,
 * CONFIG_U1 is 0x2a9a699c, TEST_U1 is programmed, and MODE update-bypass
 * is not set. Calibration RUN/STANDBY precedes the steady RUN. */
typedef struct {UINT32 Offset,Value;} COLD_SETTING;
STATIC CONST COLD_SETTING mColdSettings[]={
  {4,31},{8,0x44},{0x40,0x4000},{0x18,0x20485699},
  {0x1c,0x2261},{0x20,0x2a9a699c},{0x10,0x805},{0x2c,0x01800000}
};
#define COLD_TRY(X) do{CR_STATUS ColdStatus=(X);if(ColdStatus!=CR_SUCCESS)return ColdStatus;}while(0)
STATIC CR_STATUS ColdAccess(CR_COLD_PLL *P,BOOLEAN Quiet) {
  if(CrMutexCheckLevel()!=CR_SUCCESS || !P || !P->Initialized ||
      P->Profile!=CR_COLD_PLL_SM8350_DISP1 || !P->Base || (P->Base&3U) ||
      P->Size<0x44 || P->Base>(UINTN)-1-P->Size || !P->ReadyCheck)return CR_INVALID_PARAMETER;
  CR_STATUS S=P->ReadyCheck(P->Context,Quiet);
  if(S!=CR_SUCCESS)return CR_ERROR(S)?S:CR_NOT_READY;
  /* A lost direct-mode contract forbids all subsequent destructive IO,
     including cleanup; retain this exact source and suppliers for review. */
  return Quiet && (CrMmioRead32(P->Base+0x0c)&BIT(21))?CR_NOT_READY:CR_SUCCESS;
}
STATIC CR_STATUS ColdRead(CR_COLD_PLL *P,UINT32 O,UINT32 *V,BOOLEAN Quiet) {
  COLD_TRY(ColdAccess(P,Quiet));*V=CrMmioRead32(P->Base+O);return CR_SUCCESS;
}
STATIC CR_STATUS ColdWrite(CR_COLD_PLL *P,UINT32 O,UINT32 V,UINT32 Mask) {
  COLD_TRY(ColdAccess(P,TRUE));
  /* The physical write can commit before its readback fails. Journal first. */
  P->ColdClaimed=TRUE;P->Touched=TRUE;P->Pending=TRUE;
  CrMmioWrite32(P->Base+O,V);CrMemoryFence();
  UINT32 Actual;COLD_TRY(ColdRead(P,O,&Actual,TRUE));
  return ((Actual^V)&Mask)?CR_DEVICE_ERROR:CR_SUCCESS;
}
STATIC CR_STATUS ColdUpdate(CR_COLD_PLL *P,UINT32 O,UINT32 Mask,UINT32 Value) {
  UINT32 V;COLD_TRY(ColdRead(P,O,&V,TRUE));
  return ColdWrite(P,O,(V&~Mask)|(Value&Mask),Mask);
}
STATIC CR_STATUS ColdPoll(CR_COLD_PLL *P,UINT32 O,UINT32 Mask,UINT32 Value) {
  for(UINT32 I=0;I<1500;++I){UINT32 V;COLD_TRY(ColdRead(P,O,&V,TRUE));
    if((V&Mask)==Value)return CR_SUCCESS;
    COLD_TRY(ColdAccess(P,TRUE));cr_sleep(1);
  }
  return CR_TIMEOUT;
}
STATIC CR_STATUS ColdOff(CR_COLD_PLL *P) {
  UINT32 User;COLD_TRY(ColdRead(P,0x0c,&User,TRUE));
  if(User&BIT(21))return CR_NOT_READY; /* never take over an FSM source */
  COLD_TRY(ColdUpdate(P,0,BIT(0),0));
  COLD_TRY(ColdUpdate(P,0x0c,7,0));
  COLD_TRY(ColdWrite(P,0x38,0,~0U));
  COLD_TRY(ColdPoll(P,0,BIT(31)|BIT(0),0));
  COLD_TRY(ColdPoll(P,0x0c,7,0));
  return ColdPoll(P,0x38,~0U,0);
}
STATIC CR_STATUS ColdRun(CR_COLD_PLL *P) {
  COLD_TRY(ColdPoll(P,0,BIT(31),0)); /* reject stale LOCK */
  COLD_TRY(ColdUpdate(P,0,BIT(2),BIT(2)));
  COLD_TRY(ColdWrite(P,0x38,1,~0U));
  COLD_TRY(ColdPoll(P,0,BIT(31),BIT(31)));
  COLD_TRY(ColdUpdate(P,0x0c,7,7));
  return ColdUpdate(P,0,BIT(0),BIT(0));
}
STATIC CR_STATUS ColdConfiguration(CR_COLD_PLL *P,BOOLEAN Quiet) {
  for(UINT32 I=0;I<ARRAY_SIZE(mColdSettings);++I){UINT32 V;
    COLD_TRY(ColdRead(P,mColdSettings[I].Offset,&V,Quiet));
    if(V!=mColdSettings[I].Value)return CR_NOT_READY;
  }
  return CR_SUCCESS;
}
CR_STATUS CrColdPllInit(CR_COLD_PLL *P,UINTN Base,UINT32 Size,UINT32 Profile,
    CR_COLD_PLL_READY Ready,VOID *Context) {
  if(!P || !Ready || !Context)return CR_INVALID_PARAMETER;
  if(P->Initialized || CrColdPllNeedsAccess(P))return CR_ALREADY_STARTED;
  if(Profile!=CR_COLD_PLL_SM8350_DISP1 || !Base || (Base&3U) || Size<0x44 ||
      Base>(UINTN)-1-Size)return CR_UNSUPPORTED;
  *P=(CR_COLD_PLL){.Base=Base,.Size=Size,.Profile=Profile,.ReadyCheck=Ready,
    .Context=Context,.Initialized=TRUE};return CR_SUCCESS;
}
CR_STATUS CrColdPllCheck(CR_COLD_PLL *P) {
  COLD_TRY(ColdAccess(P,FALSE));
  if(!P->Ready || P->Pending || !P->Touched || !P->CalibrationDone)return CR_NOT_READY;
  COLD_TRY(ColdConfiguration(P,FALSE));
  UINT32 Mode,User,Op;COLD_TRY(ColdRead(P,0,&Mode,FALSE));
  COLD_TRY(ColdRead(P,0x0c,&User,FALSE));COLD_TRY(ColdRead(P,0x38,&Op,FALSE));
  return (Mode&(BIT(31)|BIT(2)|BIT(0)))==(BIT(31)|BIT(2)|BIT(0)) &&
    !(User&BIT(21)) && (User&7)==7 && Op==1?CR_SUCCESS:CR_NOT_READY;
}
CR_STATUS CrColdPllPrepare(CR_COLD_PLL *P) {
  COLD_TRY(ColdAccess(P,FALSE));
  if(P->Ready && !P->Pending)return CrColdPllCheck(P);
  if(CrColdPllNeedsAccess(P))return CR_NOT_READY;
  COLD_TRY(ColdAccess(P,TRUE));
  /* Only an exact cold/unconfigured direct source can begin this lifetime.
     A source previously touched by this same owner may be rewritten only
     after its successful Release proved STANDBY and fresh LOCK low. */
  STATIC CONST UINT32 Offsets[]={0,4,8,0x40,0x0c,0x38};UINT32 V[6],After;
  for(UINT32 I=0;I<ARRAY_SIZE(Offsets);++I)COLD_TRY(ColdRead(P,Offsets[I],&V[I],TRUE));
  CrMemoryFence();
  for(UINT32 I=0;I<ARRAY_SIZE(Offsets);++I){
    COLD_TRY(ColdRead(P,Offsets[I],&After,TRUE));if(After!=V[I])return CR_NOT_READY;
  }
  if((V[0]&~(BIT(2)|(P->ColdClaimed?BIT(11):0U))) || (V[4]&BIT(21)) || V[5] ||
      (!P->ColdClaimed && (V[1] || V[2] || V[3] || (V[4]&~7U))))return CR_NOT_READY;
  for(UINT32 I=0;I<ARRAY_SIZE(mColdSettings);++I)
    COLD_TRY(ColdWrite(P,mColdSettings[I].Offset,mColdSettings[I].Value,~0U));
  COLD_TRY(ColdUpdate(P,0,BIT(0),0));COLD_TRY(ColdWrite(P,0x38,0,~0U));
  COLD_TRY(ColdUpdate(P,0,BIT(2),BIT(2)));
  COLD_TRY(ColdRun(P));COLD_TRY(ColdOff(P));P->CalibrationDone=TRUE;
  COLD_TRY(ColdRun(P));COLD_TRY(ColdConfiguration(P,TRUE));
  P->Ready=TRUE;P->Pending=FALSE;
  CR_STATUS S=CrColdPllCheck(P);
  if(S!=CR_SUCCESS){P->Ready=FALSE;P->Pending=TRUE;}
  return S;
}
CR_STATUS CrColdPllRelease(CR_COLD_PLL *P) {
  if(!P || !P->Initialized)return CR_INVALID_PARAMETER;
  if(!CrColdPllNeedsAccess(P))return CR_SUCCESS;
  P->Ready=FALSE;P->Pending=TRUE;
  if(P->Touched)COLD_TRY(ColdOff(P));
  P->Touched=FALSE;P->Pending=FALSE;P->CalibrationDone=FALSE;return CR_SUCCESS;
}
#undef COLD_TRY
