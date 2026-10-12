/* Actual portable core with independently supplied observations, no hardware
 * readiness or native/Windows producer integration claimed. SPDX: MIT */
#include <Library/UsbHandoffCrLib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
static unsigned Checks;
#define CHECK(X) do {__atomic_add_fetch(&Checks,1,__ATOMIC_RELAXED);if(!(X)){fprintf(stderr,"line%d: %s\n",__LINE__,#X);abort();}}while(0)
static CR_USB_NATIVE_HANDOFF *Changing;
static unsigned Barriers,ChangeAt;
VOID CrUsbTestBarrier(VOID){if(Changing&&++Barriers==ChangeAt)Changing->Sequence+=2;}
#ifndef _KERNEL_MODE
VOID EFIAPI MemoryFence(VOID){CrUsbTestBarrier();}
#endif
static CR_USB_CONTROLLER_STOP Stop(UINT32 Role){
  CR_USB_CONTROLLER_STOP R={0};R.Role=Role;R.Id=0x33310000;R.Revision=0x3139302a;
  R.GlobalControl=Role<<12;R.EventBuffers=1;
  if(Role==CR_USBH_HOST)R.HostStatus=1;
  else{R.DeviceStatus=1U<<22;R.MaskedEventBuffers=1;}
  return R;
}
static CR_USB_NATIVE_HANDOFF Native(UINT32 Role){
  CR_USB_NATIVE_HANDOFF R={0};R.Signature=CR_USBH_SIGNATURE;R.Revision=CR_USBH_REVISION;
  R.Length=sizeof(R);R.BootGeneration=19;R.TransferId=29;R.Soc=0x8450;R.Board=1;
  R.Controller=CR_USBH_CONTROLLER;R.ControllerBytes=CR_USBH_CONTROLLER_BYTES;
  R.Qscratch=CR_USBH_QSCRATCH;R.QscratchBytes=CR_USBH_QSCRATCH_BYTES;
  R.ExpectedWriters=R.FrozenWriters=R.DrainedWriters=7;R.State=CR_USBH_RETIRED;
  R.Stop=Stop(Role);SmmuMappingEvidence *E=&R.Revoked;
  E->Base=0x15000000;E->BankBase=0x15041000;E->Bank=1;E->Stream=3;E->Asid=5;
  E->Tables=0x83000000;E->TableBytes=34*4096;E->Iova=0x10000000;E->Bytes=0x04000000;
  E->Smr=0x80000000;E->S2cr=1;E->Sctlr=0x1027;E->Cbar=0x1f300;
  E->Cba2r=1;E->Tcr=0x803500|28;E->Tcr2=0x38012;
  E->Ttbr0=E->Tables|((UINT64)5<<48);E->Ttbr1=(UINT64)5<<48;E->Mair0=0x44;
  CHECK(CrUsbHandoffSeal(&R,2)==CR_SUCCESS);return R;
}
static void Records(void){
  for(unsigned Role=1;Role<=2;++Role){
    CR_USB_NATIVE_HANDOFF R=Native(Role),Copy=R,Scratch,Out,Sentinel;
    CHECK(CrUsbHandoffValidate(&R)==CR_SUCCESS);
    CHECK(CrUsbHandoffCanTakeover(&R,19)==CR_SUCCESS);
    CHECK(CrUsbHandoffCanTakeover(&R,18)==CR_NOT_READY);
    memset(&Out,0xa7,sizeof(Out));Sentinel=Out;
    for(size_t I=0;I<sizeof(R);++I){Copy=R;((UINT8 *)&Copy)[I]^=1;
      CHECK(CrUsbHandoffValidate(&Copy)!=CR_SUCCESS);
      CHECK(CrUsbHandoffSnapshot(&Copy,sizeof(Copy),&Scratch,&Out)!=CR_SUCCESS);
      CHECK(!memcmp(&Sentinel,&Out,sizeof(Out)));
    }
    CHECK(CrUsbHandoffSnapshot(&R,sizeof(R),&Scratch,&Out)==CR_SUCCESS);
    CHECK(!memcmp(&R,&Out,sizeof(R)));Out=Sentinel;
    CHECK(CrUsbHandoffSnapshot(&R,sizeof(R)-1,&Scratch,&Out)==CR_INVALID_PARAMETER);
    CHECK(CrUsbHandoffSnapshot(&R,sizeof(R),&R,&Out)==CR_INVALID_PARAMETER);
    CHECK(CrUsbHandoffSnapshot(&R,sizeof(R),&Scratch,&Scratch)==CR_INVALID_PARAMETER);
    CHECK(!memcmp(&Sentinel,&Out,sizeof(Out)));
    Changing=&R;Barriers=0;ChangeAt=2;
    CHECK(CrUsbHandoffSnapshot(&R,sizeof(R),&Scratch,&Out)==CR_NOT_READY);
    CHECK(!memcmp(&Sentinel,&Out,sizeof(Out)));Changing=NULL;R=Copy=Native(Role);
    CHECK(CrUsbHandoffSeal(&R,0)==CR_INVALID_PARAMETER);CHECK(!memcmp(&R,&Copy,sizeof(R)));
    CHECK(CrUsbHandoffSeal(&R,3)==CR_INVALID_PARAMETER);CHECK(!memcmp(&R,&Copy,sizeof(R)));
    CHECK(CrUsbHandoffSnapshot((void *)(UINTN)(~(UINTN)0-7),sizeof(R),&Scratch,&Out)==CR_INVALID_PARAMETER);
  }
  CR_USB_NATIVE_HANDOFF R=Native(1),T;
  /* Each missing actual proof must refuse retired sealing. Diagnostic output
     remains valid only when marked UNCERTAIN, and never permits takeover. */
#define BAD(STMT) do{T=R;STMT;CR_USB_NATIVE_HANDOFF Old=T;\
  CHECK(CrUsbHandoffSeal(&T,4)!=CR_SUCCESS);CHECK(!memcmp(&T,&Old,sizeof(T)));\
  T.State=CR_USBH_UNCERTAIN;CHECK(CrUsbHandoffSeal(&T,4)==CR_SUCCESS);\
  CHECK(CrUsbHandoffValidate(&T)==CR_SUCCESS);CHECK(CrUsbHandoffCanTakeover(&T,19)!=CR_SUCCESS);}while(0)
  BAD(T.ExpectedWriters=3;T.FrozenWriters=T.DrainedWriters=3);
  BAD(T.DrainedWriters=3);BAD(T.FailedWriters=1);BAD(T.ActiveCalls[0]=1);
  BAD(T.ActiveCalls[1]=1);BAD(T.ActiveCalls[2]=1);BAD(T.FreezeStatus=1);
  BAD(T.HaltStatus=1);BAD(T.InterruptStatus=1);BAD(T.RevocationStatus=1);
  BAD(T.Stop.Role=0);BAD(T.Stop.GlobalControl=2U<<12);BAD(T.Stop.HostCommand=1);
  BAD(T.Stop.HostCommand=4);BAD(T.Stop.HostStatus=0);
  BAD(T.Revoked.Sid=1);BAD(T.Revoked.Smr|=1);BAD(T.Revoked.S2cr=2);
  BAD(T.Revoked.Tables|=1);BAD(T.Revoked.TableBytes-=4096);
  BAD(T.Revoked.BankBase=0x15101000);BAD(T.Revoked.Ttbr0^=4096);
  BAD(T.Revoked.Ttbr1=0);BAD(T.Revoked.Sctlr=0);BAD(T.Revoked.Cbar=0);
  BAD(T.Revoked.Tcr=0);BAD(T.Revoked.Tcr2=0);BAD(T.Revoked.Mair0=0);
  BAD(T.Revoked.Access=1);BAD(T.Revoked.Physical=0x83000000);
  BAD(T.Revoked.Iova+=4096);BAD(T.Revoked.Bytes-=4096);BAD(T.Revoked.Asid=0);
  R=Native(2);BAD(T.Stop.DeviceControl=1U<<31);BAD(T.Stop.DeviceStatus=0);
  BAD(T.Stop.DeviceEvents=1);BAD(T.Stop.DeviceEndpoints=1);BAD(T.Stop.MaskedEventBuffers=0);
#undef BAD
  T=R;T.State=CR_USBH_UNCERTAIN;T.Stop.Role=0;T.ExpectedWriters=T.FrozenWriters=T.DrainedWriters=0;
  T.FreezeStatus=1;CHECK(CrUsbHandoffSeal(&T,4)==CR_SUCCESS);
  CHECK(CrUsbHandoffCanTakeover(&T,19)==CR_NOT_READY); /* no-owner is not bypass */
  T=R;T.Controller++;CHECK(CrUsbHandoffSeal(&T,4)!=CR_SUCCESS);
  T=R;T.ControllerBytes++;CHECK(CrUsbHandoffSeal(&T,4)!=CR_SUCCESS);
  T=R;T.QscratchBytes--;CHECK(CrUsbHandoffSeal(&T,4)!=CR_SUCCESS);
  T=R;T.Reserved0=1;CHECK(CrUsbHandoffSeal(&T,4)!=CR_SUCCESS);
}
static void Configure(CR_USB_ROLE_BOUNDARY *B,void *P,unsigned Role){
  CR_USB_CONTROLLER_STOP S=Stop(Role);
  CHECK(CrUsbRoleBeginConfigure(B,P,Role)==CR_SUCCESS);
  CHECK(!B->Halted&&B->PendingRole==Role);
  CHECK(CrUsbRoleCommitConfigure(B,P,CR_SUCCESS,&S)==CR_SUCCESS);
}
static void Boundaries(void){
  int P,C,F;CR_USB_ROLE_BOUNDARY B={0};CR_USB_ROLE_TOKEN T={0},Other={0},Copy;
  CR_USB_NATIVE_HANDOFF R=Native(1),Changed;CR_USB_CONTROLLER_STOP H=Stop(1),D=Stop(2);
  CHECK(CrUsbRoleInit(&B,&P,19,11)==CR_SUCCESS);
  CHECK(CrUsbRoleBeginConfigure(&B,&P,1)==CR_NOT_READY);
  CHECK(CrUsbRoleImportNative(&B,&F,&R)==CR_INVALID_PARAMETER);
  CHECK(CrUsbRoleImportNative(&B,&P,&R)==CR_SUCCESS);
  CHECK(CrUsbRoleImportNative(&B,&P,&R)==CR_SUCCESS);
  Changed=R;CHECK(CrUsbHandoffSeal(&Changed,4)==CR_SUCCESS);
  CHECK(CrUsbRoleImportNative(&B,&P,&Changed)==CR_ALREADY_STARTED);
  CHECK(!memcmp(&B.NativeRecord,&R,sizeof(R)));
  CHECK(CrUsbRoleBeginConfigure(&B,&P,1)==CR_SUCCESS);
  CHECK(CrUsbRoleCommitConfigure(&B,&P,1,&H)==CR_NOT_READY);
  CHECK(B.PendingRole==1&&!B.Halted);
  CHECK(CrUsbRoleBeginConfigure(&B,&P,2)!=CR_SUCCESS);
  CHECK(CrUsbRoleCleanupComplete(&B,&P,CR_SUCCESS)==CR_BUSY);
  CHECK(CrUsbRoleConfirmHalt(&B,&P,&H)==CR_SUCCESS);
  CHECK(CrUsbRoleCleanupComplete(&B,&P,1)==CR_NOT_READY);
  CHECK(B.PendingRole==1);CHECK(CrUsbRoleCleanupComplete(&B,&P,CR_SUCCESS)==CR_SUCCESS);
  Configure(&B,&P,1);
  CHECK(CrUsbRoleReserveChild(&B,&P,&C,12,1,&T)==CR_INVALID_PARAMETER);
  CHECK(CrUsbRoleReserveChild(&B,&P,&C,11,1,&B.TokenImage)==CR_INVALID_PARAMETER);
  CHECK(CrUsbRoleReserveChild(&B,&P,&P,11,1,&T)==CR_INVALID_PARAMETER);
  CHECK(CrUsbRoleReserveChild(&B,&P,&C,11,1,&T)==CR_SUCCESS);
  CHECK(!B.Halted&&T.Incarnation==1);Copy=T;
  CHECK(CrUsbRoleEnter(&B,&C,&Copy)==CR_INVALID_PARAMETER);
  CHECK(CrUsbRoleEnter(&B,&F,&T)==CR_INVALID_PARAMETER);
  CHECK(CrUsbRoleCanHalt(&B,&P)==CR_BUSY);
  CHECK(CrUsbRoleReserveChild(&B,&P,&F,11,1,&Other)!=CR_SUCCESS);
  CHECK(CrUsbRoleEnter(&B,&C,&T)==CR_SUCCESS);
  CHECK(CrUsbRoleEnter(&B,&C,&T)==CR_SUCCESS);
  CHECK(CrUsbRoleStopChild(&B,&C,&T)==CR_BUSY);
  CHECK(CrUsbRoleEnter(&B,&C,&T)==CR_NOT_READY);
  CHECK(CrUsbRoleCanHalt(&B,&P)==CR_BUSY);
  CHECK(CrUsbRoleLeave(&B,&C,&T)==CR_SUCCESS);
  CHECK(CrUsbRoleLeave(&B,&C,&T)==CR_SUCCESS);
  CHECK(CrUsbRoleStopChild(&B,&C,&T)==CR_SUCCESS);
  CHECK(B.ChildOwner==&C&&B.Token==&T);
  CHECK(CrUsbRoleCanHalt(&B,&P)==CR_SUCCESS);
  CHECK(CrUsbRoleConfirmHalt(&B,&P,&H)==CR_SUCCESS);
  CHECK(CrUsbRoleBeginConfigure(&B,&P,2)==CR_BUSY); /* STOP is not removal */
  CHECK(CrUsbRoleRestartChild(&B,&C,&T,12)==CR_INVALID_PARAMETER);
  CHECK(CrUsbRoleRestartChild(&B,&C,&T,11)==CR_SUCCESS);
  CHECK(!B.Halted&&CrUsbRoleCanHalt(&B,&P)==CR_BUSY);
  CHECK(CrUsbRoleEnter(&B,&C,&T)==CR_SUCCESS);
  CHECK(CrUsbRoleRemoveChild(&B,&C,&T)==CR_BUSY);
  CHECK(CrUsbRoleEnter(&B,&C,&T)==CR_NOT_READY);
  CHECK(CrUsbRoleLeave(&B,&C,&T)==CR_SUCCESS);
  CHECK(CrUsbRoleRemoveChild(&B,&C,&T)==CR_SUCCESS);
  CHECK(!T.Incarnation&&!B.ChildOwner&&!B.Token);
  CHECK(CrUsbRoleEnter(&B,&C,&Copy)==CR_INVALID_PARAMETER);
  CHECK(CrUsbRoleBeginConfigure(&B,&P,2)==CR_NOT_READY); /* no hardware halt yet */
  CHECK(CrUsbRoleConfirmHalt(&B,&P,&H)==CR_SUCCESS);
  CHECK(CrUsbRoleBeginConfigure(&B,&P,2)==CR_SUCCESS);
  CHECK(CrUsbRoleCommitConfigure(&B,&P,CR_TIMEOUT,&D)==CR_TIMEOUT);
  /* Partial configuration can leave either actual old/new role. */
  CHECK(CrUsbRoleConfirmHalt(&B,&P,&H)==CR_SUCCESS);
  CHECK(CrUsbRoleBeginConfigure(&B,&P,1)==CR_BUSY);
  CHECK(CrUsbRoleCleanupComplete(&B,&P,CR_TIMEOUT)==CR_TIMEOUT);
  CHECK(B.PendingRole==2);CHECK(CrUsbRoleCleanupComplete(&B,&P,CR_SUCCESS)==CR_SUCCESS);
  Configure(&B,&P,2);
  CHECK(CrUsbRoleReserveChild(&B,&P,&F,11,2,&T)==CR_SUCCESS);
  CHECK(T.Incarnation==2);CHECK(CrUsbRoleEnter(&B,&F,&T)==CR_SUCCESS);
  CHECK(CrUsbRoleQuarantine(&B,&P)==CR_SUCCESS);
  CHECK(B.Token==&T&&B.ChildOwner==&F&&B.Calls==1);
  CHECK(CrUsbRoleEnter(&B,&F,&T)==CR_NOT_READY);
  CHECK(CrUsbRoleCanHalt(&B,&P)==CR_NOT_READY);
  CHECK(CrUsbRoleConfirmHalt(&B,&P,&D)==CR_NOT_READY);
  CHECK(CrUsbRoleLeave(&B,&F,&T)==CR_SUCCESS);
  CHECK(CrUsbRoleRemoveChild(&B,&F,&T)==CR_NOT_READY);
  CHECK(CrUsbRoleRestartChild(&B,&F,&T,11)==CR_NOT_READY);
  CHECK(CrUsbRoleImportNative(&B,&P,&R)==CR_NOT_READY);
  CHECK(CrUsbRoleInit(&B,&P,19,11)==CR_ALREADY_STARTED);
}
typedef struct {
  pthread_mutex_t Lock;
  pthread_cond_t Condition;
  CR_USB_ROLE_BOUNDARY B;
  CR_USB_ROLE_TOKEN Token;
  int Parent,Child;
  unsigned Entered,Release,Returned;
} THREAD_FIXTURE;
static void *Callback(void *Arg){
  THREAD_FIXTURE *F=Arg;pthread_mutex_lock(&F->Lock);
  CHECK(CrUsbRoleEnter(&F->B,&F->Child,&F->Token)==CR_SUCCESS);
  F->Entered=1;pthread_cond_broadcast(&F->Condition);
  while(!F->Release)pthread_cond_wait(&F->Condition,&F->Lock);
  CHECK(CrUsbRoleLeave(&F->B,&F->Child,&F->Token)==CR_SUCCESS);
  F->Returned=1;pthread_cond_broadcast(&F->Condition);pthread_mutex_unlock(&F->Lock);
  return NULL;
}
static void Threaded(void){
  for(unsigned Kind=0;Kind<3;++Kind){
    THREAD_FIXTURE F={0};pthread_t Thread;CR_USB_NATIVE_HANDOFF N=Native(1);
    pthread_mutex_init(&F.Lock,NULL);pthread_cond_init(&F.Condition,NULL);
    CHECK(CrUsbRoleInit(&F.B,&F.Parent,19,11)==CR_SUCCESS);
    CHECK(CrUsbRoleImportNative(&F.B,&F.Parent,&N)==CR_SUCCESS);Configure(&F.B,&F.Parent,1);
    CHECK(CrUsbRoleReserveChild(&F.B,&F.Parent,&F.Child,11,1,&F.Token)==CR_SUCCESS);
    CHECK(!pthread_create(&Thread,NULL,Callback,&F));pthread_mutex_lock(&F.Lock);
    while(!F.Entered)pthread_cond_wait(&F.Condition,&F.Lock);
    CHECK(F.B.Calls==1&&!F.Returned);
    if(Kind==0)CHECK(CrUsbRoleStopChild(&F.B,&F.Child,&F.Token)==CR_BUSY);
    else if(Kind==1)CHECK(CrUsbRoleRemoveChild(&F.B,&F.Child,&F.Token)==CR_BUSY);
    else CHECK(CrUsbRoleQuarantine(&F.B,&F.Parent)==CR_SUCCESS);
    CHECK(F.B.ChildOwner==&F.Child&&F.B.Token==&F.Token&&F.B.Calls==1);
    CHECK(CrUsbRoleEnter(&F.B,&F.Child,&F.Token)==CR_NOT_READY);
    CHECK(CrUsbRoleCanHalt(&F.B,&F.Parent)!=CR_SUCCESS);
    CHECK(CrUsbRoleBeginConfigure(&F.B,&F.Parent,2)!=CR_SUCCESS);
    F.Release=1;pthread_cond_broadcast(&F.Condition);
    while(!F.Returned)pthread_cond_wait(&F.Condition,&F.Lock);
    CHECK(!F.B.Calls);
    if(Kind==0){
      CHECK(CrUsbRoleStopChild(&F.B,&F.Child,&F.Token)==CR_SUCCESS);
      CHECK(CrUsbRoleCanHalt(&F.B,&F.Parent)==CR_SUCCESS);
      CHECK(CrUsbRoleBeginConfigure(&F.B,&F.Parent,2)!=CR_SUCCESS);
    }else if(Kind==1){
      CHECK(CrUsbRoleRemoveChild(&F.B,&F.Child,&F.Token)==CR_SUCCESS);
      CHECK(!F.B.ChildOwner&&!F.Token.Incarnation);
    }else{
      CHECK(CrUsbRoleRemoveChild(&F.B,&F.Child,&F.Token)==CR_NOT_READY);
      CHECK(F.B.ChildOwner==&F.Child&&F.Token.Incarnation);
    }
    pthread_mutex_unlock(&F.Lock);CHECK(!pthread_join(Thread,NULL));
    pthread_cond_destroy(&F.Condition);pthread_mutex_destroy(&F.Lock);
  }
}
int main(void){Records();Boundaries();Threaded();printf("USBH actual pure core %u checks PASS (wire corruption, partial proof/configure, exact child, rebalance/restart, running callback drain, quarantine)\n",Checks);return 0;}
