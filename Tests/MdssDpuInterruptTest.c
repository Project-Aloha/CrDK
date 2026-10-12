/* Actual owned TOP0 core with independent W1C/MMIO model. */
#include <Library/MdssDpuInterruptCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {UINT32 Parent,Enable,Status,Writes,Calls,Fail,Drop,LastAck;CR_STATUS Error;} HW;
static UINT32 Checks;
#define CHECK(X) do {++Checks;assert(X);} while(0)
static CR_STATUS Step(HW *H) {++H->Calls;return H->Calls==H->Fail?H->Error:CR_SUCCESS;}
static CR_STATUS Read(VOID *P,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 *V) {
 HW *H=P;CR_STATUS R=Step(H);if(R!=CR_SUCCESS)return R;
 if(W==CrMdssIrqParent){CHECK(O==0x10);*V=H->Parent;}
 else {CHECK(W==CrMdssIrqDpu);CHECK(O==0x10||O==0x14);*V=O==0x10?H->Enable:H->Status;}
 return CR_SUCCESS;
}
static CR_STATUS Write(VOID *P,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 V) {
 HW *H=P;CR_STATUS R=Step(H);++H->Writes;CHECK(W==CrMdssIrqDpu);CHECK(O==0x10||O==0x18);
 if(R!=CR_SUCCESS)return R;
 if(H->Writes==H->Drop)return CR_SUCCESS;
 if(O==0x10)H->Enable=V;else {H->LastAck=V;H->Status&=~V;}
 return CR_SUCCESS;
}
static CR_STATUS Barrier(VOID *P){return Step(P);}
static CR_MDSS_IRQ_IO Io(HW *H){CR_MDSS_IRQ_IO I={H,Read,Write,Barrier};return I;}
static void Init(HW *H,CR_MDSS_IRQ_CORE *C){memset(H,0,sizeof(*H));memset(C,0,sizeof(*C));H->Error=CR_DEVICE_ERROR;CR_MDSS_IRQ_IO I=Io(H);CHECK(CrMdssIrqInit(C,&I,99)==CR_SUCCESS);}
static void Register(CR_MDSS_IRQ_CORE *C,VOID *Owner,UINT32 M,CR_MDSS_IRQ_TOKEN *T){BOOLEAN O=FALSE;memset(T,0,sizeof(*T));CHECK(CrMdssIrqRegister(C,Owner,M,T,&O)==CR_SUCCESS&&O);}
static void Close(CR_MDSS_IRQ_CORE *C,VOID *O,CR_MDSS_IRQ_TOKEN *T,VOID *J){CHECK(CrMdssIrqDisarm(C,O,T)==CR_SUCCESS);CHECK(CrMdssIrqDiscard(C,O,T)==CR_SUCCESS);if(J)CHECK(CrMdssIrqRetireJob(C,O,T,J)==CR_SUCCESS);CHECK(CrMdssIrqUnregister(C,O,T)==CR_SUCCESS);}
static void Nominal(void){HW H;CR_MDSS_IRQ_CORE C;CR_MDSS_IRQ_TOKEN W,V;int O,J;UINT64 G,Gv;BOOLEAN B,Q;CR_MDSS_IRQ_EVENT E;
 Init(&H,&C);H.Enable=1U<<20;H.Status=1U<<21;Register(&C,&O,CR_MDSS_IRQ_WB2,&W);Register(&C,&O,CR_MDSS_IRQ_DSI0_VBLANK,&V);
 CHECK(CrMdssIrqArm(&C,&O,&W,&J,&G)==CR_SUCCESS);CHECK(CrMdssIrqArm(&C,&O,&V,NULL,&Gv)==CR_SUCCESS&&Gv>G);
 CHECK(H.Enable==((CR_MDSS_IRQ_WB2|CR_MDSS_IRQ_DSI0_VBLANK)|(1U<<20)));CHECK(H.Status==(1U<<21));
 H.Parent=1;H.Status|=(CR_MDSS_IRQ_WB2|CR_MDSS_IRQ_DSI0_VBLANK);CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_SUCCESS&&B&&Q);
 CHECK(H.LastAck==(CR_MDSS_IRQ_WB2|CR_MDSS_IRQ_DSI0_VBLANK));CHECK(H.Status==(1U<<21));CHECK(H.Enable==(CR_MDSS_IRQ_DSI0_VBLANK|(1U<<20)));
 CHECK(CrMdssIrqTake(&C,&O,&W,&E)==CR_SUCCESS&&E.Job==&J&&E.Generation==G&&E.Sequence==1&&E.Identity==&W);
 CR_MDSS_IRQ_EVENT Saved=E;CHECK(CrMdssIrqTake(&C,&O,&W,&E)==CR_NOT_FOUND&&!memcmp(&E,&Saved,sizeof(E)));
 for(UINT32 I=0;I<4;++I){H.Status|=CR_MDSS_IRQ_DSI0_VBLANK;CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_SUCCESS);}
 CHECK(CrMdssIrqTake(&C,&O,&V,&E)==CR_SUCCESS&&E.Sequence==5&&E.Job==NULL&&E.Generation==Gv);
 CHECK(CrMdssIrqArm(&C,&O,&W,&J,&G)==CR_BUSY);Close(&C,&O,&W,&J);Close(&C,&O,&V,NULL);CHECK(H.Enable==(1U<<20));CHECK(CrMdssIrqUninit(&C)==CR_SUCCESS);
}
static void ThreeSlots(void) {
 HW H;CR_MDSS_IRQ_CORE C;CR_MDSS_IRQ_TOKEN W,V,D,Copy;int O,J,F;
 UINT64 Gw,Gv,Gd;BOOLEAN B=FALSE,Q=FALSE;CR_MDSS_IRQ_EVENT E;
 Init(&H,&C);H.Enable=1U<<20;H.Status=1U<<21;
 Register(&C,&O,CR_MDSS_IRQ_WB2,&W);
 Register(&C,&O,CR_MDSS_IRQ_DSI0_VBLANK,&V);
 Register(&C,&O,CR_MDSS_IRQ_DP0,&D);
 CHECK(CrMdssIrqArm(&C,&O,&W,&J,&Gw)==CR_SUCCESS);
 CHECK(CrMdssIrqArm(&C,&O,&V,NULL,&Gv)==CR_SUCCESS);
 CHECK(CrMdssIrqArm(&C,&O,&D,NULL,&Gd)==CR_SUCCESS&&Gd>Gv&&Gv>Gw);
 CHECK(H.Enable==(CR_MDSS_IRQ_WB2|CR_MDSS_IRQ_DSI0_VBLANK|
                  CR_MDSS_IRQ_DP0_UNDERRUN|CR_MDSS_IRQ_DP0_VBLANK|(1U<<20)));
 Copy=D;UINT32 Calls=H.Calls;
 CHECK(CrMdssIrqDisarm(&C,&O,&Copy)==CR_INVALID_PARAMETER);
 CHECK(CrMdssIrqUnregister(&C,&F,&D)==CR_INVALID_PARAMETER&&H.Calls==Calls);
 H.Parent=1;H.Status|=CR_MDSS_IRQ_WB2|CR_MDSS_IRQ_DSI0_VBLANK|CR_MDSS_IRQ_DP0_VBLANK;
 CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_SUCCESS&&B&&Q);
 CHECK(H.LastAck==(CR_MDSS_IRQ_WB2|CR_MDSS_IRQ_DSI0_VBLANK|CR_MDSS_IRQ_DP0_VBLANK));
 CHECK(H.Status==(1U<<21));
 CHECK(H.Enable==(CR_MDSS_IRQ_DSI0_VBLANK|CR_MDSS_IRQ_DP0|(1U<<20)));
 H.Status|=CR_MDSS_IRQ_DP0_UNDERRUN;
 CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_SUCCESS&&B&&Q);
 CHECK(H.LastAck==CR_MDSS_IRQ_DP0_UNDERRUN);
 CHECK(CrMdssIrqTake(&C,&O,&D,&E)==CR_SUCCESS&&E.Mask==CR_MDSS_IRQ_DP0&&
       E.Sequence==2&&E.Generation==Gd&&E.Identity==&D&&E.Owner==&O&&E.Job==NULL);
 CR_MDSS_IRQ_EVENT Saved=E;
 CHECK(CrMdssIrqTake(&C,&O,&D,&E)==CR_NOT_FOUND&&!memcmp(&E,&Saved,sizeof(E)));
 H.Status|=CR_MDSS_IRQ_DP0_VBLANK;
 CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_SUCCESS);
 CHECK(CrMdssIrqTake(&C,&O,&D,&E)==CR_SUCCESS&&E.Mask==CR_MDSS_IRQ_DP0_VBLANK&&E.Sequence==3);
 Close(&C,&O,&W,&J);Close(&C,&O,&V,NULL);
 CHECK(CrMdssIrqUninit(&C)==CR_BUSY); /* the third owner remains retained */
 Close(&C,&O,&D,NULL);CHECK(H.Enable==(1U<<20));CHECK(CrMdssIrqUninit(&C)==CR_SUCCESS);

 /* A disabled unowned DP status is neither acknowledged nor attributed to DSI.
    Enabling that foreign source instead closes capture before any write. */
 Init(&H,&C);Register(&C,&O,CR_MDSS_IRQ_DSI0_VBLANK,&V);
 CHECK(CrMdssIrqArm(&C,&O,&V,NULL,&Gv)==CR_SUCCESS);
 H.Parent=1;H.Status=CR_MDSS_IRQ_DP0_UNDERRUN|CR_MDSS_IRQ_DSI0_VBLANK;
 CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_SUCCESS&&H.LastAck==CR_MDSS_IRQ_DSI0_VBLANK);
 CHECK(H.Status==CR_MDSS_IRQ_DP0_UNDERRUN);
 H.Enable|=CR_MDSS_IRQ_DP0_UNDERRUN;UINT32 Writes=H.Writes;B=7;Q=8;
 CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_NOT_READY&&B==7&&Q==8&&H.Writes==Writes&&C.Faulted);
 Close(&C,&O,&V,NULL);CHECK(H.Status==CR_MDSS_IRQ_DP0_UNDERRUN);
 CHECK(H.Enable==CR_MDSS_IRQ_DP0_UNDERRUN);CHECK(CrMdssIrqUninit(&C)==CR_SUCCESS);
}
static void Identities(void){HW H;CR_MDSS_IRQ_CORE C;CR_MDSS_IRQ_TOKEN T,Copy;int O,F,J;BOOLEAN B;UINT64 G=123;
 Init(&H,&C);Register(&C,&O,CR_MDSS_IRQ_WB2,&T);Copy=T;UINT32 N=H.Calls;
 CHECK(CrMdssIrqArm(&C,&O,&Copy,&J,&G)==CR_INVALID_PARAMETER);CHECK(CrMdssIrqArm(&C,&F,&T,&J,&G)==CR_INVALID_PARAMETER);
 CHECK(CrMdssIrqUnregister(&C,&F,&T)==CR_INVALID_PARAMETER);CHECK(H.Calls==N&&G==123);
 CHECK(CrMdssIrqRegister(&C,&O,T.Mask,&T,&B)==CR_SUCCESS&&B&&H.Calls==N);
 CHECK(CrMdssIrqInit(&C,&C.Io,99)==CR_BUSY);T.Serial++;CHECK(CrMdssIrqDisarm(&C,&O,&T)==CR_INVALID_PARAMETER);T.Serial--;
 Close(&C,&O,&T,NULL);memset(&T,0,sizeof(T));H.Enable=CR_MDSS_IRQ_WB2;N=H.Writes;
 CHECK(CrMdssIrqRegister(&C,&O,CR_MDSS_IRQ_WB2,&T,&B)==CR_BUSY&&!B&&T.Provider==NULL&&H.Writes==N);
}
static UINT32 ArmFault(UINT32 At,CR_STATUS Error,UINT32 Drop){HW H;CR_MDSS_IRQ_CORE C;CR_MDSS_IRQ_TOKEN T;int O,J;UINT64 G=456;
 Init(&H,&C);Register(&C,&O,CR_MDSS_IRQ_WB2,&T);H.Calls=H.Writes=0;H.Fail=At;H.Error=Error;H.Drop=Drop;
 CR_STATUS R=CrMdssIrqArm(&C,&O,&T,&J,&G);UINT32 N=H.Calls;
 if(R!=CR_SUCCESS){CHECK(G==456&&C.Faulted);CHECK(T.Provider==&C);if(C.Slots[0].Job)CHECK(C.Slots[0].Job==&J);CHECK(CrMdssIrqArm(&C,&O,&T,&J,&G)==CR_NOT_READY);}
 H.Fail=H.Drop=0;Close(&C,&O,&T,C.Slots[0].Job?&J:NULL);return N;
}
static UINT32 CaptureFault(UINT32 At,CR_STATUS Error,UINT32 Drop){HW H;CR_MDSS_IRQ_CORE C;CR_MDSS_IRQ_TOKEN T;int O,J;UINT64 G;BOOLEAN B=7,Q=8;
 Init(&H,&C);Register(&C,&O,CR_MDSS_IRQ_WB2,&T);CHECK(CrMdssIrqArm(&C,&O,&T,&J,&G)==CR_SUCCESS);
 H.Calls=H.Writes=0;H.Fail=At;H.Error=Error;H.Drop=Drop;H.Parent=1;H.Status=CR_MDSS_IRQ_WB2;
 CR_STATUS R=CrMdssIrqCapture(&C,&B,&Q);UINT32 N=H.Calls;
 if(R!=CR_SUCCESS){CHECK(B==7&&Q==8&&C.Faulted);CHECK(C.Slots[0].Job==&J&&!C.Slots[0].Pending);}
 else CHECK(B&&Q&&C.Slots[0].Pending&&!C.Slots[0].Armed);
 H.Fail=H.Drop=0;Close(&C,&O,&T,&J);return N;
}
static void Hostile(void){HW H;CR_MDSS_IRQ_CORE C;CR_MDSS_IRQ_TOKEN T;int O,J;UINT64 G;BOOLEAN B=7,Q=8;CR_MDSS_IRQ_EVENT E;
 Init(&H,&C);Register(&C,&O,CR_MDSS_IRQ_WB2,&T);CHECK(CrMdssIrqArm(&C,&O,&T,&J,&G)==CR_SUCCESS);
 H.Parent=1;H.Enable|=1U<<20;H.Status=(1U<<20)|CR_MDSS_IRQ_WB2;UINT32 N=H.Writes;
 CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_NOT_READY&&B==7&&Q==8&&H.Writes==N);CHECK(H.Status==((1U<<20)|CR_MDSS_IRQ_WB2));
 CrMdssIrqQuarantine(&C);N=H.Calls;CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_NOT_READY);CHECK(CrMdssIrqTake(&C,&O,&T,&E)==CR_NOT_READY);CHECK(H.Calls==N);CHECK(CrMdssIrqUninit(&C)==CR_BUSY);
 Close(&C,&O,&T,&J);CHECK(H.Enable==(1U<<20));CHECK(CrMdssIrqUninit(&C)==CR_SUCCESS);
 Init(&H,&C);Register(&C,&O,CR_MDSS_IRQ_DSI0_VBLANK,&T);CHECK(CrMdssIrqArm(&C,&O,&T,NULL,&G)==CR_SUCCESS);C.Slots[1].Sequence=CR_MAX_UINT64;H.Parent=1;H.Status=CR_MDSS_IRQ_DSI0_VBLANK;N=H.Writes;CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_OUT_OF_RESOURCES&&H.Writes==N);Close(&C,&O,&T,NULL);
 Init(&H,&C);Register(&C,&O,CR_MDSS_IRQ_WB2,&T);H.Parent=1U<<5;N=H.Writes;CHECK(CrMdssIrqCapture(&C,&B,&Q)==CR_NOT_READY&&H.Writes==N);Close(&C,&O,&T,NULL);
}
int main(void){Nominal();ThreeSlots();Identities();Hostile();UINT32 A=ArmFault(0,CR_DEVICE_ERROR,0),C=CaptureFault(0,CR_DEVICE_ERROR,0);
#ifdef _KERNEL_MODE
 CR_STATUS Positive=(NTSTATUS)0x103; /* actual STATUS_PENDING */
#else
 CR_STATUS Positive=EFI_WARN_UNKNOWN_GLYPH;
#endif
 for(UINT32 I=1;I<=A;++I){ArmFault(I,CR_DEVICE_ERROR,0);ArmFault(I,Positive,0);}
 for(UINT32 I=1;I<=C;++I){CaptureFault(I,CR_DEVICE_ERROR,0);CaptureFault(I,Positive,0);}
 ArmFault(0,CR_DEVICE_ERROR,1);CaptureFault(0,CR_DEVICE_ERROR,1);CaptureFault(0,CR_DEVICE_ERROR,2);
 printf("Actual TOP0 IRQ core PASS %u checks; %u arm and %u ISR IO phases, retained errors/nonfinal/dropped writes\n",Checks,A,C);return 0;}
