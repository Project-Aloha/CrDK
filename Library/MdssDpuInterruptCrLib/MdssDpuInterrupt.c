/* Owned TOP0 IRQ ledger; no parent line ownership grant. SPDX-License-Identifier: MIT */
#include <Library/MdssDpuInterruptCrLib.h>
#include <Library/MdssDpInterruptCrLib.h>
#include <Library/MdssIrqSourcesCrLib.h>

STATIC VOID Zero(VOID *P,UINTN N) {UINT8 *B=P;while(N--)*B++=0;}
STATIC CR_STATUS Final(CR_STATUS S) {return S==CR_SUCCESS?S:(CR_ERROR(S)?S:CR_NOT_READY);}
STATIC CR_STATUS Fail(CR_MDSS_IRQ_CORE *C,CR_STATUS S) {
 C->Faulted=TRUE;C->LastError=Final(S);return C->LastError;
}
STATIC UINT32 Mask(UINT32 I) {return I==0?CR_MDSS_IRQ_WB2:I==1?CR_MDSS_IRQ_DSI0_VBLANK:CR_MDSS_IRQ_DP0;}
STATIC INT32 Index(UINT32 M) {
 return M==CR_MDSS_IRQ_WB2?0:(M==CR_MDSS_IRQ_DSI0_VBLANK?1:(M==CR_MDSS_IRQ_DP0?2:-1));
}
STATIC BOOLEAN Valid(CONST CR_MDSS_IRQ_CORE *C) {return C&&C->Initialized;}
STATIC BOOLEAN Overlap(CONST VOID *A,UINTN N,CONST VOID *B,UINTN M) {
 UINTN X=(UINTN)A,Y=(UINTN)B;
 return !A||!B||X>~(UINTN)0-N||Y>~(UINTN)0-M||(X<Y+M&&Y<X+N);
}
STATIC BOOLEAN DpOutput(CR_MDSS_IRQ_CORE *C,CR_MDSS_DP_IRQ_CORE *D,CONST VOID *P,UINTN N) {
 if(!P||Overlap(C,sizeof(*C),P,N)||Overlap(D,sizeof(*D),P,N))return FALSE;
 if(D->Identity&&Overlap(D->Identity,sizeof(*D->Identity),P,N))return FALSE;
 if(D->Link.Identity&&Overlap(D->Link.Identity,sizeof(*D->Link.Identity),P,N))return FALSE;
 for(UINT32 I=0;I<CR_MDSS_IRQ_SLOT_COUNT;++I)if(C->Slots[I].Identity&&Overlap(C->Slots[I].Identity,sizeof(*C->Slots[I].Identity),P,N))return FALSE;
 return TRUE;
}
STATIC CR_MDSS_IRQ_SLOT *Match(CR_MDSS_IRQ_CORE *C,CONST VOID *O,CR_MDSS_IRQ_TOKEN *T) {
 INT32 I;
 if(!Valid(C)||!O||!T||(I=Index(T->Mask))<0)return NULL;
 CR_MDSS_IRQ_SLOT *S=&C->Slots[I];
 return S->Identity==T&&S->Owner==O&&T->Provider==C&&
 T->Incarnation==C->Incarnation&&T->Serial==S->Serial?S:NULL;
}
STATIC CR_STATUS Read(CR_MDSS_IRQ_CORE *C,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 *V) {
 return Final(C->Io.Read32(C->Io.Context,W,O,V));
}
STATIC CR_STATUS Enable(CR_MDSS_IRQ_CORE *C,UINT32 M,BOOLEAN On) {
 UINT32 V,A;CR_STATUS R=Read(C,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_ENABLE,&V);
 if(R!=CR_SUCCESS)return Fail(C,R);
 A=On?(V|M):(V&~M);
 if(A!=V){
  R=Final(C->Io.Write32(C->Io.Context,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_ENABLE,A));
  if(R!=CR_SUCCESS)return Fail(C,R);
  R=Final(C->Io.Barrier(C->Io.Context));if(R!=CR_SUCCESS)return Fail(C,R);
 }
 R=Read(C,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_ENABLE,&A);
 if(R!=CR_SUCCESS)return Fail(C,R);
 return (A&M)==(On?M:0)?CR_SUCCESS:Fail(C,CR_NOT_READY);
}
STATIC CR_STATUS Ack(CR_MDSS_IRQ_CORE *C,UINT32 M) {
 UINT32 V;CR_STATUS R;
 if(!M)return CR_SUCCESS;
 R=Final(C->Io.Write32(C->Io.Context,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_CLEAR,M));
 if(R!=CR_SUCCESS)return Fail(C,R);
 R=Final(C->Io.Barrier(C->Io.Context));if(R!=CR_SUCCESS)return Fail(C,R);
 R=Read(C,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_STATUS,&V);
 if(R!=CR_SUCCESS)return Fail(C,R);
 /* A immediately reasserted level is conservatively retained as uncertainty. */
 return (V&M)?Fail(C,CR_NOT_READY):CR_SUCCESS;
}
STATIC CR_STATUS OwnPending(CR_MDSS_IRQ_CORE *C,UINT32 M) {
 UINT32 V;CR_STATUS R=Read(C,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_STATUS,&V);
 return R==CR_SUCCESS?Ack(C,V&M):Fail(C,R);
}
CR_STATUS CrMdssIrqInit(CR_MDSS_IRQ_CORE *C,CONST CR_MDSS_IRQ_IO *Io,UINT64 Inc) {
 if(!C||!Io||!Io->Context||!Io->Read32||!Io->Write32||!Io->Barrier||!Inc)return CR_INVALID_PARAMETER;
 if(C->Initialized||C->Slots[0].Identity||C->Slots[1].Identity||C->Slots[2].Identity)return CR_BUSY;
 Zero(C,sizeof(*C));C->Io=*Io;C->Incarnation=Inc;C->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrMdssIrqRegister(CR_MDSS_IRQ_CORE *C,CONST VOID *O,UINT32 M,
 CR_MDSS_IRQ_TOKEN *T,BOOLEAN *Owned) {
 INT32 I=Index(M);UINT32 V;CR_STATUS R;CR_MDSS_IRQ_SLOT *S;
 if(!Valid(C)||!O||!T||!Owned||I<0)return CR_INVALID_PARAMETER;
 S=&C->Slots[I];
 if(Match(C,O,T)==S){*Owned=TRUE;return C->Closed||C->Faulted?CR_NOT_READY:CR_SUCCESS;}
 if(T->Provider||T->Serial||T->Incarnation||T->Mask)return CR_INVALID_PARAMETER;
 *Owned=FALSE;
 if(C->Closed||C->Faulted)return CR_NOT_READY;
 if(S->Identity)return CR_BUSY;
 if(C->Serial==CR_MAX_UINT64)return Fail(C,CR_OUT_OF_RESOURCES);
 R=Read(C,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_ENABLE,&V);if(R!=CR_SUCCESS)return R;
 if(V&M)return CR_BUSY;
 S->Identity=T;S->Owner=O;S->Serial=++C->Serial;
 T->Provider=C;T->Incarnation=C->Incarnation;T->Serial=S->Serial;T->Mask=M;*Owned=TRUE;
 return OwnPending(C,M);
}
CR_STATUS CrMdssIrqArm(CR_MDSS_IRQ_CORE *C,CONST VOID *O,CR_MDSS_IRQ_TOKEN *T,
 CONST VOID *Job,UINT64 *Generation) {
 CR_MDSS_IRQ_SLOT *S=Match(C,O,T);CR_STATUS R;
 if(!S||!Generation||((T->Mask==CR_MDSS_IRQ_WB2)!=(Job!=NULL)))return CR_INVALID_PARAMETER;
 if(C->Closed||C->Faulted)return CR_NOT_READY;
 if(S->Armed||S->Job||S->Pending)return CR_BUSY;
 if(C->Generation==CR_MAX_UINT64)return Fail(C,CR_OUT_OF_RESOURCES);
 R=Enable(C,T->Mask,FALSE);if(R!=CR_SUCCESS)return R;
 R=OwnPending(C,T->Mask);if(R!=CR_SUCCESS)return R;
 S->Generation=++C->Generation;S->Job=Job;S->Armed=TRUE;S->Observed=0;
 /* Commit ownership before the first enabling write, including its failure. */
 R=Enable(C,T->Mask,TRUE);if(R!=CR_SUCCESS)return R;
 *Generation=S->Generation;return CR_SUCCESS;
}
static CR_STATUS Capture(CR_MDSS_IRQ_CORE *C,UINT32 OtherParent,BOOLEAN *Handled,BOOLEAN *Pending) {
 UINT32 P,V,E,Owned=0,Observed;CR_STATUS R;
 if(!Valid(C)||!Handled||!Pending)return CR_INVALID_PARAMETER;
 if(C->Closed||C->Faulted)return CR_NOT_READY;
 R=Read(C,CrMdssIrqParent,CR_MDSS_IRQ_PARENT_STATUS,&P);if(R!=CR_SUCCESS)return Fail(C,R);
 if(P&~(CR_MDSS_IRQ_PARENT_MDP|OtherParent))return Fail(C,CR_NOT_READY);
 if(!(P&CR_MDSS_IRQ_PARENT_MDP)){*Handled=FALSE;*Pending=FALSE;return CR_SUCCESS;}
 R=Read(C,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_STATUS,&V);if(R!=CR_SUCCESS)return Fail(C,R);
 R=Read(C,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_ENABLE,&E);if(R!=CR_SUCCESS)return Fail(C,R);
 for(UINT32 I=0;I<CR_MDSS_IRQ_SLOT_COUNT;++I)if(C->Slots[I].Identity&&C->Slots[I].Armed)Owned|=Mask(I);
 Observed=V&E;
 if(Observed&~Owned)return Fail(C,CR_NOT_READY);
 if(!Observed){*Handled=FALSE;*Pending=FALSE;return CR_SUCCESS;}
 for(UINT32 I=0;I<CR_MDSS_IRQ_SLOT_COUNT;++I)if((Observed&Mask(I))&&C->Slots[I].Sequence==CR_MAX_UINT64)return Fail(C,CR_OUT_OF_RESOURCES);
 if(Observed&CR_MDSS_IRQ_WB2){R=Enable(C,CR_MDSS_IRQ_WB2,FALSE);if(R!=CR_SUCCESS)return R;}
 R=Ack(C,Observed);if(R!=CR_SUCCESS)return R;
 for(UINT32 I=0;I<CR_MDSS_IRQ_SLOT_COUNT;++I)if(Observed&Mask(I)){
  CR_MDSS_IRQ_SLOT *S=&C->Slots[I];++S->Sequence;S->Pending=TRUE;S->Observed|=Observed&Mask(I);
  if(!I)S->Armed=FALSE;
 }
 *Handled=TRUE;*Pending=TRUE;return CR_SUCCESS;
}
CR_STATUS CrMdssIrqCapture(CR_MDSS_IRQ_CORE *C,BOOLEAN *Handled,BOOLEAN *Pending) {
 return Capture(C,0,Handled,Pending);
}
CR_STATUS CrMdssIrqCaptureWithDp(CR_MDSS_IRQ_CORE *C,CR_MDSS_DP_IRQ_CORE *Dp,
 BOOLEAN *Handled,BOOLEAN *Pending) {
 if(!Valid(C)||!Dp||!DpOutput(C,Dp,Handled,sizeof(*Handled))||
    !DpOutput(C,Dp,Pending,sizeof(*Pending))||Overlap(Handled,sizeof(*Handled),Pending,sizeof(*Pending)))return CR_INVALID_PARAMETER;
 UINT32 Allowed=0;
 for(UINT32 N=0;N<CR_MDSS_IRQ_SLOT_COUNT;++N)if(C->Slots[N].Identity&&C->Slots[N].Armed)Allowed|=Mask(N);
 CR_MDSS_IRQ_SOURCE_OBSERVATION O;
 CR_STATUS S=CrMdssIrqSourcesQuietWithDp(&C->Io,Allowed,Dp,&O);
 if(S!=CR_SUCCESS){CrMdssDpIrqQuarantine(Dp);return Fail(C,S);}
 BOOLEAN A=FALSE,B=FALSE,X=FALSE,Y=FALSE;
 S=Capture(C,CR_MDSS_IRQ_PARENT_DP,&A,&B);
 if(S==CR_SUCCESS)S=CrMdssDpIrqCapture(Dp,&X,&Y);
 if(S==CR_SUCCESS){BOOLEAN LH=FALSE,LQ=FALSE;S=CrMdssDpLinkIrqCapture(Dp,&LH,&LQ);X=X||LH;Y=Y||LQ;}
 if(S!=CR_SUCCESS){CrMdssDpIrqQuarantine(Dp);return Fail(C,S);}
 *Handled=A||X;*Pending=B||Y;return CR_SUCCESS;
}
CR_STATUS CrMdssIrqTake(CR_MDSS_IRQ_CORE *C,CONST VOID *O,CR_MDSS_IRQ_TOKEN *T,
 CR_MDSS_IRQ_EVENT *Out) {
 CR_MDSS_IRQ_SLOT *S=Match(C,O,T);CR_MDSS_IRQ_EVENT V;
 if(!S||!Out)return CR_INVALID_PARAMETER;
 if(C->Closed||C->Faulted)return CR_NOT_READY;
 if(!S->Pending)return CR_NOT_FOUND;
 V.Identity=T;V.Owner=O;V.Job=S->Job;V.Registration=S->Serial;
 V.Generation=S->Generation;V.Sequence=S->Sequence;V.Mask=T->Mask==CR_MDSS_IRQ_DP0?S->Observed:T->Mask;
 *Out=V;S->Pending=FALSE;S->Observed=0;return CR_SUCCESS;
}
CR_STATUS CrMdssIrqDisarm(CR_MDSS_IRQ_CORE *C,CONST VOID *O,CR_MDSS_IRQ_TOKEN *T) {
 CR_MDSS_IRQ_SLOT *S=Match(C,O,T);CR_STATUS R;
 if(!S)return CR_INVALID_PARAMETER;
 R=Enable(C,T->Mask,FALSE);if(R!=CR_SUCCESS)return R;
 R=OwnPending(C,T->Mask);if(R!=CR_SUCCESS)return R;
 S->Armed=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssIrqRetireJob(CR_MDSS_IRQ_CORE *C,CONST VOID *O,CR_MDSS_IRQ_TOKEN *T,
 CONST VOID *Job) {
 CR_MDSS_IRQ_SLOT *S=Match(C,O,T);
 if(!S||T->Mask!=CR_MDSS_IRQ_WB2||!Job||S->Job!=Job)return CR_INVALID_PARAMETER;
 if(S->Armed||S->Pending)return CR_BUSY;
 S->Job=NULL;return CR_SUCCESS;
}
CR_STATUS CrMdssIrqUnregister(CR_MDSS_IRQ_CORE *C,CONST VOID *O,CR_MDSS_IRQ_TOKEN *T) {
 CR_MDSS_IRQ_SLOT *S=Match(C,O,T);CR_STATUS R;
 if(!S)return CR_INVALID_PARAMETER;
 if(S->Job||S->Pending)return CR_BUSY;
 R=CrMdssIrqDisarm(C,O,T);if(R!=CR_SUCCESS)return R;
 Zero(S,sizeof(*S));Zero(T,sizeof(*T));return CR_SUCCESS;
}
CR_STATUS CrMdssIrqDiscard(CR_MDSS_IRQ_CORE *C,CONST VOID *O,CR_MDSS_IRQ_TOKEN *T) {
 CR_MDSS_IRQ_SLOT *S=Match(C,O,T);
 if(!S)return CR_INVALID_PARAMETER;
 if(S->Armed)return CR_BUSY;
 S->Pending=FALSE;S->Observed=0;return CR_SUCCESS;
}
VOID CrMdssIrqQuarantine(CR_MDSS_IRQ_CORE *C) {
 if(Valid(C)){C->Closed=TRUE;C->LastError=CR_NOT_READY;}
}
CR_STATUS CrMdssIrqUninit(CR_MDSS_IRQ_CORE *C) {
 if(!Valid(C))return CR_INVALID_PARAMETER;
 if(C->Slots[0].Identity||C->Slots[1].Identity||C->Slots[2].Identity)return CR_BUSY;
 Zero(C,sizeof(*C));return CR_SUCCESS;
}
