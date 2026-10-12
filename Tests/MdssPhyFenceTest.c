/* Real SM8450 DSI shutdown with posted/dropped writes and lease revocation.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#endif
#include <Library/MdssPlatformLib.h>
typedef struct {
  UINT32 Reg[0x800/4], Calls, Fault, Writes, Delay, Drop, PostedOffset, PostedValue, ReadsLeft;
  BOOLEAN CommitOnError, PostWrites, Reactivate;
} FIXTURE;
STATIC FIXTURE F;
STATIC CR_STATUS Tick(VOID){return ++F.Calls==F.Fault?CR_DEVICE_ERROR:CR_SUCCESS;}
STATIC CR_STATUS Ready(VOID *C){assert(C==&F);return Tick();}
STATIC CR_STATUS Read(VOID *C,UINT32 O,UINT32 *V){
  assert(C==&F&&!(O&3)&&O<sizeof(F.Reg));CR_STATUS S=Tick();if(S!=CR_SUCCESS)return S;
  if(F.ReadsLeft&&!--F.ReadsLeft)F.Reg[F.PostedOffset/4]=F.PostedValue;
  *V=F.Reg[O/4];return CR_SUCCESS;
}
STATIC CR_STATUS Write(VOID *C,UINT32 O,UINT32 V){
  assert(C==&F&&!(O&3)&&O<sizeof(F.Reg));CR_STATUS S=Tick();++F.Writes;
  if(S!=CR_SUCCESS&&!F.CommitOnError)return S;
  /* No write may enable anything or overwrite unrelated selector bits. */
  assert((V&~F.Reg[O/4])==0);
  if(F.Drop==O+1)return S;
  if(F.PostWrites){F.PostedOffset=O;F.PostedValue=V;F.ReadsLeft=3;}
  else F.Reg[O/4]=V;
  if(O==0x24&&!(V&0xff)&&F.Reactivate)F.Reg[0x14/4]|=0x20;
  return S;
}
STATIC CR_STATUS Fence(VOID *C){assert(C==&F);return Tick();}
STATIC CR_STATUS Delay(VOID *C,UINT32 Us){assert(C==&F);F.Delay+=Us;return Tick();}
STATIC CR_MDSS_DSI_PHY_FENCE_IO Setup(VOID){
  memset(&F,0,sizeof(F));for(UINT32 I=0;I<sizeof(F.Reg)/sizeof(F.Reg[0]);++I)F.Reg[I]=0xa5a5ffff;
  return (CR_MDSS_DSI_PHY_FENCE_IO){&F,0xae94400,0x800,Ready,Read,Write,Fence,Delay};
}
STATIC VOID Stopped(VOID){
  assert(!(F.Reg[0xa4/4]&0x60)&&!(F.Reg[0x14/4]&0x20)&&!(F.Reg[0x3c/4]&1));
  CONST UINT32 O[]={0x1c,0x528,0x24,0x214,0x1ac,0xa0};
  for(UINT32 I=0;I<sizeof(O)/sizeof(O[0]);++I)assert(!(F.Reg[O[I]/4]&0xff));
  assert(F.Reg[0x14/4]==(0xa5a5ffff&~0x20U)); /* keep source/divider selection */
  assert(F.Reg[0x500/4]==0xa5a5ffff&&F.Reg[0x6b0/4]==0xa5a5ffff); /* no PLL tuning/status writes */
}
int main(VOID){
  CR_MDSS_DSI_PHY_FENCE_IO I=Setup();assert(CrMdssPlatformFenceDsiPhy(&I)==CR_SUCCESS);Stopped();
  UINT32 Calls=F.Calls;assert(F.Writes==11&&F.Delay==3);
  /* Every actual callback may fail before/after a committed write. */
  for(UINT32 Commit=0;Commit<2;++Commit)for(UINT32 N=1;N<=Calls;++N){
    I=Setup();F.Fault=N;F.CommitOnError=(BOOLEAN)Commit;
    assert(CrMdssPlatformFenceDsiPhy(&I)==CR_DEVICE_ERROR&&F.Calls==N);
    F.Fault=0;assert(CrMdssPlatformFenceDsiPhy(&I)==CR_SUCCESS);Stopped();
  }
  CONST UINT32 O[]={0xa4,0x14,0x3c,0x1c,0x528,0x24,0x214,0x1ac,0xa0};
  for(UINT32 N=0;N<sizeof(O)/sizeof(O[0]);++N){
    I=Setup();F.Drop=O[N]+1;assert(CrMdssPlatformFenceDsiPhy(&I)==CR_TIMEOUT);
    assert(F.Delay>=1000&&F.Delay<=1003);F.Drop=0;
    assert(CrMdssPlatformFenceDsiPhy(&I)==CR_SUCCESS);Stopped();
  }
  I=Setup();F.PostWrites=TRUE;assert(CrMdssPlatformFenceDsiPhy(&I)==CR_SUCCESS&&F.Delay>3);Stopped();
  I=Setup();F.Reactivate=TRUE;assert(CrMdssPlatformFenceDsiPhy(&I)==CR_TIMEOUT);F.Reactivate=FALSE;
  assert(CrMdssPlatformFenceDsiPhy(&I)==CR_SUCCESS);Stopped();
  I=Setup();I.PhyBytes=0x7ff;assert(CrMdssPlatformFenceDsiPhy(&I)==CR_BAD_BUFFER_SIZE&&!F.Calls);
  I=Setup();I.PhyPhysical=0xae96400;assert(CrMdssPlatformFenceDsiPhy(&I)==CR_UNSUPPORTED&&!F.Calls);
  I=Setup();I.Ready=NULL;assert(CrMdssPlatformFenceDsiPhy(&I)==CR_INVALID_PARAMETER&&!F.Calls);
  printf("PASS actual DSI PHY fence: %u callback failures x2, nine stuck controls, posted writes, late reactivation, retry and pure preflight\n",Calls);
  return 0;
}
