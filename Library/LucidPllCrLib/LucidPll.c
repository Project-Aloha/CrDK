/* Direct Lucid DISPCC PLL1 lifetime for SM8250.
 * Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Ordering/constants: OnePlus Android cad09e061ef6cf689a4a1e54d27562e0f042236a
 * drivers/clk/qcom/{clk-alpha-pll.c,dispcc-kona.c}. Current mainline Lucid
 * differs in CONFIG_CTL_U1 and calibration status location; neither is
 * silently mixed into this exact Android-qualified configuration.
 */
#include <Library/LucidPllCrLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>

#define PLL_OUTCTRL 1U
#define PLL_RESET_N 4U
#define PLL_FSM (1U<<20)
#define PLL_UPDATE_BYPASS (1U<<23)
#define PLL_LOCK (1U<<31)
#define PLL_OUTPUTS 7U
#define REQUIRE_BASE (CR_LUCID_PLL_READY_MAPPING|CR_LUCID_PLL_READY_RETIRED)
#define TRY(E) do { CR_STATUS S=(E); if(S!=CR_SUCCESS)return S; } while(0)

typedef struct { UINT32 Offset,Value; } SETTING;
/* Same nonzero fields programmed by Android clk_lucid_pll_configure. USER
 * output enables are separately masked; unrelated USER fields are preserved. */
STATIC CONST SETTING Configuration[]={
  {0x04,0x1f},{0x08,0x44},{0x40,0x4000},{0x18,0x20485699},
  {0x1c,0x2261},{0x20,0x029a699c},{0x10,0x805}
};

STATIC CR_STATUS Exact(CR_STATUS S)
{ return S==CR_SUCCESS || CR_ERROR(S)?S:CR_NOT_READY; }

STATIC CR_STATUS Valid(CR_LUCID_PLL *P)
{
  if(CrMutexCheckLevel()!=CR_SUCCESS || !P || !P->Initialized || P->Profile!=CR_LUCID_PLL_SM8250_DISP1 ||
     P->Io.Physical!=0xaf01000ULL || P->Io.Bytes<0x44 || !P->Io.Ready ||
     !P->Io.Read32 || !P->Io.Write32 || !P->Io.Fence || !P->Io.DelayUs ||
     !P->Io.MmcxVote) return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}

STATIC CR_STATUS Access(CR_LUCID_PLL *P,BOOLEAN Quiet)
{
  UINT32 Flags=REQUIRE_BASE;
  TRY(Valid(P));
  if(P->OperatingOwned && P->CalibrationCycleDone)
    Flags|=CR_LUCID_PLL_READY_RUN_RAIL;
  else if(P->CalibrationOwned)Flags|=CR_LUCID_PLL_READY_CAL_RAIL;
  else if(P->OperatingOwned)Flags|=CR_LUCID_PLL_READY_RUN_RAIL;
  if(Quiet)Flags|=CR_LUCID_PLL_READY_CHILDREN_QUIET;
  return Exact(P->Io.Ready(P->Io.Context,Flags));
}

STATIC CR_STATUS Read(CR_LUCID_PLL *P,UINT32 Offset,UINT32 *Value,BOOLEAN Quiet)
{
  TRY(Access(P,Quiet));
  if((Offset&3) || Offset>P->Io.Bytes-4)return CR_BAD_BUFFER_SIZE;
  return Exact(P->Io.Read32(P->Io.Context,Offset,Value));
}

STATIC CR_STATUS Fence(CR_LUCID_PLL *P,BOOLEAN Quiet)
{ TRY(Access(P,Quiet));return Exact(P->Io.Fence(P->Io.Context)); }

STATIC CR_STATUS Delay(CR_LUCID_PLL *P,UINT32 Us,BOOLEAN Quiet)
{ TRY(Access(P,Quiet));return Exact(P->Io.DelayUs(P->Io.Context,Us)); }

STATIC CR_STATUS Poll(CR_LUCID_PLL *P,UINT32 Offset,UINT32 Mask,UINT32 Value)
{
  for(UINT32 I=0;I<1500;++I){
    UINT32 Actual;TRY(Read(P,Offset,&Actual,TRUE));
    if((Actual&Mask)==Value)return CR_SUCCESS;
    TRY(Delay(P,1,TRUE));
  }
  return CR_TIMEOUT;
}

STATIC CR_STATUS Write(CR_LUCID_PLL *P,UINT32 Offset,UINT32 Value,UINT32 Mask)
{
  UINT32 Actual;
  TRY(Access(P,TRUE));
  if((Offset&3) || Offset>P->Io.Bytes-4)return CR_BAD_BUFFER_SIZE;
  /* A native write callback may commit even when returning failure. */
  P->Touched=TRUE;
  TRY(Exact(P->Io.Write32(P->Io.Context,Offset,Value)));
  TRY(Fence(P,TRUE));TRY(Read(P,Offset,&Actual,TRUE));
  return ((Actual^Value)&Mask)?CR_DEVICE_ERROR:CR_SUCCESS;
}

STATIC CR_STATUS Update(CR_LUCID_PLL *P,UINT32 Offset,UINT32 Mask,UINT32 Value)
{
  UINT32 Actual;TRY(Read(P,Offset,&Actual,TRUE));
  return Write(P,Offset,(Actual&~Mask)|(Value&Mask),Mask);
}

STATIC CR_STATUS Vote(CR_LUCID_PLL *P,BOOLEAN Calibration,BOOLEAN On)
{
  BOOLEAN *Owned=Calibration?&P->CalibrationOwned:&P->OperatingOwned;
  UINT32 Level=Calibration?CR_LUCID_PLL_MMCX_CAL_LEVEL:CR_LUCID_PLL_MMCX_RUN_LEVEL;
  /* Native votes are serialized and must remain allocated even after a
   * failed withdrawal. Do not zero the ownership output before a retry. */
  TRY(Valid(P));
  /* A failed vote withdrawal can retain its token after the physical vote
   * was removed. Retrying that provider transaction needs base ownership,
   * not the obsolete voltage demand; no PLL MMIO occurs in this helper. */
  TRY(Exact(P->Io.Ready(P->Io.Context,REQUIRE_BASE|CR_LUCID_PLL_READY_CHILDREN_QUIET)));
  CR_STATUS S=Exact(P->Io.MmcxVote(P->Io.Context,Level,On,Owned));
  if(S!=CR_SUCCESS)return S;
  return *Owned==On?CR_SUCCESS:CR_NOT_READY;
}

STATIC CR_STATUS Off(CR_LUCID_PLL *P)
{
  UINT32 Mode,User,Op;
  TRY(Read(P,0,&Mode,TRUE));
  if(Mode&PLL_FSM)return CR_NOT_READY;
  /* Android: gate global output before per-output gates and STANDBY.
   * Keep RESET_N high as its disable path does; no unqualified reset pulse. */
  TRY(Update(P,0,PLL_OUTCTRL,0));
  TRY(Poll(P,0,PLL_OUTCTRL,0));
  TRY(Update(P,0x0c,PLL_OUTPUTS,0));
  TRY(Poll(P,0x0c,PLL_OUTPUTS,0));
  TRY(Write(P,0x38,0,1));
  TRY(Poll(P,0x38,1,0));
  TRY(Update(P,0,PLL_RESET_N,PLL_RESET_N));
  TRY(Fence(P,TRUE));
  TRY(Read(P,0,&Mode,TRUE));TRY(Read(P,0x0c,&User,TRUE));TRY(Read(P,0x38,&Op,TRUE));
  if((Mode&(PLL_OUTCTRL|PLL_FSM)) || (User&PLL_OUTPUTS) || (Op&1))return CR_DEVICE_ERROR;
  return CR_SUCCESS;
}

STATIC CR_STATUS Run(CR_LUCID_PLL *P)
{
  /* Fresh low->high lock observation proves this RUN, not that calibration
   * reran. Neither Android MODE.bit27 nor mainline STATUS.bit27 is a shortcut. */
  TRY(Poll(P,0,PLL_LOCK,0));
  TRY(Write(P,0x38,1,1));
  TRY(Poll(P,0,PLL_LOCK,PLL_LOCK));
  TRY(Update(P,0x0c,PLL_OUTPUTS,PLL_OUTPUTS));
  TRY(Update(P,0,PLL_OUTCTRL,PLL_OUTCTRL));
  return Fence(P,TRUE);
}

STATIC CR_STATUS ConfigurationCheck(CR_LUCID_PLL *P,BOOLEAN Quiet)
{
  for(UINT32 I=0;I<sizeof(Configuration)/sizeof(Configuration[0]);++I){
    UINT32 V;TRY(Read(P,Configuration[I].Offset,&V,Quiet));
    if(V!=Configuration[I].Value)return CR_NOT_READY;
  }
  return CR_SUCCESS;
}

CR_STATUS CrLucidPllInit(CR_LUCID_PLL *P,CONST CR_LUCID_PLL_IO *Io,UINT32 Profile)
{
  if(!P || !Io)return CR_INVALID_PARAMETER;
  if(P->Initialized || CrLucidPllNeedsAccess(P))return CR_ALREADY_STARTED;
  if(Profile!=CR_LUCID_PLL_SM8250_DISP1 || Io->Physical!=0xaf01000ULL ||
     Io->Bytes<0x44)return CR_UNSUPPORTED;
  if(!Io->Ready || !Io->Read32 || !Io->Write32 || !Io->Fence || !Io->DelayUs ||
     !Io->MmcxVote)return CR_INVALID_PARAMETER;
  /* Permit callers to initialize a zeroed object using its own Io member. */
  CR_LUCID_PLL_IO Copy=*Io;
  cr_memset(P,0,sizeof(*P));P->Io=Copy;P->Profile=Profile;P->Initialized=TRUE;
  return CR_SUCCESS;
}

STATIC CR_STATUS Acquire(CR_LUCID_PLL *P)
{
  UINT32 Mode,Op;
  TRY(Vote(P,TRUE,TRUE));
  TRY(Read(P,0,&Mode,TRUE));TRY(Read(P,0x38,&Op,TRUE));
  if(Mode&PLL_FSM)return CR_UNSUPPORTED;
  if((Mode&PLL_OUTCTRL) || (Op&1))return CR_BUSY;
  /* Exclusive retired ownership precedes every programming attempt. */
  for(UINT32 I=0;I<sizeof(Configuration)/sizeof(Configuration[0]);++I)
    TRY(Write(P,Configuration[I].Offset,Configuration[I].Value,0xffffffffU));
  TRY(Update(P,0,PLL_UPDATE_BYPASS,PLL_UPDATE_BYPASS));
  TRY(Update(P,0,PLL_OUTCTRL,0));
  TRY(Write(P,0x38,0,1));
  TRY(Fence(P,TRUE));
  TRY(Update(P,0,PLL_RESET_N,PLL_RESET_N));

  /* Source prepare cycle always executes under the real MMCX>=192 vote.
   * Android's helper hides enable errors; every such error propagates here. */
  TRY(Run(P));TRY(Off(P));P->CalibrationCycleDone=TRUE;
  TRY(Vote(P,FALSE,TRUE));            /* steady vote exists before downgrade */
  TRY(Vote(P,TRUE,FALSE));
  TRY(Run(P));TRY(ConfigurationCheck(P,TRUE));
  P->Ready=TRUE;
  return CR_SUCCESS;
}

CR_STATUS CrLucidPllAcquire(CR_LUCID_PLL *P)
{
  TRY(Valid(P));
  if(P->Ready && !P->RecoveryPending)return CrLucidPllCheck(P);
  if(CrLucidPllNeedsAccess(P))return CR_NOT_READY;
  CR_STATUS S=Acquire(P);
  if(S!=CR_SUCCESS){P->Ready=FALSE;P->RecoveryPending=CrLucidPllNeedsAccess(P);}
  return S;
}

CR_STATUS CrLucidPllCheck(CR_LUCID_PLL *P)
{
  UINT32 Mode,User,Op;
  TRY(Valid(P));
  if(!P->Ready || P->RecoveryPending || !P->Touched || !P->OperatingOwned ||
     P->CalibrationOwned || !P->CalibrationCycleDone)return CR_NOT_READY;
  TRY(ConfigurationCheck(P,FALSE));
  TRY(Read(P,0,&Mode,FALSE));TRY(Read(P,0x0c,&User,FALSE));TRY(Read(P,0x38,&Op,FALSE));
  return (Mode&(PLL_LOCK|PLL_OUTCTRL|PLL_RESET_N|PLL_FSM))==
           (PLL_LOCK|PLL_OUTCTRL|PLL_RESET_N) &&
         (User&PLL_OUTPUTS)==PLL_OUTPUTS && (Op&1)?CR_SUCCESS:CR_NOT_READY;
}

CR_STATUS CrLucidPllRelease(CR_LUCID_PLL *P)
{
  TRY(Valid(P));
  if(!CrLucidPllNeedsAccess(P))return CR_SUCCESS;
  P->Ready=FALSE;P->RecoveryPending=TRUE;
  if(P->Touched){TRY(Off(P));P->Touched=FALSE;P->CalibrationCycleDone=FALSE;}
  if(P->CalibrationOwned)TRY(Vote(P,TRUE,FALSE));
  if(P->OperatingOwned)TRY(Vote(P,FALSE,FALSE));
  P->RecoveryPending=FALSE;
  return CR_SUCCESS;
}

CR_STATUS CrLucidPllUninit(CR_LUCID_PLL *P)
{
  TRY(Valid(P));
  if(CrLucidPllNeedsAccess(P))return CR_BUSY;
  cr_memset(P,0,sizeof(*P));return CR_SUCCESS;
}
