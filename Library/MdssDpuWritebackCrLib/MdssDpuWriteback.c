/* SPDX-License-Identifier: GPL-2.0-only
 * Register recipe derived from Linux dpu_hw_{sspp,lm,ctl,wb,vbif} and the
 * Qualcomm Android SM8450 display driver. Provenance and upstream notices
 * are recorded in README.md. This is a checked, single-job portable port.
 */
#include <Library/MdssDpuWritebackCrLib.h>
#include <oskal/cr_string.h>
#define DMA 0x26000U
#define LM 0x46000U
#define CTL 0x17000U
#define WB 0x65000U
#define MERGE 0x4f000U
#define XINS ((1U<<5)|(1U<<6))
#define ACKS (XINS<<CR_MDSS_WB_ACK_SHIFT)
#define FLUSH 0x31100U
#define ROUTE 0x1200000U
#define FULL 0xffffffffU
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
static CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
static BOOLEAN Alias(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn){
 UINTN X=(UINTN)A,Y=(UINTN)B;
 return An>(UINTN)-1-X||Bn>(UINTN)-1-Y||(X<Y+Bn&&Y<X+An);
}
static CR_STATUS Read(CR_MDSS_DPU_WRITEBACK *D,CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 *V){
 UINT32 T;if(W>=CrMdssWbWindowCount||(O&3)||D->Io.Bytes[W]<4||O>D->Io.Bytes[W]-4)return CR_BAD_BUFFER_SIZE;
 CR_STATUS S=Exact(D->Io.Read32(D->Io.Context,W,O,&T));if(S==CR_SUCCESS)*V=T;return S;
}
static CR_STATUS Write(CR_MDSS_DPU_WRITEBACK *D,CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 V){
 if(W>=CrMdssWbWindowCount||(O&3)||D->Io.Bytes[W]<4||O>D->Io.Bytes[W]-4)return CR_BAD_BUFFER_SIZE;
 return Exact(D->Io.Write32(D->Io.Context,W,O,V));
}
static CR_STATUS Fence(CR_MDSS_DPU_WRITEBACK *D){return Exact(D->Io.Fence(D->Io.Context));}
static CR_STATUS Delay(CR_MDSS_DPU_WRITEBACK *D){return Exact(D->Io.DelayUs(D->Io.Context,50));}
static CR_STATUS Match(CR_MDSS_DPU_WRITEBACK *D,CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 M,UINT32 Want){
 UINT32 V;CR_STATUS S=Read(D,W,O,&V);return S!=CR_SUCCESS?S:(V&M)==(Want&M)?CR_SUCCESS:CR_DEVICE_ERROR;
}
static CR_STATUS Rmw(CR_MDSS_DPU_WRITEBACK *D,CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 M,UINT32 Bits){
 UINT32 V;CR_STATUS S=Read(D,W,O,&V);if(S!=CR_SUCCESS)return S;
 S=Write(D,W,O,(V&~M)|(Bits&M));if(S!=CR_SUCCESS)return S;
 S=Fence(D);return S!=CR_SUCCESS?S:Match(D,W,O,M,Bits);
}
/* Only ordinary software-owned configuration goes here. No W1C, START,
 * FLUSH, SW_RESET, halt ACK or idle status can be saved/restored by this helper. */
static CR_STATUS Program(CR_MDSS_DPU_WRITEBACK *D,CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 M,UINT32 Bits){
 UINT32 I,V;CR_STATUS S;
 for(I=0;I<D->JournalCount;++I)if(D->Journal[I].Window==W&&D->Journal[I].Offset==O)break;
 if(I==D->JournalCount){
  if(I==CR_MDSS_WB_JOURNAL_MAX)return CR_OUT_OF_RESOURCES;
  S=Read(D,W,O,&V);if(S!=CR_SUCCESS)return S;
  CR_MDSS_WB_REGISTER *J=&D->Journal[I];J->Window=W;J->Offset=O;J->Mask=M;
  J->Original=V&M;J->Desired=Bits&M;++D->JournalCount;
 }else if(D->Journal[I].Mask!=M||D->Journal[I].Desired!=(Bits&M))return CR_COMPROMISED_DATA;
 D->Journal[I].Touched=TRUE;D->Journal[I].Restored=FALSE;
 D->Touched=TRUE;D->CleanupPending=TRUE;
 return Rmw(D,W,O,M,Bits);
}
static CR_STATUS Wait(CR_MDSS_DPU_WRITEBACK *D,CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 M,UINT32 Bits,UINT32 Limit){
 for(UINT32 Us=0;Us<=Limit;Us+=50){UINT32 V;CR_STATUS S=Read(D,W,O,&V);if(S!=CR_SUCCESS)return S;
  if((V&M)==Bits)return CR_SUCCESS;if(Us+50>Limit)break;S=Delay(D);if(S!=CR_SUCCESS)return S;
 }return CR_TIMEOUT;
}
static CR_STATUS Force(CR_MDSS_DPU_WRITEBACK *D){
 CR_STATUS S=Program(D,CrMdssWbDpu,0x2b4,1U<<8,1U<<8);
 return S!=CR_SUCCESS?S:Program(D,CrMdssWbDpu,0x2bc,1U<<16,1U<<16);
}
static CR_STATUS Idle(CR_MDSS_DPU_WRITEBACK *D){
 CR_STATUS S=Match(D,CrMdssWbDpu,0x2b8,1U<<12,0);
 return S!=CR_SUCCESS?S:Match(D,CrMdssWbDpu,0x3bc,1U<<20,0);
}
static CR_STATUS Halt(CR_MDSS_DPU_WRITEBACK *D){
 CR_STATUS S=Force(D);if(S!=CR_SUCCESS)return S;
 if(!D->Halted){
  /* Clear first, and actually observe low before requesting a fresh high. */
  S=Rmw(D,CrMdssWbVbif,0x200,XINS,0);if(S!=CR_SUCCESS)return S;
  S=Wait(D,CrMdssWbVbif,0x204,ACKS,0,16384);if(S!=CR_SUCCESS)return S;
 }
 S=Rmw(D,CrMdssWbVbif,0x200,XINS,XINS);if(S!=CR_SUCCESS)return S;
 S=Wait(D,CrMdssWbVbif,0x204,ACKS,ACKS,16384);if(S!=CR_SUCCESS)return S;
 S=Idle(D);if(S==CR_SUCCESS)D->Halted=TRUE;return S;
}
static CR_STATUS Unhalt(CR_MDSS_DPU_WRITEBACK *D){
 /* Before the first possibly committed unhalt, invalidate the old witness. */
 D->Halted=FALSE;CR_STATUS S=Rmw(D,CrMdssWbVbif,0x200,XINS,0);
 return S!=CR_SUCCESS?S:Wait(D,CrMdssWbVbif,0x204,ACKS,0,16384);
}
static BOOLEAN SameJob(CONST CR_MDSS_WB_JOB *A,CONST CR_MDSS_WB_JOB *B){
 return A->Owner==B->Owner&&A->InputIova==B->InputIova&&A->OutputIova==B->OutputIova&&
 A->InputBytes==B->InputBytes&&A->OutputBytes==B->OutputBytes&&A->Width==B->Width&&
 A->Height==B->Height&&A->InputStride==B->InputStride&&A->OutputStride==B->OutputStride&&
 A->Reflections==B->Reflections&&A->PixelsPerSecond==B->PixelsPerSecond;
}
static BOOLEAN SameToken(CONST CR_MDSS_IRQ_TOKEN *A,CONST CR_MDSS_IRQ_TOKEN *B){
 return A->Provider==B->Provider&&A->Incarnation==B->Incarnation&&A->Serial==B->Serial&&A->Mask==B->Mask;
}
static CR_STATUS Geometry(CONST CR_MDSS_WB_JOB *J){
 if(!J->Owner||!J->Width||J->Width>CR_MDSS_WB_MAX_WIDTH||!J->Height||J->Height>65535||
   (J->Reflections!=0&&J->Reflections!=3)||!J->PixelsPerSecond)return CR_INVALID_PARAMETER;
 UINT64 I=(UINT64)(J->Height-1)*J->InputStride+(UINT64)J->Width*4;
 UINT64 O=(UINT64)(J->Height-1)*J->OutputStride+(UINT64)J->Width*4;
 if(J->InputStride<J->Width*4||J->OutputStride<J->Width*4||(J->InputStride&3)||(J->OutputStride&3)||
    J->InputStride>65535||J->OutputStride>65535||!J->InputBytes||!J->OutputBytes||
    (J->InputBytes&4095)||(J->OutputBytes&4095)||I>J->InputBytes||O>J->OutputBytes||
    I>CR_MDSS_WB_MAX_BYTES||O>CR_MDSS_WB_MAX_BYTES||!J->InputIova||!J->OutputIova||
    (J->InputIova&4095)||(J->OutputIova&4095)||J->InputIova>CR_MAX_UINT32||J->OutputIova>CR_MAX_UINT32||
    J->InputBytes-1>CR_MAX_UINT32-J->InputIova||J->OutputBytes-1>CR_MAX_UINT32-J->OutputIova)return CR_BAD_BUFFER_SIZE;
 if(J->InputIova<J->OutputIova+J->OutputBytes&&J->OutputIova<J->InputIova+J->InputBytes)return CR_INVALID_PARAMETER;
 return CR_SUCCESS;
}
/* Reject all other CTL references to the chosen source/mixer/merge/WB. The
 * root still must retire actual foreign software writers before this scan. */
static CR_STATUS Quiet(CR_MDSS_DPU_WRITEBACK *D){
 static CONST UINT32 Ext[]={0x40,0x70,0xa0,0xb8};UINT32 V;CR_STATUS S;
 for(UINT32 C=0;C<6;++C){UINT32 B=0x15000+C*0x1000;
  S=Read(D,CrMdssWbDpu,B+0xfc,&V);if(S!=CR_SUCCESS)return S;if(V&(1U<<1))return CR_BUSY;
  S=Read(D,CrMdssWbDpu,B+0xec,&V);if(S!=CR_SUCCESS)return S;if(V&(1U<<2))return CR_BUSY;
  S=Read(D,CrMdssWbDpu,B+0xe4,&V);if(S!=CR_SUCCESS)return S;if(V&(1U<<1))return CR_BUSY;
  for(UINT32 L=0;L<6;++L){
   S=Read(D,CrMdssWbDpu,B+(L==5?0x24:L*4),&V);if(S!=CR_SUCCESS)return S;
   if((V&(7U<<21))||((C==2||L==2||L==3)&&(V&~(1U<<24))))return CR_BUSY;
   for(UINT32 E=0;E<COUNT(Ext);++E){S=Read(D,CrMdssWbDpu,B+Ext[E]+L*4,&V);if(S!=CR_SUCCESS)return S;
    if((C==2||L==2||L==3)?V!=0:(E==0?(V&(1U<<18))!=0:E==1?(V&(15U<<12))!=0:FALSE))return CR_BUSY;
   }
  }
 }
 static CONST UINT32 Active[]={0xe4,0xe8,0xec,0xf0,0xf4,0xf8,0xfc};
 for(UINT32 I=0;I<COUNT(Active);++I){S=Match(D,CrMdssWbDpu,CTL+Active[I],FULL,0);if(S!=CR_SUCCESS)return S;}
 S=Match(D,CrMdssWbDpu,CTL+0x1c,1,0);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssWbDpu,CTL+0x30,1,0);if(S!=CR_SUCCESS)return S;
 return Match(D,CrMdssWbDpu,CTL+0x18,FULL,0);
}
CR_STATUS CrMdssDpuWritebackInit(CR_MDSS_DPU_WRITEBACK *D,CONST CR_MDSS_WB_IO *Io){
 if(!D||!Io||Alias(D,sizeof(*D),Io,sizeof(*Io))||!Io->Context||!Io->Read32||!Io->Write32||!Io->Fence||!Io->DelayUs)return CR_INVALID_PARAMETER;
 if(D->Initialized)return CR_BUSY;
 if(Io->Physical[0]!=0xae01000ULL||Io->Physical[1]!=0xaeb0000ULL)return CR_UNSUPPORTED;
 if(Io->Bytes[0]<0x8f000||Io->Bytes[1]<0x3000||Io->Bytes[0]>CR_MAX_UINT64-Io->Physical[0]||Io->Bytes[1]>CR_MAX_UINT64-Io->Physical[1])return CR_BAD_BUFFER_SIZE;
 cr_memset(D,0,sizeof(*D));cr_memcpy(&D->Io,Io,sizeof(*Io));D->Initialized=TRUE;return CR_SUCCESS;
}
static CR_STATUS Configure(CR_MDSS_DPU_WRITEBACK *D){
 UINT32 Size=D->Job.Width|(D->Job.Height<<16);
 const UINT32 Src[][2]={{0,Size},{8,0},{0xc,Size},{0x10,0},{0x14,(UINT32)D->Job.InputIova},{0x18,0},{0x1c,0},{0x20,0},
  {0x24,D->Job.InputStride},{0x28,0},{0x30,0x236ff},{0x34,0x03020001},
  {0x38,(1U<<31)|(D->Job.Reflections?((1U<<13)|(1U<<14)):0)},
  {0x48,0x87},{0x60,0},{0x64,0xffff},{0x6c,1},{0x74,0},{0x78,0},{0xb4,0},
  {0x100,0},{0x104,0},{0x108,Size},{0x110,0},{0x114,0},{0x118,0},{0x120,0},{0x124,0},{0x128,Size},{0x134,9},{0x170,0}};
 const UINT32 Dst[][2]={{0,0x277ff},{4,0},{8,0x03020001},{0xc,(UINT32)D->Job.OutputIova},{0x10,0},{0x14,0},{0x18,0},
  {0x1c,D->Job.OutputStride},{0x20,0},{0x48,0},{0x74,Size},{0x78,0xff},{0x84,0},{0x88,0xffff},{0x90,1},
  {0x98,0},{0x9c,0},{0x2b0,0},{0x2b4,8},{0x2c0,Size},{0x2c4,0}};
 CR_STATUS S;
 for(UINT32 I=0;I<COUNT(Src);++I){S=Program(D,CrMdssWbDpu,DMA+Src[I][0],FULL,Src[I][1]);if(S!=CR_SUCCESS)return S;}
 S=Program(D,CrMdssWbDpu,DMA+0x70,15,0);if(S!=CR_SUCCESS)return S;
 for(UINT32 I=0;I<COUNT(Dst);++I){S=Program(D,CrMdssWbDpu,WB+Dst[I][0],FULL,Dst[I][1]);if(S!=CR_SUCCESS)return S;}
 /* PP2/PP3 share MERGE_3D_1. Even inactive CTLs may retain a persistent 3D
  * mode; Linux always disables both MODE and MUX for BLEND_3D_NONE. */
 S=Program(D,CrMdssWbDpu,MERGE+4,FULL,0);if(S!=CR_SUCCESS)return S;
 S=Program(D,CrMdssWbDpu,MERGE,FULL,0);if(S!=CR_SUCCESS)return S;
 /* Android clone WB can retain crop-enable. Explicitly clear the known
  * enable bit, preserving its unrelated fields and exact original image. */
 S=Program(D,CrMdssWbDpu,WB+0x154,1,0);if(S!=CR_SUCCESS)return S;
 S=Program(D,CrMdssWbDpu,WB+0x150,15,2);if(S!=CR_SUCCESS)return S;
 S=Program(D,CrMdssWbDpu,LM,FULL,0);if(S!=CR_SUCCESS)return S;
 S=Program(D,CrMdssWbDpu,LM+4,FULL,Size);if(S!=CR_SUCCESS)return S;
 const UINT32 Ctrl[][2]={{8,ROUTE},{0x48,0},{0x78,0},{0xa8,0},{0xc0,0},{0x14,0xf0000000},
  {0xe4,0},{0xe8,0},{0xec,4},{0xf0,0},{0xf4,0},{0xf8,0},{0xfc,2}};
 for(UINT32 I=0;I<COUNT(Ctrl);++I){S=Program(D,CrMdssWbDpu,CTL+Ctrl[I][0],FULL,Ctrl[I][1]);if(S!=CR_SUCCESS)return S;}
 /* NRT XIN5 input and XIN6 output: preserve every neighbouring field. The
  * board has no default read OT limit; leave that inherited field untouched. */
 S=Program(D,CrMdssWbVbif,0x160,(7U<<20)|(7U<<24),(3U<<20)|(3U<<24));if(S!=CR_SUCCESS)return S;
 for(UINT32 I=0;I<8;++I){S=Program(D,CrMdssWbVbif,0x550+I*8,(7U<<20)|(7U<<24),(3U<<20)|(3U<<24));if(S!=CR_SUCCESS)return S;
  S=Program(D,CrMdssWbVbif,0x590+I*8,(7U<<20)|(7U<<24),(3U<<20)|(3U<<24));if(S!=CR_SUCCESS)return S;}
 UINT32 Ot=D->Job.PixelsPerSecond<=62208000?2:D->Job.PixelsPerSecond<=124416000?6:D->Job.PixelsPerSecond<=497664000?16:32;
 S=Program(D,CrMdssWbVbif,0xc4,255U<<16,Ot<<16);if(S!=CR_SUCCESS)return S;
 return Program(D,CrMdssWbVbif,0xac,1U<<6,1U<<6);
}
static CR_STATUS Prepared(CR_MDSS_DPU_WRITEBACK *D){
 for(UINT32 I=0;I<D->JournalCount;++I){CR_MDSS_WB_REGISTER *J=&D->Journal[I];
  CR_STATUS S=Match(D,J->Window,J->Offset,J->Mask,J->Desired);if(S!=CR_SUCCESS)return S;
 }
 CR_STATUS S=Match(D,CrMdssWbDpu,CTL+0x18,FULL,FLUSH);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssWbDpu,CTL+0x30,1,0);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssWbDpu,CTL+0x1c,1,0);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssWbVbif,0x200,XINS,0);if(S!=CR_SUCCESS)return S;
 return Match(D,CrMdssWbVbif,0x204,ACKS,0);
}
CR_STATUS CrMdssDpuWritebackPrepare(CR_MDSS_DPU_WRITEBACK *D,CONST CR_MDSS_WB_JOB *J){
 if(!D||!D->Initialized||!J||Alias(D,sizeof(*D),J,sizeof(*J)))return CR_INVALID_PARAMETER;
 if(D->State==CrMdssWbQuarantined)return CR_NOT_READY;
 if(D->Identity){
  if(D->Identity!=J||!SameJob(J,&D->Job)||D->State!=CrMdssWbPrepared)return CR_BUSY;
  CR_STATUS Again=Prepared(D);D->LastError=Again;return Again;
 }
 CR_STATUS S=Geometry(J);if(S!=CR_SUCCESS)return S;
 D->Identity=J;cr_memcpy(&D->Job,J,sizeof(*J));D->CleanupPending=TRUE;
 S=Quiet(D);if(S!=CR_SUCCESS)goto Done;
 S=Halt(D);if(S!=CR_SUCCESS)goto Done;
 S=Configure(D);if(S!=CR_SUCCESS)goto Done;
 /* The owned CTL is not started yet. Its requested shadow transaction must
  * be observed before the synchronized final kick can consume it. */
 S=Write(D,CrMdssWbDpu,CTL+0x108,4);if(S!=CR_SUCCESS)goto Done;
 S=Write(D,CrMdssWbDpu,CTL+0x18,FLUSH);if(S!=CR_SUCCESS)goto Done;
 S=Fence(D);if(S!=CR_SUCCESS)goto Done;
 S=Match(D,CrMdssWbDpu,CTL+0x18,FULL,FLUSH);if(S!=CR_SUCCESS)goto Done;D->FlushObserved=TRUE;
 S=Unhalt(D);if(S!=CR_SUCCESS)goto Done;
 S=Prepared(D);if(S!=CR_SUCCESS)goto Done;
 D->State=CrMdssWbPrepared;
Done:D->LastError=S;return S;
}
CR_STATUS CrMdssDpuWritebackKick(CR_MDSS_DPU_WRITEBACK *D,CONST CR_MDSS_WB_JOB *J,
 CONST VOID *Owner,CONST CR_MDSS_IRQ_TOKEN *T,UINT64 Generation){
 if(!D||!D->Initialized||!J||!Owner||!T||Alias(D,sizeof(*D),T,sizeof(*T)))return CR_INVALID_PARAMETER;
 if(D->State!=CrMdssWbPrepared||D->Identity!=J||!SameJob(J,&D->Job)||!D->FlushObserved)return CR_NOT_READY;
 if(!T->Provider||!T->Incarnation||!T->Serial||T->Mask!=CR_MDSS_IRQ_WB2||!Generation)return CR_INVALID_PARAMETER;
 /* Bounded DIRQL operations only. Caller holds the same actual IRQ lock
  * across ledger Arm and this operation; it drains that admission externally. */
 CR_STATUS S=Prepared(D);if(S!=CR_SUCCESS)goto Done;
 S=Fence(D);if(S!=CR_SUCCESS)goto Done;
 D->IrqIdentity=T;D->IrqOwner=Owner;cr_memcpy(&D->IrqToken,T,sizeof(*T));D->ArmGeneration=Generation;
 D->State=CrMdssWbKickAttempted;D->CleanupPending=TRUE;
 S=Write(D,CrMdssWbDpu,CTL+0x1c,1);if(S!=CR_SUCCESS)goto Done;
 S=Fence(D);if(S==CR_SUCCESS)D->State=CrMdssWbRunning;
Done:D->LastError=S;return S;
}
CR_STATUS CrMdssDpuWritebackComplete(CR_MDSS_DPU_WRITEBACK *D,CONST CR_MDSS_IRQ_EVENT *E){
 if(!D||!D->Initialized||!E||Alias(D,sizeof(*D),E,sizeof(*E)))return CR_INVALID_PARAMETER;
 if(!D->IrqIdentity||!SameToken(D->IrqIdentity,&D->IrqToken)||E->Identity!=D->IrqIdentity||E->Owner!=D->IrqOwner||
    !SameJob(D->Identity,&D->Job)||
    E->Job!=D->Identity||E->Registration!=D->IrqToken.Serial||E->Generation!=D->ArmGeneration||
    !E->Sequence||E->Sequence<=D->IrqSequence||E->Mask!=CR_MDSS_IRQ_WB2)return CR_NOT_READY;
 if(D->State!=CrMdssWbRunning&&D->State!=CrMdssWbKickAttempted)return CR_NOT_READY;
 D->InterruptObserved=TRUE;D->IrqSequence=E->Sequence;
 if(D->State!=CrMdssWbRunning)return CR_NOT_READY;
 D->State=CrMdssWbIrqDone;return CR_SUCCESS;
}
static CR_STATUS Detach(CR_MDSS_DPU_WRITEBACK *D){
 for(UINT32 I=0;I<D->JournalCount;++I){CR_MDSS_WB_REGISTER *J=&D->Journal[I];
  if(J->Window==CrMdssWbDpu&&(J->Offset==CTL+8||J->Offset==CTL+0xfc||J->Offset==CTL+0xec||J->Offset==WB+0x150))J->Restored=FALSE;
 }
 CR_STATUS S=Rmw(D,CrMdssWbDpu,CTL+8,FULL,0);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssWbDpu,CTL+0xfc,FULL,0);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssWbDpu,CTL+0xec,FULL,0);if(S!=CR_SUCCESS)return S;
 return Rmw(D,CrMdssWbDpu,WB+0x150,15,15);
}
static CR_STATUS Reset(CR_MDSS_DPU_WRITEBACK *D){
 D->ResetAttempted=TRUE;D->ResetAcknowledged=FALSE;
 D->Halted=FALSE; /* Reset invalidates all earlier physical drain evidence. */
 CR_STATUS S=Write(D,CrMdssWbDpu,CTL+0x30,1);if(S!=CR_SUCCESS)return S;
 S=Fence(D);if(S!=CR_SUCCESS)return S;
 /* A dropped reset cannot be certified by an already-zero busy register.
  * A hardware reset that finishes before observation safely declines. */
 S=Match(D,CrMdssWbDpu,CTL+0x30,1,1);if(S!=CR_SUCCESS)return S;
 S=Wait(D,CrMdssWbDpu,CTL+0x30,1,0,2000);if(S!=CR_SUCCESS)return S;
 /* Reset completion must also remove the old start/flush transaction. */
 S=Match(D,CrMdssWbDpu,CTL+0x1c,1,0);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssWbDpu,CTL+0x18,FULL,0);if(S==CR_SUCCESS)D->ResetAcknowledged=TRUE;return S;
}
static CR_STATUS Restore(CR_MDSS_DPU_WRITEBACK *D){
 /* Force entries are created first, hence restored last after the fresh halt.
  * All route originals were proved disabled before the first mutation. */
 for(UINT32 N=D->JournalCount;N;--N){CR_MDSS_WB_REGISTER *J=&D->Journal[N-1];
  if(!J->Touched||J->Restored)continue;
  CR_STATUS S=Rmw(D,J->Window,J->Offset,J->Mask,J->Original);if(S!=CR_SUCCESS)return S;J->Restored=TRUE;
 }return CR_SUCCESS;
}
static CR_STATUS Off(CR_MDSS_DPU_WRITEBACK *D){
 if(!D->Halted||!D->ResetAcknowledged)return CR_NOT_READY;
 CR_STATUS S=Quiet(D);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssWbVbif,0x200,XINS,XINS);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssWbVbif,0x204,ACKS,ACKS);return S!=CR_SUCCESS?S:Idle(D);
}
CR_STATUS CrMdssDpuWritebackStop(CR_MDSS_DPU_WRITEBACK *D){
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;if(D->State==CrMdssWbQuarantined)return CR_NOT_READY;
 if(!D->Identity)return CR_SUCCESS;
 D->State=CrMdssWbStopping;D->CleanupPending=TRUE;
 CR_STATUS S;
 if(!D->Touched){D->State=CrMdssWbStopped;D->CleanupPending=FALSE;return CR_SUCCESS;}
 S=Detach(D);if(S!=CR_SUCCESS)goto Done;
 S=Reset(D);if(S!=CR_SUCCESS)goto Done;
 S=Halt(D);if(S!=CR_SUCCESS)goto Done;
 S=Restore(D);if(S!=CR_SUCCESS)goto Done;
 S=Off(D);if(S!=CR_SUCCESS)goto Done;
 D->State=CrMdssWbStopped;D->CleanupPending=FALSE;
Done:D->LastError=S;return S;
}
CR_STATUS CrMdssDpuWritebackCheckStopped(CR_MDSS_DPU_WRITEBACK *D,CONST CR_MDSS_WB_JOB *J,UINT64 Gen){
 if(!D||!D->Initialized||!J)return CR_INVALID_PARAMETER;
 if(D->Identity!=J||!SameJob(J,&D->Job)||Gen!=D->ArmGeneration||D->State!=CrMdssWbStopped||D->CleanupPending)return CR_NOT_READY;
 return D->Touched?Off(D):CR_SUCCESS;
}
CR_STATUS CrMdssDpuWritebackRetire(CR_MDSS_DPU_WRITEBACK *D){
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;if(!D->Identity)return D->State==CrMdssWbIdle?CR_SUCCESS:CR_NOT_READY;
 if(D->State!=CrMdssWbStopped||D->CleanupPending)return CR_BUSY;
 if(D->Touched){CR_STATUS S=Off(D);if(S!=CR_SUCCESS){D->LastError=S;D->CleanupPending=TRUE;return S;}}
 /* Preserve IO/initialization without struct assignment or boot allocator. */
 CR_MDSS_WB_IO Io;cr_memcpy(&Io,&D->Io,sizeof(Io));cr_memset(D,0,sizeof(*D));cr_memcpy(&D->Io,&Io,sizeof(Io));D->Initialized=TRUE;
 return CR_SUCCESS;
}
VOID CrMdssDpuWritebackQuarantine(CR_MDSS_DPU_WRITEBACK *D){
 if(D&&D->Initialized){D->State=CrMdssWbQuarantined;D->CleanupPending=TRUE;D->LastError=CR_NOT_READY;}
}
CR_STATUS CrMdssDpuWritebackUninit(CR_MDSS_DPU_WRITEBACK *D){
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 if(D->Identity||D->State!=CrMdssWbIdle||D->Touched||D->CleanupPending)return CR_BUSY;
 cr_memset(D,0,sizeof(*D));return CR_SUCCESS;
}
