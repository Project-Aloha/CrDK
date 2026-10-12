/* Actual ARC table/voter adversarial lifecycle. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/RpmhArcVoteLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *B,UINTN N,UINT8 V) {return memset(B,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) {return memcpy(D,S,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) {return strcmp(A,B);}
#else
static KIRQL Irql;
KIRQL KeGetCurrentIrql(VOID) {return Irql;}
#endif
static UINT32 Calls,FailCall,Hardware[2],Log[256];
static CR_STATUS Write(VOID *Context,UINT32 Address,UINT32 Corner) {
  assert(Context==Hardware && (Address==0x30004 || Address==0x30008));
#ifdef _KERNEL_MODE
  assert(Irql==PASSIVE_LEVEL);
#endif
  assert(Calls<256);Log[Calls++]=Corner;
  Hardware[(Address-0x30004)/4]=Corner; /* Commit before transport failure. */
  return Calls==FailCall ? CR_DEVICE_ERROR : CR_SUCCESS;
}
static CR_RPMH_ARC_IO Io={Hardware,Write};
static const UINT8 Aux[]={0,0,16,0,64,0,64,0,128,0,0,1,0,0};
static void Qualification(void) {
  CR_RPMH_ARC_RESOURCE R={0},Old;
  assert(CrRpmhArcQualify(&R,"cx.lvl",0x30004,CrRpmhArcPower,Aux,sizeof(Aux))==CR_SUCCESS);
  assert(R.LevelCount==6 && R.Levels[2]==64 && R.Levels[3]==64 && R.Levels[5]==256);
  Old=R;
  const UINT8 Descending[]={0,0,64,0,32,0},InteriorZero[]={0,0,32,0,0,0,64,0},NoOff[]={1,0,32,0};
  assert(CrRpmhArcQualify(&R,"bad",0x30004,CrRpmhArcPower,Descending,sizeof(Descending))==CR_COMPROMISED_DATA);
  assert(!memcmp(&R,&Old,sizeof(R)));
  assert(CrRpmhArcQualify(&R,"bad",0x30004,CrRpmhArcPower,InteriorZero,sizeof(InteriorZero))==CR_COMPROMISED_DATA);
  assert(CrRpmhArcQualify(&R,"bad",0x30004,CrRpmhArcPower,NoOff,sizeof(NoOff))==CR_COMPROMISED_DATA);
  assert(CrRpmhArcQualify(&R,"bad",0x40004,CrRpmhArcPower,Aux,sizeof(Aux))==CR_INVALID_PARAMETER);
  assert(CrRpmhArcQualify(&R,"bad",0x30005,CrRpmhArcPower,Aux,sizeof(Aux))==CR_INVALID_PARAMETER);
  assert(CrRpmhArcQualify(&R,"123456789",0x30004,CrRpmhArcPower,Aux,sizeof(Aux))==CR_INVALID_PARAMETER);
  assert(CrRpmhArcQualify(&R,"bad",0x30004,CrRpmhArcPower,Aux,sizeof(Aux)-1)==CR_INVALID_PARAMETER);
  assert(CrRpmhArcQualify(&R,"xo.lvl",0x30008,CrRpmhArcXo,NULL,0)==CR_SUCCESS && R.LevelCount==0);
}
static void Lifecycle(void) {
  CR_RPMH_ARC_RESOURCE R[2];CR_RPMH_ARC_ENTRY E[3];CR_RPMH_ARC_VOTER V={0};UINT32 Owners[4],Corner;
  CR_RPMH_ARC_VALUE Value,High={5,TRUE},Low={2,TRUE};BOOLEAN Pending;
  Calls=FailCall=0;Hardware[0]=9;Hardware[1]=3;
  assert(CrRpmhArcQualify(R,"cx.lvl",0x30004,CrRpmhArcPower,Aux,sizeof(Aux))==CR_SUCCESS);
  assert(CrRpmhArcQualify(R+1,"xo.lvl",0x30008,CrRpmhArcXo,NULL,0)==CR_SUCCESS);
  assert(CrRpmhArcVotesInit(&V,&Io,R,2,E,3)==CR_SUCCESS && Calls==0);
  assert(CrRpmhArcVotesInit(&V,&Io,R,2,E,3)==CR_ALREADY_STARTED);
  assert(CrRpmhArcResolveLevel(&V,"cx.lvl",64,&Corner)==CR_SUCCESS && Corner==2);
  assert(CrRpmhArcResolveLevel(&V,"cx.lvl",65,&Corner)==CR_SUCCESS && Corner==4);
  assert(CrRpmhArcResolveLevel(&V,"cx.lvl",257,&Corner)==CR_UNSUPPORTED);
  assert(CrRpmhArcResolveLevel(&V,"xo.lvl",128,&Corner)==CR_UNSUPPORTED);
  assert(CrRpmhArcSet(&V,Owners,"cx.lvl",&High)==CR_NOT_FOUND && Calls==0);
  assert(CrRpmhArcReserve(&V,Owners,"cx.lvl")==CR_SUCCESS);
  assert(CrRpmhArcGet(&V,Owners,"cx.lvl",&Value,&Pending)==CR_SUCCESS && Pending && !Value.Present);
  assert(CrRpmhArcReleaseOwner(&V,Owners)==CR_SUCCESS && Calls==0 && Hardware[0]==9);
  assert(CrRpmhArcReserve(&V,Owners,"cx.lvl")==CR_SUCCESS);
  assert(CrRpmhArcReserve(&V,Owners+1,"cx.lvl")==CR_SUCCESS);
  assert(CrRpmhArcReserve(&V,Owners+2,"xo.lvl")==CR_SUCCESS);
  assert(CrRpmhArcReserve(&V,Owners+3,"cx.lvl")==CR_OUT_OF_RESOURCES && Calls==0);
  assert(CrRpmhArcSet(&V,Owners,"cx.lvl",&High)==CR_SUCCESS && Calls==1 && Log[0]==5);
  assert(CrRpmhArcSet(&V,Owners+1,"cx.lvl",&Low)==CR_SUCCESS && Calls==1);
  assert(CrRpmhArcReleaseOwner(&V,Owners)==CR_SUCCESS && Hardware[0]==2 && Calls==2);
  assert(CrRpmhArcReserve(&V,Owners,"cx.lvl")==CR_SUCCESS);
  FailCall=Calls+1;
  assert(CrRpmhArcSet(&V,Owners,"cx.lvl",&High)==CR_DEVICE_ERROR && Hardware[0]==5);
  assert(CrRpmhArcGet(&V,Owners,"cx.lvl",&Value,&Pending)==CR_SUCCESS && Pending && !Value.Present);
  assert(CrRpmhArcGet(&V,Owners+1,"cx.lvl",&Value,&Pending)==CR_SUCCESS && Pending && Value.Corner==2);
  /* An absent foreign slot may retire without repairing this failed writer. */
  UINT32 Before=Calls;
  assert(CrRpmhArcReleaseOwner(&V,Owners+2)==CR_SUCCESS && Calls==Before && Hardware[1]==3);
  assert(CrRpmhArcVotesInit(&V,&Io,R,2,E,3)==CR_ALREADY_STARTED && E[0].Owner!=NULL);
  FailCall=Calls+1;
  assert(CrRpmhArcReleaseOwner(&V,Owners)==CR_DEVICE_ERROR && Hardware[0]==2);
  assert(CrRpmhArcUninit(&V)==CR_BUSY);
  FailCall=0;Before=Calls;
  assert(CrRpmhArcReleaseOwner(&V,Owners)==CR_SUCCESS && Calls==Before+1 && Hardware[0]==2);
  assert(CrRpmhArcGet(&V,Owners+1,"cx.lvl",&Value,&Pending)==CR_SUCCESS && !Pending && Value.Corner==2);
  assert(CrRpmhArcSet(&V,Owners+1,"cx.lvl",&(CR_RPMH_ARC_VALUE){2,FALSE})==CR_INVALID_PARAMETER);
  assert(CrRpmhArcReserve(&V,Owners+2,"xo.lvl")==CR_SUCCESS);
  assert(CrRpmhArcSet(&V,Owners+2,"xo.lvl",&(CR_RPMH_ARC_VALUE){1,TRUE})==CR_INVALID_PARAMETER);
  assert(CrRpmhArcSet(&V,Owners+2,"xo.lvl",&(CR_RPMH_ARC_VALUE){3,TRUE})==CR_SUCCESS);
#ifdef _KERNEL_MODE
  Irql=1;Before=Calls;
  assert(CrRpmhArcSet(&V,Owners+1,"cx.lvl",&(CR_RPMH_ARC_VALUE){0})==CR_INVALID_PARAMETER && Calls==Before);
  assert(CrRpmhArcReserve(&V,Owners+3,"cx.lvl")==CR_INVALID_PARAMETER);
  Irql=PASSIVE_LEVEL;
#endif
  FailCall=Calls+1;
  assert(CrRpmhArcDrain(&V)==CR_DEVICE_ERROR && CrRpmhArcUninit(&V)==CR_BUSY);
  FailCall=0;assert(CrRpmhArcDrain(&V)==CR_SUCCESS && Hardware[0]==0 && Hardware[1]==0);
  assert(CrRpmhArcUninit(&V)==CR_SUCCESS);
  /* XO virtual levels only qualify if the actual aux index proves vote3. */
  const UINT8 XoAux[]={0,0,32,0,64,0,128,0};
  assert(CrRpmhArcQualify(R+1,"xo.lvl",0x30008,CrRpmhArcXo,XoAux,sizeof(XoAux))==CR_SUCCESS);
  assert(CrRpmhArcVotesInit(&V,&Io,R,2,E,3)==CR_SUCCESS);
  assert(CrRpmhArcResolveLevel(&V,"xo.lvl",128,&Corner)==CR_SUCCESS && Corner==3);
  assert(CrRpmhArcResolveLevel(&V,"xo.lvl",64,&Corner)==CR_UNSUPPORTED);
  assert(CrRpmhArcUninit(&V)==CR_SUCCESS);
  R[1].Address=R[0].Address;
  assert(CrRpmhArcVotesInit(&V,&Io,R,2,E,3)==CR_INVALID_PARAMETER && !V.Initialized);
  assert(CrRpmhArcVotesInit(&V,&Io,R,1,(CR_RPMH_ARC_ENTRY *)(void *)R,3)==CR_INVALID_PARAMETER);
}
static void Baseline(void) {
  CR_RPMH_ARC_RESOURCE R[2];CR_RPMH_ARC_ENTRY E[4];CR_RPMH_ARC_VOTER V={0};UINT32 Owners[3],Corners[2]={4,3};
  CR_RPMH_ARC_VALUE Value;BOOLEAN Pending;
  Calls=FailCall=0;Hardware[0]=4;Hardware[1]=3;
  assert(CrRpmhArcQualify(R,"cx.lvl",0x30004,CrRpmhArcPower,Aux,sizeof(Aux))==CR_SUCCESS);
  assert(CrRpmhArcQualify(R+1,"xo.lvl",0x30008,CrRpmhArcXo,NULL,0)==CR_SUCCESS);
  assert(CrRpmhArcVotesInit(&V,&Io,R,2,E,4)==CR_SUCCESS);
  Corners[1]=2;
  assert(CrRpmhArcImportBaseline(&V,Owners,Corners,2)==CR_INVALID_PARAMETER && Calls==0 && E[0].Owner==NULL && !V.States[0].Known);
  Corners[1]=3;
  assert(CrRpmhArcImportBaseline(&V,Owners,Corners,1)==CR_INVALID_PARAMETER);
  assert(CrRpmhArcReserve(&V,Owners+1,"cx.lvl")==CR_SUCCESS);
  assert(CrRpmhArcImportBaseline(&V,Owners,Corners,2)==CR_BUSY && Calls==0 && !V.States[0].Known);
  assert(CrRpmhArcReleaseOwner(&V,Owners+1)==CR_SUCCESS);
  assert(CrRpmhArcImportBaseline(&V,Owners,Corners,2)==CR_SUCCESS && Calls==0);
  assert(CrRpmhArcImportBaseline(&V,Owners,Corners,2)==CR_BUSY);
  assert(CrRpmhArcGet(&V,Owners,"cx.lvl",&Value,&Pending)==CR_SUCCESS && !Pending && Value.Corner==4 && Value.Present);
  assert(CrRpmhArcSet(&V,Owners,"cx.lvl",&(CR_RPMH_ARC_VALUE){0})==CR_BUSY);
  assert(CrRpmhArcReleaseOwner(&V,Owners)==CR_BUSY && Calls==0);
  assert(CrRpmhArcReserve(&V,Owners+1,"cx.lvl")==CR_SUCCESS);
  assert(CrRpmhArcSet(&V,Owners+1,"cx.lvl",&(CR_RPMH_ARC_VALUE){2,TRUE})==CR_SUCCESS && Calls==0);
  FailCall=1;
  assert(CrRpmhArcSet(&V,Owners+1,"cx.lvl",&(CR_RPMH_ARC_VALUE){5,TRUE})==CR_DEVICE_ERROR && Hardware[0]==5);
  assert(CrRpmhArcGet(&V,Owners+1,"cx.lvl",&Value,&Pending)==CR_SUCCESS && Pending && Value.Corner==2);
  FailCall=0;
  assert(CrRpmhArcSet(&V,Owners+1,"cx.lvl",&(CR_RPMH_ARC_VALUE){2,TRUE})==CR_SUCCESS && Calls==2 && Log[1]==4 && Hardware[0]==4);
  assert(CrRpmhArcReleaseOwner(&V,Owners+1)==CR_SUCCESS && Hardware[0]==4);
  assert(CrRpmhArcDrain(&V)==CR_BUSY && CrRpmhArcUninit(&V)==CR_BUSY && Calls==2 && Hardware[1]==3);
  CR_RPMH_ARC_VOTER Small={0};CR_RPMH_ARC_ENTRY One[1];
  assert(CrRpmhArcVotesInit(&Small,&Io,R,2,One,1)==CR_SUCCESS);
  assert(CrRpmhArcImportBaseline(&Small,Owners,Corners,2)==CR_OUT_OF_RESOURCES && One[0].Owner==NULL && !Small.States[0].Known);
  assert(CrRpmhArcUninit(&Small)==CR_SUCCESS);
}
int main(void) {Qualification();Lifecycle();Baseline();puts("PASS actual ARC/XO tables, pure reservations, foreign owners and retained uncertain cleanup and atomic inherited baseline");return 0;}
