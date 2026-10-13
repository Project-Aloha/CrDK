/* Fixed PLL qualification and retained local vote. SPDX-License-Identifier: MIT */
#include <Library/FixedPllVoteLib.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_time.h>
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
