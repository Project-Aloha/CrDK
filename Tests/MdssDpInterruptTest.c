/* Actual packed DP ledger + DPU core/source qualifier; independent model. */
#include <Library/MdssDpInterruptCrLib.h>
#include <Library/MdssIrqSourcesCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
 UINT32 Dpu[0x8f000/4],Dsi0[256],Dsi1[256],Ahb[128],Aux[128];
 UINT32 Calls,Writes,Fail,Drop,ParentExtra,LastDpAck;
 CR_STATUS Error;
} HW;
static HW H;
static UINT32 Checks;
#define CHECK(X) do { ++Checks; assert(X); } while(0)
static CR_STATUS Step(void){++H.Calls;return H.Calls==H.Fail?H.Error:CR_SUCCESS;}
static UINT32 *Reg(CR_MDSS_IRQ_WINDOW W,UINT32 O){
 CHECK(!(O&3));switch(W){
 case CrMdssIrqDpu:CHECK(O<sizeof(H.Dpu));return &H.Dpu[O/4];
 case CrMdssIrqDsi0:CHECK(O<sizeof(H.Dsi0));return &H.Dsi0[O/4];
 case CrMdssIrqDsi1:CHECK(O<sizeof(H.Dsi1));return &H.Dsi1[O/4];
 case CrMdssIrqDpAhb:CHECK(O<sizeof(H.Ahb));return &H.Ahb[O/4];
 case CrMdssIrqDpAux:CHECK(O<sizeof(H.Aux));return &H.Aux[O/4];
 default:assert(0);return NULL;}
}
static CR_STATUS Read(VOID *P,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 *V){
 CHECK(P==&H);CR_STATUS S=Step();if(S!=CR_SUCCESS)return S;
 if(W==CrMdssIrqParent){CHECK(O==0x10);*V=H.ParentExtra;
  if(H.Dpu[0x10/4]&H.Dpu[0x14/4])*V|=1;
  if((H.Ahb[0x20/4]&CR_MDSS_DP_AUX_IRQ_STATUS)&(H.Ahb[0x20/4]>>2))*V|=CR_MDSS_IRQ_PARENT_DP;
 }else *V=*Reg(W,O);return CR_SUCCESS;
}
static CR_STATUS Write(VOID *P,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 V){
 CHECK(P==&H);CR_STATUS S=Step();++H.Writes;
 if(S!=CR_SUCCESS)return S;if(H.Writes==H.Drop)return CR_SUCCESS;
 if(W==CrMdssIrqDpu){CHECK(O==0x10||O==0x18);if(O==0x10)H.Dpu[O/4]=V;else H.Dpu[0x14/4]&=~V;}
 else{CHECK(W==CrMdssIrqDpAhb&&O==0x20);
  H.LastDpAck=(V>>1)&CR_MDSS_DP_AUX_IRQ_STATUS;
  H.Ahb[O/4]=(H.Ahb[O/4]&CR_MDSS_DP_AUX_IRQ_STATUS&~H.LastDpAck)|(V&CR_MDSS_DP_AUX_IRQ_MASK)|(V&(1U<<2));}
 return CR_SUCCESS;
}
static CR_STATUS Barrier(VOID *P){CHECK(P==&H);return Step();}
static CR_MDSS_IRQ_IO Io(void){return (CR_MDSS_IRQ_IO){&H,Read,Write,Barrier};}
static void Init(CR_MDSS_IRQ_CORE *C,CR_MDSS_DP_IRQ_CORE *D){
 memset(&H,0,sizeof(H));memset(C,0,sizeof(*C));memset(D,0,sizeof(*D));H.Error=CR_DEVICE_ERROR;
 CR_MDSS_IRQ_IO I=Io();CHECK(CrMdssIrqInit(C,&I,99)==CR_SUCCESS);CHECK(CrMdssDpIrqInit(D,&I,99)==CR_SUCCESS);
}
static void Register(CR_MDSS_DP_IRQ_CORE *D,VOID *O,CR_MDSS_DP_IRQ_TOKEN *T){
 memset(T,0,sizeof(*T));BOOLEAN Owned=FALSE;CHECK(CrMdssDpIrqRegister(D,O,T,&Owned)==CR_SUCCESS&&Owned);
}
static void Close(CR_MDSS_DP_IRQ_CORE *D,VOID *O,CR_MDSS_DP_IRQ_TOKEN *T,VOID *R){
 CHECK(CrMdssDpIrqDisarm(D,O,T)==CR_SUCCESS);CHECK(CrMdssDpIrqDiscard(D,O,T)==CR_SUCCESS);
 if(R)CHECK(CrMdssDpIrqRetireRequest(D,O,T,R)==CR_SUCCESS);
 CHECK(CrMdssDpIrqUnregister(D,O,T)==CR_SUCCESS);CHECK(CrMdssDpIrqUninit(D)==CR_SUCCESS);
}
static void Nominal(void){
 CR_MDSS_IRQ_CORE C;CR_MDSS_DP_IRQ_CORE D;CR_MDSS_DP_IRQ_TOKEN T;int O,R,J;
 CR_MDSS_IRQ_TOKEN V={0},W={0};CR_MDSS_IRQ_EVENT E;CR_MDSS_DP_IRQ_EVENT F;UINT64 G,A;BOOLEAN B,Q,Owned;
 Init(&C,&D);Register(&D,&O,&T);
 CHECK(CrMdssIrqRegister(&C,&O,CR_MDSS_IRQ_DSI0_VBLANK,&V,&Owned)==CR_SUCCESS&&Owned);
 CHECK(CrMdssIrqRegister(&C,&O,CR_MDSS_IRQ_WB2,&W,&Owned)==CR_SUCCESS&&Owned);
 CHECK(CrMdssIrqArm(&C,&O,&V,NULL,&A)==CR_SUCCESS);CHECK(CrMdssIrqArm(&C,&O,&W,&J,&A)==CR_SUCCESS);
 CHECK(CrMdssDpIrqArm(&D,&O,&T,&R,&G)==CR_SUCCESS&&G);
 H.Dpu[0x14/4]=CR_MDSS_IRQ_SUPPORTED;H.Ahb[0x20/4]|=1U<<3;
 CHECK(CrMdssIrqCaptureWithDp(&C,&D,&B,&Q)==CR_SUCCESS&&B&&Q&&H.LastDpAck==(1U<<3));
 CHECK(!H.Dpu[0x14/4]&&!H.Ahb[0x20/4]);CHECK(CrMdssDpIrqTake(&D,&O,&T,&F)==CR_SUCCESS&&F.Generation==G&&F.Request==&R);
 CR_MDSS_DP_IRQ_EVENT Saved=F;CHECK(CrMdssDpIrqTake(&D,&O,&T,&F)==CR_NOT_FOUND&&!memcmp(&F,&Saved,sizeof(F)));
 CHECK(CrMdssIrqTake(&C,&O,&V,&E)==CR_SUCCESS);CHECK(CrMdssIrqTake(&C,&O,&W,&E)==CR_SUCCESS);
 CHECK(CrMdssIrqDisarm(&C,&O,&V)==CR_SUCCESS);CHECK(CrMdssIrqDiscard(&C,&O,&V)==CR_SUCCESS);CHECK(CrMdssIrqUnregister(&C,&O,&V)==CR_SUCCESS);
 CHECK(CrMdssIrqDisarm(&C,&O,&W)==CR_SUCCESS);CHECK(CrMdssIrqDiscard(&C,&O,&W)==CR_SUCCESS);CHECK(CrMdssIrqRetireJob(&C,&O,&W,&J)==CR_SUCCESS);CHECK(CrMdssIrqUnregister(&C,&O,&W)==CR_SUCCESS);
 Close(&D,&O,&T,&R);CHECK(CrMdssIrqUninit(&C)==CR_SUCCESS);
}
static void Identity(void){
 CR_MDSS_IRQ_CORE C;CR_MDSS_DP_IRQ_CORE D;CR_MDSS_DP_IRQ_TOKEN T,Copy;int O,F,R;UINT64 G=55;BOOLEAN Owned;
 Init(&C,&D);Register(&D,&O,&T);Copy=T;UINT32 N=H.Calls;
 CHECK(CrMdssDpIrqArm(&D,&O,&Copy,&R,&G)==CR_INVALID_PARAMETER&&G==55&&H.Calls==N);
 CHECK(CrMdssDpIrqRegister(&D,&F,&T,&Owned)==CR_BUSY&&!Owned);
 CHECK(CrMdssDpIrqArm(&D,&O,&T,&R,&G)==CR_SUCCESS);CHECK(CrMdssDpIrqUnregister(&D,&O,&T)==CR_BUSY);
 CHECK(CrMdssDpIrqRetireRequest(&D,&O,&T,&F)==CR_INVALID_PARAMETER);
 Close(&D,&O,&T,&R);
}
static UINT32 Failure(UINT32 At,CR_STATUS Error,UINT32 Drop){
 CR_MDSS_IRQ_CORE C;CR_MDSS_DP_IRQ_CORE D;CR_MDSS_DP_IRQ_TOKEN T;int O,R;UINT64 G=888;BOOLEAN B=7,Q=8;
 Init(&C,&D);Register(&D,&O,&T);H.Fail=At;H.Error=Error;H.Drop=Drop;H.Calls=H.Writes=0;
 CR_STATUS S=CrMdssDpIrqArm(&D,&O,&T,&R,&G);
 if(S==CR_SUCCESS){H.Ahb[0x20/4]|=1U<<3;S=CrMdssIrqCaptureWithDp(&C,&D,&B,&Q);}
 UINT32 Count=H.Calls;
 if(S!=CR_SUCCESS){CHECK(D.Identity==&T&&T.Provider==&D);CHECK(B==7&&Q==8);CHECK(D.Faulted||D.Closed);CHECK(CrMdssDpIrqUninit(&D)==CR_BUSY);}
 else CHECK(B&&Q&&D.Pending);
 return Count;
}
static void Foreign(void){
 CR_MDSS_IRQ_CORE C;CR_MDSS_DP_IRQ_CORE D;CR_MDSS_DP_IRQ_TOKEN T;int O,R;UINT64 G;BOOLEAN B=7,Q=8;
 CR_MDSS_IRQ_SOURCE_OBSERVATION Obs;
 Init(&C,&D);Register(&D,&O,&T);CHECK(CrMdssDpIrqArm(&D,&O,&T,&R,&G)==CR_SUCCESS);
 CHECK(CrMdssIrqSourcesQuiet(&C.Io,0,&Obs)==CR_NOT_READY); /* original policy retained */
 H.Ahb[0x20/4]|=1U<<3;H.Ahb[0x24/4]=1U<<17;UINT32 N=H.Writes;
 CHECK(CrMdssIrqCaptureWithDp(&C,&D,&B,&Q)==CR_NOT_READY&&H.Writes==N&&B==7&&Q==8&&D.Closed&&C.Faulted);
 Init(&C,&D);Register(&D,&O,&T);CHECK(CrMdssDpIrqArm(&D,&O,&T,&R,&G)==CR_SUCCESS);
 H.Ahb[0x20/4]|=(1U<<3)|(1U<<2);N=H.Writes;
 CHECK(CrMdssIrqCaptureWithDp(&C,&D,&B,&Q)==CR_NOT_READY&&H.Writes==N);
 Init(&C,&D);H.ParentExtra=CR_MDSS_IRQ_PARENT_DP;N=H.Writes;
 CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_NOT_READY&&H.Writes==N); /* no unowned bypass */
}
static void Aliases(void){
 CR_MDSS_IRQ_CORE C;CR_MDSS_DP_IRQ_CORE D;CR_MDSS_DP_IRQ_TOKEN T;int O,R;UINT64 G=77;BOOLEAN B=5,Q=6;
 Init(&C,&D);Register(&D,&O,&T);UINT32 N=H.Calls;
 CHECK(CrMdssDpIrqRegister(&D,&O,&T,(BOOLEAN*)&T)==CR_INVALID_PARAMETER&&T.Provider==&D&&H.Calls==N);
 CHECK(CrMdssDpIrqArm(&D,&O,&T,&R,&T.Serial)==CR_INVALID_PARAMETER&&H.Calls==N);
 CHECK(CrMdssDpIrqCapture(&D,&B,&B)==CR_INVALID_PARAMETER&&B==5&&H.Calls==N);
 CHECK(CrMdssIrqCaptureWithDp(&C,&D,(BOOLEAN*)&D.Armed,&Q)==CR_INVALID_PARAMETER&&!D.Armed&&Q==6&&H.Calls==N);
 CHECK(CrMdssIrqCaptureWithDp(&C,&D,&B,&B)==CR_INVALID_PARAMETER&&B==5&&H.Calls==N);
 CHECK(CrMdssDpIrqArm(&D,&O,&T,&R,&G)==CR_SUCCESS);
 H.Ahb[0x20/4]|=1U<<3;CHECK(CrMdssIrqCaptureWithDp(&C,&D,&B,&Q)==CR_SUCCESS);
 N=H.Calls;CHECK(CrMdssDpIrqTake(&D,&O,&T,(CR_MDSS_DP_IRQ_EVENT*)&D)==CR_INVALID_PARAMETER&&D.Pending&&H.Calls==N);
 Close(&D,&O,&T,&R);
}
int main(void){
 Nominal();Identity();Foreign();Aliases();UINT32 Phases=Failure(0,CR_DEVICE_ERROR,0);
#ifdef _KERNEL_MODE
 CR_STATUS Nonfinal=(NTSTATUS)0x103;
#else
 CR_STATUS Nonfinal=EFI_WARN_UNKNOWN_GLYPH;
#endif
 for(UINT32 I=1;I<=Phases;++I){Failure(I,CR_DEVICE_ERROR,0);Failure(I,Nonfinal,0);}
 for(UINT32 I=1;I<=4;++I)Failure(0,CR_DEVICE_ERROR,I);
 printf("DP IRQ actual combined ledger PASS %u checks, %u acquisition/capture callback phases\n",Checks,Phases);return 0;
}
