/* Actual LT9611 callbacks; modeled MCU/boot-services boundaries. MIT */
#include "MdssResetFenceHost.h"
#define DEBUG(a) do {} while (0)
#define CR_MDSS_NATIVE_RETAINED_ERROR 1U
#define CR_MDSS_NATIVE_LIVE 2U
#define LT9611_IRQ_POLL_100NS UINT64_C(1000000)
#define TimerRelative 2U
typedef struct {UINT32 Value;} CR_DAL_LT9611_CONFIG;
typedef struct {UINT32 Value;} EFI_LT9611_CR_PROTOCOL;
typedef struct {UINT32 Value;} EFI_MDP_DSI_TIMING;
static struct {BOOLEAN Initialized,Powered,ResetHeld;VOID *Owner;} mPanelOwner;
static BOOLEAN mBootServicesLive,mHandoffFrozen,mHandoffRegistered,mVideoTouched,mIrqPending;
static struct {UINT32 HighPins,Flags,State;} mNativeBridge;
static EFI_EVENT mI2cTestEvent,mIrqTimer;
static CR_DAL_LT9611_CONFIG Configuration;
static int EventStorage[3];
static UINT32 LockCalls,UnlockCalls,DiagnosticCalls,VideoCalls,IrqCalls,CloseCalls,TimerCalls,RetainedCalls;
static BOOLEAN LockAllowed,MissingConfig,FreezeDuringVideo;
static EFI_STATUS IoResult;
static EFI_STATUS Close(EFI_EVENT E){assert(E==mI2cTestEvent);++CloseCalls;return EFI_SUCCESS;}
static EFI_STATUS Timer(EFI_EVENT E,UINT32 T,UINT64 Delay){assert(E==mIrqTimer&&T==TimerRelative&&Delay==LT9611_IRQ_POLL_100NS);++TimerCalls;return EFI_SUCCESS;}
static struct {EFI_STATUS (*CloseEvent)(EFI_EVENT);EFI_STATUS (*SetTimer)(EFI_EVENT,UINT32,UINT64);} Services={Close,Timer},*gBS=&Services;
static CONST CR_DAL_LT9611_CONFIG *CrDalGetLt9611Config(VOID){return MissingConfig?NULL:&Configuration;}
static BOOLEAN Lt9611AcquireMcu(VOID){++LockCalls;return LockAllowed;}
static VOID Lt9611ReleaseMcu(VOID){++UnlockCalls;}
static EFI_STATUS Lt9611TestI2c(CONST CR_DAL_LT9611_CONFIG *C){assert(C==&Configuration);++DiagnosticCalls;return IoResult;}
static EFI_STATUS Lt9611ConfigureVideoInternal(EFI_LT9611_CR_PROTOCOL *P,CONST EFI_MDP_DSI_TIMING *T){
  assert(P&&T);++VideoCalls;mVideoTouched=TRUE;if(FreezeDuringVideo)mHandoffFrozen=TRUE;return IoResult;
}
static EFI_STATUS Lt9611ServiceIrq(VOID){++IrqCalls;return IoResult;}
static VOID Lt9611RetainFailure(EFI_STATUS S){if(S!=EFI_SUCCESS)++RetainedCalls;}
#include "BridgeCallbacks.inc"
static void Init(BOOLEAN Initialized,BOOLEAN Powered,BOOLEAN Held){
  memset(&mPanelOwner,0,sizeof(mPanelOwner));memset(&mNativeBridge,0,sizeof(mNativeBridge));
  mPanelOwner.Initialized=Initialized;mPanelOwner.Powered=Powered;mPanelOwner.ResetHeld=Held;
  mBootServicesLive=TRUE;mHandoffFrozen=FALSE;mHandoffRegistered=FALSE;mVideoTouched=FALSE;mIrqPending=TRUE;
  mI2cTestEvent=&EventStorage[0];mIrqTimer=&EventStorage[1];
  LockCalls=UnlockCalls=DiagnosticCalls=VideoCalls=IrqCalls=CloseCalls=TimerCalls=RetainedCalls=0;
  LockAllowed=TRUE;MissingConfig=FALSE;FreezeDuringVideo=FALSE;IoResult=EFI_SUCCESS;
}
static unsigned Failures,Checks;
#define CHECK(expr) do {++Checks;if(!(expr)){++Failures;fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr);}} while(0)
int main(void){
  EFI_LT9611_CR_PROTOCOL Protocol={0};EFI_MDP_DSI_TIMING Timing={0};
  /* Reset-held, incomplete power, powered-low and legacy routes. */
  for(UINT32 InitFlag=0;InitFlag<2;++InitFlag)for(UINT32 Power=0;Power<2;++Power)for(UINT32 Held=0;Held<2;++Held){
    BOOLEAN Allowed=!InitFlag||(Power&&!Held);
    Init(InitFlag,Power,Held);EFI_EVENT Event=mI2cTestEvent;
    Lt9611TestI2cReadyToBoot(Event,NULL);
    CHECK(DiagnosticCalls==(UINT32)Allowed);CHECK(LockCalls==(UINT32)Allowed);
    CHECK(CloseCalls==1&&mI2cTestEvent==NULL);CHECK(RetainedCalls==0);
    Lt9611TestI2cReadyToBoot(Event,NULL);CHECK(DiagnosticCalls==(UINT32)Allowed&&CloseCalls==1);
    Init(InitFlag,Power,Held);EFI_STATUS S=Lt9611ConfigureVideo(&Protocol,&Timing);
    CHECK(S==(Allowed?EFI_SUCCESS:EFI_NOT_READY));CHECK(VideoCalls==(UINT32)Allowed);CHECK(LockCalls==(UINT32)Allowed);
    Init(InitFlag,Power,Held);Lt9611IrqWorker(mIrqTimer,NULL);
    CHECK(IrqCalls==(UINT32)Allowed);CHECK(mIrqPending==!Allowed);CHECK(TimerCalls==1);
  }
  /* Existing owner exclusion remains stronger than the legacy entry point. */
  Init(TRUE,TRUE,FALSE);mPanelOwner.Owner=&Protocol;
  CHECK(Lt9611ConfigureVideo(&Protocol,&Timing)==EFI_ACCESS_DENIED&&LockCalls==0);
  /* A skipped pending IRQ is serviced after the owner releases reset. */
  Init(TRUE,TRUE,TRUE);Lt9611IrqWorker(mIrqTimer,NULL);mPanelOwner.ResetHeld=FALSE;
  Lt9611IrqWorker(mIrqTimer,NULL);CHECK(IrqCalls==1&&TimerCalls==2&&!mIrqPending);
  /* EBS/stale event guards, lock failures, genuine I/O failure retention. */
  for(unsigned Mode=0;Mode<3;++Mode){
    Init(TRUE,TRUE,FALSE);if(Mode==0)mBootServicesLive=FALSE;if(Mode==1)mHandoffFrozen=TRUE;
    EFI_EVENT E=Mode==2?&EventStorage[2]:mI2cTestEvent;
    Lt9611TestI2cReadyToBoot(E,NULL);Lt9611IrqWorker(Mode==2?E:mIrqTimer,NULL);
    CHECK(!DiagnosticCalls&&!IrqCalls&&!CloseCalls&&!TimerCalls&&!LockCalls);
  }
  Init(TRUE,TRUE,FALSE);LockAllowed=FALSE;Lt9611TestI2cReadyToBoot(mI2cTestEvent,NULL);CHECK(!DiagnosticCalls&&!UnlockCalls);
  Init(TRUE,TRUE,FALSE);MissingConfig=TRUE;Lt9611TestI2cReadyToBoot(mI2cTestEvent,NULL);CHECK(!LockCalls&&!DiagnosticCalls);
  Init(TRUE,TRUE,FALSE);IoResult=EFI_DEVICE_ERROR;Lt9611TestI2cReadyToBoot(mI2cTestEvent,NULL);CHECK(DiagnosticCalls==1&&RetainedCalls==1&&UnlockCalls==1);
  Init(TRUE,TRUE,FALSE);FreezeDuringVideo=TRUE;CHECK(Lt9611ConfigureVideo(&Protocol,&Timing)==EFI_NOT_READY&&UnlockCalls==1);
  Init(TRUE,TRUE,FALSE);IoResult=EFI_DEVICE_ERROR;Lt9611IrqWorker(mIrqTimer,NULL);CHECK(mIrqPending&&RetainedCalls==1&&TimerCalls==1);
  printf("%s: LT9611 reset admission, %u checks, %u failures\n",Failures?"FAIL":"PASS",Checks,Failures);
  return Failures?1:0;
}
