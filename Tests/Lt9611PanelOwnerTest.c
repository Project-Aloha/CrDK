/* Actual native panel owner; provider faults and physical level readbacks. MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include "../Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.h"
#include "../Driver/MDSSCrDxe/MdpExtendedNative.h"
static LT9611_PANEL_OWNER P;
static UINT8 Levels[3];
static BOOLEAN Locked,Closed,Busy,Commit,Drop,InputHigh,BadMux,BadOe;
static UINT32 Calls,Fail,Sets,Reads,Prepares,Observed,Sleeps;
static EFI_TPL Tpl;
static EFI_STATUS Failure,LastObserved;
static UINT32 Owner,Foreign;
static UINT16 Pins[]={9,109,107};
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return Tpl;}
VOID *EFIAPI ZeroMem(VOID *B,UINTN N){return memset(B,0,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
UINTN EFIAPI MicroSecondDelay(UINTN Us){assert((Us==1000&&Locked)||(Us==20000&&!Locked));++Sleeps;return Us;}
static EFI_STATUS Op(VOID){return ++Calls==Fail?Failure:EFI_SUCCESS;}
static UINT32 Index(UINT32 Pin){for(UINT32 I=0;I<3;++I)if(Pin==Pins[I])return I;assert(0);return 0;}
static EFI_STATUS EFIAPI Set(EFI_GPIO_CR_PROTOCOL *This,UINT16 Pin,UINT8 V){assert(This==P.Gpio&&Locked&&V<=1);++Sets;
 UINT32 I=Index(Pin);EFI_STATUS S=Op();if(!Drop&&(S==EFI_SUCCESS||Commit))Levels[I]=V;return S;}
static EFI_STATUS EFIAPI Read(EFI_GPIO_CR_PROTOCOL *This,UINT16 Pin,UINT8 *V){assert(This==P.Gpio);++Reads;EFI_STATUS S=Op();if(S==EFI_SUCCESS)*V=InputHigh?1:Levels[Index(Pin)];return S;}
static EFI_STATUS EFIAPI GetConfig(EFI_GPIO_CR_PROTOCOL *This,GpioConfigParams *C){
 assert(This==P.Gpio);++Reads;EFI_STATUS S=Op();if(S==EFI_SUCCESS){
 C->OutputValue=Levels[Index(C->PinNumber)];C->OutputEnable=!BadOe;C->FunctionSel=BadMux?GPIO_FUNC_1:GPIO_FUNC_NORMAL;}return S;}
static EFI_STATUS Ready(VOID *C){assert(C==&P);return Closed?EFI_ACCESS_DENIED:Op();}
static BOOLEAN Lock(VOID *C){assert(C==&P&&!Locked);if(Busy)return FALSE;Locked=TRUE;return TRUE;}
static VOID Unlock(VOID *C){assert(C==&P&&Locked);Locked=FALSE;}
static EFI_STATUS Prepare(VOID *C,CONST EFI_MDP_DSI_TIMING *T){assert(C==&P&&T&&Locked&&P.Powered&&Levels[0]&&Levels[1]&&Levels[2]);++Prepares;return Op();}
static VOID Observe(VOID *C,UINT32 I,BOOLEAN V,EFI_STATUS S){assert(C==&P&&I<3&&V<=1);++Observed;LastObserved=S;}
static EFI_GPIO_CR_PROTOCOL G={.Revision=EFI_GPIO_CR_PROTOCOL_REVISION,.SetIoValue=Set,.ReadIoValue=Read,.GetGpioConfig=GetConfig};
static LT9611_PANEL_OWNER_IO Io={&P,Ready,Lock,Unlock,Prepare,Observe};
static EFI_MDP_DSI_TIMING Timing;
static VOID Fixture(VOID){assert(!Locked);memset(&P,0,sizeof(P));memset(Levels,1,sizeof(Levels));Calls=Fail=Sets=Reads=Prepares=Observed=Sleeps=0;Tpl=TPL_CALLBACK;
 Closed=Busy=Commit=Drop=InputHigh=BadMux=BadOe=FALSE;Failure=EFI_DEVICE_ERROR;LastObserved=EFI_SUCCESS;}
static EFI_STATUS Init(VOID){return Lt9611PanelOwnerInit(&P,&G,&Io);}
static VOID Drain(VOID){Fail=0;Drop=Commit=FALSE;BOOLEAN Owned=TRUE;
 if(!P.Owner)assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
 assert(P.Protocol.Release(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&!Owned&&!P.Owner&&!P.Powered&&!Locked);
 for(UINT32 I=0;I<3;++I)assert(!Levels[I]&&!P.Touched[I]);}
static MDP_EXTENDED_NATIVE Native;
static UINT32 SupplierCalls,SupplierFail;
EFI_STATUS MdpNativeResourcesSuppliersReady(MDP_NATIVE_RESOURCES *R){
 assert(R==&Native.Resources);return ++SupplierCalls==SupplierFail?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS Current(VOID *C){assert(C==&Native);return MdpNativeResourcesSuppliersReady(&Native.Resources);}
static CR_STATUS NativeReady(VOID *C){assert(C==&P);return P.Protocol.Ready(&P.Protocol,&Owner);}
static CR_STATUS NativeAcquire(VOID *C,BOOLEAN *Owned){assert(C==&P);return P.Protocol.Acquire(&P.Protocol,&Owner,Owned);}
static CR_STATUS NativeRelease(VOID *C,BOOLEAN *Owned){assert(C==&P);return P.Protocol.Release(&P.Protocol,&Owner,Owned);}
static CR_STATUS NativeReset(VOID *C,BOOLEAN High){assert(C==&P);return P.Protocol.Reset(&P.Protocol,&Owner,High);}
#include "ExtendedBlank.inc"
static VOID BlankFixture(VOID){
 Fixture();assert(Init()==EFI_SUCCESS);memset(&Native,0,sizeof(Native));
 Native.Config.Panel=(CR_MDSS_PANEL_IO){.Context=&P,.Ready=NativeReady,.Acquire=NativeAcquire,.Release=NativeRelease,.Reset=NativeReset};
 SupplierCalls=SupplierFail=0;
}
static VOID TestEarlyBlank(VOID){
 BlankFixture();SupplierFail=2;
 assert(BlankOutput(&Native)==EFI_DEVICE_ERROR&&Native.PanelLeaseOwned&&Native.OutputPanelAcquired&&P.ResetHeld&&!Levels[2]);
 SupplierFail=0;UINT32 Before=Sets;
 assert(BlankOutput(&Native)==EFI_SUCCESS&&Sets==Before+1&&!Levels[2]);
 BOOLEAN Owned=FALSE;Before=Sets;
 assert(PanelAcquire(&Native,&Owned)==EFI_SUCCESS&&Owned&&Sets==Before&&!Levels[2]);
 assert(PanelRelease(&Native,&Owned)==EFI_SUCCESS&&!Owned&&!Native.OutputPanelAcquired);
 // Failed Acquire may own the panel but must never set the success journal.
 BlankFixture();Calls=0;Fail=4;
 assert(BlankOutput(&Native)==EFI_DEVICE_ERROR&&Native.PanelLeaseOwned&&!Native.OutputPanelAcquired);
 Fail=0;assert(BlankOutput(&Native)==EFI_SUCCESS&&Native.OutputPanelAcquired&&!Levels[2]);
 Drain();
}
int main(void){
 TestEarlyBlank();

 Fixture();assert(Init()==EFI_SUCCESS&&P.Powered&&Sets==0&&Reads==3&&Observed==3);UINT32 InitCalls=Calls;
 BOOLEAN Owned=FALSE;Calls=0;assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned&&P.Owner==&Owner);UINT32 AcquireCalls=Calls;
 Calls=0;Drain();UINT32 ReleaseCalls=Calls;
 Calls=0;Owned=FALSE;assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned&&P.Powered&&Sleeps==1);UINT32 OnCalls=Calls;
 UINT32 Before=Prepares;assert(P.Protocol.Reset(&P.Protocol,&Owner,FALSE)==EFI_SUCCESS&&Levels[2]==0);
 assert(P.ResetHeld&&P.Protocol.Prepare(&P.Protocol,&Owner,&Timing)==EFI_NOT_READY&&Prepares==Before);
 assert(P.Protocol.Reset(&P.Protocol,&Owner,TRUE)==EFI_SUCCESS&&P.Protocol.Prepare(&P.Protocol,&Owner,&Timing)==EFI_SUCCESS&&Prepares==Before+1);
 Drain();
 UINT32 Cases=0;
 for(UINT32 Stage=0;Stage<4;++Stage)for(UINT32 Late=0;Late<2;++Late)for(UINT32 Warning=0;Warning<2;++Warning){
 UINT32 Count=Stage==0?InitCalls:Stage==1?AcquireCalls:Stage==2?ReleaseCalls:OnCalls;
 for(UINT32 F=1;F<=Count;++F){Fixture();if(Stage){assert(Init()==EFI_SUCCESS);Owned=FALSE;if(Stage==2||Stage==3){assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS);if(Stage==3)Drain();}}
 UINT32 PriorWrites=Sets;Calls=0;Fail=F;Commit=Late;Failure=Warning?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;Owned=Stage==2;
 EFI_STATUS S=Stage==0?Init():Stage==2?P.Protocol.Release(&P.Protocol,&Owner,&Owned):P.Protocol.Acquire(&P.Protocol,&Owner,&Owned);
 assert(S!=EFI_SUCCESS&&P.Initialized&&!Locked);
 if(Stage==2){assert(Owned&&P.Owner==&Owner);if(Sets>PriorWrites)assert(!P.Powered);}
 if(Stage==1||Stage==3){if(P.Owner)assert(P.Owner==&Owner&&Owned);}
 Drain();++Cases;}}
 // Shared panel shutdown first asserts reset even after a partial Acquire.
 Fixture();assert(Init()==EFI_SUCCESS);Calls=0;Fail=3;Owned=FALSE;
 assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_DEVICE_ERROR&&Owned&&P.Owner==&Owner&&!P.Powered);
 Fail=0;UINT32 PreReset=Sets;
 assert(P.Protocol.Reset(&P.Protocol,&Owner,TRUE)==EFI_NOT_READY&&Sets==PreReset);
 assert(P.Protocol.Reset(&P.Protocol,&Owner,FALSE)==EFI_SUCCESS&&!Levels[2]&&P.Owner==&Owner&&!P.Powered);
 Drain();
 Fixture();assert(Init()==EFI_SUCCESS);Owned=FALSE;assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
 UINT32 Prior=Sets;Owned=FALSE;assert(P.Protocol.Acquire(&P.Protocol,&Foreign,&Owned)==EFI_ACCESS_DENIED&&!Owned&&Sets==Prior);
 assert(P.Protocol.Release(&P.Protocol,&Foreign,&Owned)==EFI_ACCESS_DENIED&&Sets==Prior&&P.Owner==&Owner);
 Busy=TRUE;Owned=TRUE;assert(P.Protocol.Release(&P.Protocol,&Owner,&Owned)==EFI_NOT_READY&&Owned&&P.Powered&&Sets==Prior);Busy=FALSE;
 Drop=TRUE;assert(P.Protocol.Release(&P.Protocol,&Owner,&Owned)==EFI_DEVICE_ERROR&&Owned&&!P.Powered&&P.Touched[2]&&LastObserved==EFI_DEVICE_ERROR);
 Drop=FALSE;Drain();Owned=FALSE;assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned&&P.Powered);
 Closed=TRUE;Prior=Calls;UINT32 PriorWrites=Sets;assert(P.Protocol.Reset(&P.Protocol,&Owner,FALSE)==EFI_ACCESS_DENIED&&P.Protocol.Prepare(&P.Protocol,&Owner,&Timing)==EFI_ACCESS_DENIED&&
 P.Protocol.Release(&P.Protocol,&Owner,&Owned)==EFI_ACCESS_DENIED&&Owned&&P.Owner==&Owner&&Sets==PriorWrites&&Calls==Prior);
 Closed=FALSE;Tpl=TPL_NOTIFY;assert(P.Protocol.Ready(&P.Protocol,&Owner)==EFI_ACCESS_DENIED&&Sets==PriorWrites);Tpl=TPL_CALLBACK;Drain();
 // A GPIO input sample is independent of its output latch. Verify the actual
 // mux/OE/latch for reset, even when input remains high after a low write.
 Fixture();assert(Init()==EFI_SUCCESS);Owned=FALSE;
 assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
 InputHigh=TRUE;assert(P.Protocol.Reset(&P.Protocol,&Owner,FALSE)==EFI_SUCCESS&&!Levels[2]);
 assert(P.Protocol.Reset(&P.Protocol,&Owner,TRUE)==EFI_SUCCESS&&Levels[2]);
 BadMux=TRUE;assert(P.Protocol.Reset(&P.Protocol,&Owner,FALSE)==EFI_DEVICE_ERROR);BadMux=FALSE;
 BadOe=TRUE;assert(P.Protocol.Reset(&P.Protocol,&Owner,FALSE)==EFI_DEVICE_ERROR);BadOe=FALSE;Drain();
 // A committed reset-high write with a failed latch read is not permission
 // for the MCU worker to run. Only a fully acknowledged retry clears it.
 Fixture();assert(Init()==EFI_SUCCESS);Owned=FALSE;
 assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
 assert(P.Protocol.Reset(&P.Protocol,&Owner,FALSE)==EFI_SUCCESS&&P.ResetHeld);
 Calls=0;Fail=6;Commit=TRUE;
 assert(P.Protocol.Reset(&P.Protocol,&Owner,TRUE)==EFI_DEVICE_ERROR&&Levels[2]&&P.ResetHeld);
 assert(P.Protocol.Prepare(&P.Protocol,&Owner,&Timing)==EFI_NOT_READY);
 Fail=0;assert(P.Protocol.Reset(&P.Protocol,&Owner,TRUE)==EFI_SUCCESS&&!P.ResetHeld);
 Drain();
 // Explicit alternate routing is copied; shared enable outputs stay high.
 Fixture();Pins[0]=47;Pins[1]=49;Pins[2]=48;UINT16 Descriptor[3]={47,49,48};
 assert(Lt9611PanelOwnerInitRoute(&P,&G,&Io,Descriptor,TRUE)==EFI_SUCCESS);
 Descriptor[0]=9;Descriptor[1]=109;Descriptor[2]=107;
 assert(P.Pins[0]==47&&P.Pins[1]==49&&P.Pins[2]==48);
 Owned=FALSE;assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
 Prior=Sets;assert(P.Protocol.Release(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&!Owned);
 assert(Sets==Prior+1&&Levels[0]&&Levels[1]&&!Levels[2]&&!P.Owner&&!P.Powered);
 assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned&&P.Powered);
 // Every release failure retains the owner, then retry completes without
 // ever driving either shared supply low.
 for(UINT32 F=1;F<=5;++F){Calls=0;Fail=F;Prior=Sets;
   assert(P.Protocol.Release(&P.Protocol,&Owner,&Owned)!=EFI_SUCCESS&&Owned&&P.Owner==&Owner);
   assert(Levels[0]&&Levels[1]&&Sets<=Prior+1);Fail=0;
   assert(P.Protocol.Release(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&!Owned&&Levels[0]&&Levels[1]);
   assert(P.Protocol.Acquire(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);++Cases;
 }
 assert(P.Protocol.Release(&P.Protocol,&Owner,&Owned)==EFI_SUCCESS&&!Owned);
 Fixture();Descriptor[0]=47;Descriptor[1]=47;Descriptor[2]=48;
 assert(Lt9611PanelOwnerInitRoute(&P,&G,&Io,Descriptor,TRUE)==EFI_INVALID_PARAMETER&&!P.Initialized&&!Calls);
 printf("LT9611 actual panel owner PASS: %u fault phases; exact three-pin readbacks, retained power/reset, warning/drop writes, MCU serialization and EBS closure\n",Cases);
 return 0;
}
