/* Actual cursor core, independently controlled DPU/VBIF latching and faults. MIT */
#include <Library/MdssDpuCursorCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);abort();}}while(0)
#define DMA 0x28000U
#define CTL 0x15000U
#define LM 0x44000U
#define INTF 0x35000U
#define XIN (1U<<9)
/* SM8450 Android SDE uses the upper status field, independently of the
   lower field followed by mainline. Deliberately do not mirror both. */
#define HALT_ACK (1U<<25)
static UINT32 Dpu[0x8f000/4],Vbif[0x3000/4],Posted[0x8f000/4];
static UINT8 Dirty[0x8f000/4];
static UINT32 Phase,Fail,Writes,Reads,Delays,Flushes,FramePending,DropOffset,DropWindow,AckDelay;
static BOOLEAN FailEffects,StuckFlush,StuckHalt,StuckUnhalt,PostedWrites,Lost,FreeRunning;
static CR_STATUS Failure;
static CR_STATUS Step(void){++Phase;return Phase==Fail?Failure:CR_SUCCESS;}
static CR_STATUS Ready(VOID *C){CHECK(C==Dpu);return Lost?CR_NOT_READY:Step();}
static CR_STATUS Read(VOID *C,CR_MDSS_CURSOR_WINDOW W,UINT32 O,UINT32 *V){
 CHECK(C==Dpu&&W<2&&!(O&3)&&O<(W?sizeof(Vbif):sizeof(Dpu)));++Reads;
 CR_STATUS S=Step();if(S!=CR_SUCCESS)return S;*V=(W?Vbif:Dpu)[O/4];return S;
}
static void Effect(CR_MDSS_CURSOR_WINDOW W,UINT32 O,UINT32 V){
 if(DropOffset==O&&DropWindow==(UINT32)W)return;
 if(W&&O==0x200&&((V^Vbif[O/4])&XIN))AckDelay=(V&XIN)?2:5;
 if(!W&&O==CTL+0x18){CHECK(V==((1U<<24)|(1U<<17)|(1U<<6)));FramePending=1;++Flushes;}
 if(PostedWrites&&!W){Posted[O/4]=V;Dirty[O/4]=1;}else(W?Vbif:Dpu)[O/4]=V;
}
static CR_STATUS Write(VOID *C,CR_MDSS_CURSOR_WINDOW W,UINT32 O,UINT32 V){
 CHECK(C==Dpu&&W<2&&!(O&3));++Writes;CR_STATUS S=Step();if(S==CR_SUCCESS||FailEffects)Effect(W,O,V);return S;
}
static CR_STATUS Fence(VOID *C){CHECK(C==Dpu);CR_STATUS S=Step();if(S!=CR_SUCCESS)return S;
 for(UINT32 I=0;I<sizeof(Dirty);++I)if(Dirty[I]){Dpu[I]=Posted[I];Dirty[I]=0;}return S;
}
static CR_STATUS Delay(VOID *C,UINT32 Us){CHECK(C==Dpu&&Us==50);++Delays;CR_STATUS S=Step();if(S!=CR_SUCCESS)return S;
 if(AckDelay)--AckDelay;
 if(!AckDelay&&(Dpu[0x2bc/4]&(1U<<8))){
  if((Vbif[0x200/4]&XIN)&&!StuckHalt)Vbif[0x204/4]|=HALT_ACK;
  else if(!(Vbif[0x200/4]&XIN)&&!StuckUnhalt)Vbif[0x204/4]&=~HALT_ACK;}
 if(FreeRunning)Dpu[(INTF+0xac)/4]++;
 if(FramePending&&!StuckFlush){Dpu[(CTL+0x18)/4]=0;Dpu[(INTF+0xac)/4]++;FramePending=0;}return S;
}
static CR_MDSS_MODE Mode(void){return (CR_MDSS_MODE){.Soc=CR_MDSS_SOC_SM8450,.Width=1920,.Height=1080,.Stride=7680,
 .HSync=44,.HBackPorch=148,.HFrontPorch=88,.VSync=5,.VBackPorch=36,.VFrontPorch=4,.RefreshQ16=60U<<16,.Lanes=4};}
static CR_MDSS_DPU_CURSOR_IMAGE Image(void){return (CR_MDSS_DPU_CURSOR_IMAGE){.DeviceAddress=0x12000000,.Width=64,.Height=32,.X=100,.Y=200,.Visible=TRUE,.Premultiplied=TRUE};}
static CR_MDSS_DPU_CURSOR Fresh(void){
 memset(Dpu,0,sizeof(Dpu));memset(Vbif,0,sizeof(Vbif));memset(Dirty,0,sizeof(Dirty));
 Phase=Fail=Writes=Reads=Delays=Flushes=FramePending=AckDelay=0;DropOffset=DropWindow=0xffffffff;
 FailEffects=StuckFlush=StuckHalt=StuckUnhalt=PostedWrites=Lost=FreeRunning=FALSE;Failure=CR_DEVICE_ERROR;
 CR_MDSS_MODE M=Mode();Dpu[CTL/4]=(1U<<24)|(1U<<18);Dpu[(CTL+0xfc)/4]=1;Dpu[(CTL+0xf4)/4]=2;
 Dpu[0x24000/4]=M.Width|(M.Height<<16);Dpu[(0x24000+0x14)/4]=0x10000000;Dpu[(0x24000+0x30)/4]=0x236ff;
 Dpu[(LM+4)/4]=M.Width|(M.Height<<16);Dpu[LM/4]=(1U<<31)|1;
 Dpu[INTF/4]=Dpu[(INTF+0x26c)/4]=1;Dpu[(INTF+0xac)/4]=0xffffffff;
 Dpu[(DMA+0x70)/4]=0xffffff0f;Dpu[0x2bc/4]=0x41;
 for(UINT32 L=0;L<8;++L){Vbif[(0x554+L*8)/4]=0xa5a5a58a;Vbif[(0x594+L*8)/4]=0xc6c6c69b;}
 Vbif[0x164/4]=0xeeeeeeef;Vbif[0x200/4]=4;Vbif[0x204/4]=4;
 CR_MDSS_DPU_CURSOR_IO Io={.Context=Dpu,.Physical={0xae01000,0xaeb0000},.Bytes={sizeof(Dpu),sizeof(Vbif)},.Ready=Ready,.Read32=Read,.Write32=Write,.Fence=Fence,.DelayUs=Delay};
 UINT64 A[2]={0x12000000,0x12100000};CR_MDSS_DPU_CURSOR D={0};
 CHECK(CrMdssDpuCursorInit(&D,&Io,&M,0x10000000,A,CR_MDSS_DPU_CURSOR_BYTES)==CR_SUCCESS&&!Phase);return D;
}
static void Off(CR_MDSS_DPU_CURSOR *D){
 Fail=0;Lost=FALSE;StuckFlush=StuckHalt=StuckUnhalt=FALSE;DropOffset=0xffffffff;FramePending=0;
 Dpu[INTF/4]=Dpu[(INTF+0x26c)/4]=Dpu[(CTL+0x30)/4]=0;
 CHECK(CrMdssDpuCursorStop(D)==CR_SUCCESS&&!D->Visible&&D->Halted&&!D->CleanupPending);
 CHECK(CrMdssDpuCursorRetire(D)==CR_SUCCESS&&!D->Initialized);
}
int main(void){
 CR_MDSS_DPU_CURSOR D=Fresh();CR_MDSS_DPU_CURSOR_IMAGE I=Image();
 CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS&&D.Visible&&!D.CleanupPending&&!D.Halted);
 CHECK(Dpu[DMA/4]==(64|(32U<<16))&&Dpu[(DMA+0x10)/4]==(100|(200U<<16))&&Dpu[(DMA+0x14)/4]==I.DeviceAddress);
 CHECK(Dpu[(DMA+0x30)/4]==0x237ff&&Dpu[(DMA+0x70)/4]==0xffffff00&&Dpu[(LM+0x20)/4]==0x600&&Dpu[(LM+0x24)/4]==0xff0000);
 CHECK(Dpu[LM/4]==((1U<<31)|3)&&Dpu[(CTL+0x70)/4]==2&&Dpu[(CTL+0xfc)/4]==5&&Dpu[0x2bc/4]==0x41&&!(Vbif[0x204/4]&HALT_ACK));
 const UINT32 Pri[8]={3,3,4,4,5,5,6,6};
 for(UINT32 L=0;L<8;++L){CHECK(Vbif[(0x554+L*8)/4]==((0xa5a5a58a&~0x70U)|(Pri[L]<<4)));CHECK(Vbif[(0x594+L*8)/4]==((0xc6c6c69b&~0x70U)|(Pri[L]<<4)));}
 CHECK(Vbif[0x164/4]==((0xeeeeeeef&~0x70U)|0x30));
 I.DeviceAddress=0x12100000;I.X=-10;I.Y=-5;I.Premultiplied=FALSE;
 CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS&&D.Visible);
 CHECK(Dpu[DMA/4]==(54|(27U<<16))&&Dpu[(DMA+8)/4]==(10|(5U<<16))&&!Dpu[(DMA+0x10)/4]&&Dpu[(LM+0x20)/4]==0x602);
 I.Transform=3;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS&&!Dpu[(DMA+8)/4]);
 I.X=1910;I.Y=1070;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS&&Dpu[DMA/4]==(10|(10U<<16))&&Dpu[(DMA+8)/4]==(54|(22U<<16)));
 I.X=(-2147483647-1);I.Y=2147483647;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS&&!D.Visible&&D.Halted);
 CHECK(CrMdssDpuCursorRetire(&D)==CR_SUCCESS);
 D=Fresh();PostedWrites=TRUE;I=Image();CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS);Off(&D);
 D=Fresh();I=Image();CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS);UINT32 Max=Phase,Faults=0;
 for(UINT32 F=1;F<=Max;++F)for(UINT32 K=0;K<3;++K)for(UINT32 Effect=0;Effect<2;++Effect){
  D=Fresh();I=Image();Fail=F;Failure=K==0?CR_DEVICE_ERROR:K==1?(CR_STATUS)1:(CR_STATUS)0x103;FailEffects=Effect;
  CHECK(CrMdssDpuCursorSet(&D,&I)!=CR_SUCCESS);Fail=0;UINT32 N=Writes;
  if(D.CleanupPending)CHECK(CrMdssDpuCursorSet(&D,&I)==CR_NOT_READY&&Writes==N);
  else CHECK(!D.Visible);Off(&D);++Faults;
 }
 const UINT32 Foreign[]={CTL+0x1000+0xfc,CTL+0x1070,CTL+0x74,CTL+0x40,CTL+0xa0,CTL+0x70};
 const UINT32 Value[]={4,2,2,1,1,0x20};
 for(UINT32 F=0;F<sizeof(Foreign)/sizeof(*Foreign);++F){D=Fresh();Dpu[Foreign[F]/4]=Value[F];I=Image();CHECK(CrMdssDpuCursorSet(&D,&I)!=CR_SUCCESS&&!Writes);}
 D=Fresh();I=Image();I.DeviceAddress=0xdead0000;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_BUSY&&!Writes);
 I=Image();I.Width=257;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_INVALID_PARAMETER&&!Writes);
 I=Image();I.Transform=1;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_INVALID_PARAMETER&&!Writes);
 D=Fresh();I=Image();StuckHalt=TRUE;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_TIMEOUT&&D.CleanupPending);Off(&D);
 /* Low-field-only status must not authorize retiring DMA-visible buffers. */
 D=Fresh();I=Image();CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS);
 CHECK(CrMdssDpuCursorStop(&D)==CR_SUCCESS);
 Vbif[0x204/4]=(Vbif[0x204/4]&~HALT_ACK)|XIN;
 CHECK(CrMdssDpuCursorRetire(&D)==CR_BUSY&&D.Initialized&&D.Halted);
 Vbif[0x204/4]|=HALT_ACK;CHECK(CrMdssDpuCursorRetire(&D)==CR_SUCCESS);
 /* A stale low status must not block a fresh upper ACK transition. */
 D=Fresh();I=Image();Vbif[0x204/4]|=XIN;
 CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS&&D.Visible);Off(&D);
 D=Fresh();I=Image();StuckUnhalt=TRUE;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_TIMEOUT&&D.CleanupPending&&D.ForceTouched&&!D.Halted);
 /* The retained ACK must go low before cleanup can certify a new halt. */
 UINT32 UnhaltWrites=Writes;CHECK(CrMdssDpuCursorStop(&D)==CR_TIMEOUT&&Writes>UnhaltWrites&&D.CleanupPending);Off(&D);
 D=Fresh();I=Image();StuckFlush=TRUE;CHECK(CrMdssDpuCursorSet(&D,&I)==CR_TIMEOUT&&D.CleanupPending);Off(&D);
 const UINT32 Drop[]={DMA,DMA+8,DMA+0xc,DMA+0x10,DMA+0x14,DMA+0x18,DMA+0x1c,DMA+0x20,
  DMA+0x24,DMA+0x28,DMA+0x30,DMA+0x34,DMA+0x38,DMA+0x60,DMA+0x64,DMA+0x6c,DMA+0x70,
  DMA+0x74,DMA+0x78,DMA+0xb4,DMA+0x100,DMA+0x104,DMA+0x108,DMA+0x110,DMA+0x114,
  DMA+0x118,DMA+0x120,DMA+0x124,DMA+0x128,DMA+0x134,DMA+0x170,LM+0x24,CTL+0x70,CTL+0xfc};
 for(UINT32 F=0;F<sizeof(Drop)/sizeof(*Drop);++F){D=Fresh();I=Image();
  if(Drop[F]>=DMA&&Drop[F]<=DMA+0x170&&Drop[F]!=DMA+0x70)Dpu[Drop[F]/4]=0xdeadbeef;
  DropWindow=0;DropOffset=Drop[F];CHECK(CrMdssDpuCursorSet(&D,&I)!=CR_SUCCESS&&D.CleanupPending);Off(&D);}
 /* Independent frame progress cannot certify an unlatched flush. */
 D=Fresh();I=Image();DropWindow=0;DropOffset=CTL+0x18;FreeRunning=TRUE;
 CHECK(CrMdssDpuCursorSet(&D,&I)!=CR_SUCCESS&&D.CleanupPending&&!D.Visible);Off(&D);
 /* Every owned XIN field is verified without disturbing a neighbor. */
 for(UINT32 F=0;F<17;++F){D=Fresh();I=Image();DropWindow=1;
  DropOffset=F==16?0x164:(F<8?0x554+F*8:0x594+(F-8)*8);
  CHECK(CrMdssDpuCursorSet(&D,&I)!=CR_SUCCESS&&D.CleanupPending);Off(&D);}
 D=Fresh();I=Image();CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS);Phase=0;
 CHECK(CrMdssDpuCursorStop(&D)==CR_SUCCESS);Max=Phase;
 for(UINT32 F=1;F<=Max;++F){D=Fresh();I=Image();CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS);Phase=0;Fail=F;
  CHECK(CrMdssDpuCursorStop(&D)!=CR_SUCCESS&&D.Initialized);Fail=0;CHECK(CrMdssDpuCursorStop(&D)==CR_SUCCESS&&D.Halted&&!D.Visible);CHECK(CrMdssDpuCursorRetire(&D)==CR_SUCCESS);}
 D=Fresh();I=Image();CHECK(CrMdssDpuCursorSet(&D,&I)==CR_SUCCESS);Lost=TRUE;UINT32 N=Writes;
 CHECK(CrMdssDpuCursorStop(&D)==CR_NOT_READY&&Writes==N&&D.Visible);Lost=FALSE;Off(&D);
 D=Fresh();CR_MDSS_DPU_CURSOR Saved=D;CR_MDSS_MODE M=Mode();UINT64 A[2]={0x12000000,0x12100000};
 CHECK(CrMdssDpuCursorInit(&D,&D.Io,&M,0x10000000,A,CR_MDSS_DPU_CURSOR_BYTES)==CR_BUSY&&!memcmp(&D,&Saved,sizeof(D)));
 D=(CR_MDSS_DPU_CURSOR){0};CR_MDSS_DPU_CURSOR_IO Bad=Saved.Io;Bad.Physical[1]=0xaeb1000;
 CHECK(CrMdssDpuCursorInit(&D,&Bad,&M,0x10000000,A,CR_MDSS_DPU_CURSOR_BYTES)==CR_UNSUPPORTED&&!D.Initialized);
 Bad=Saved.Io;Bad.Bytes[1]=0x1000;CHECK(CrMdssDpuCursorInit(&D,&Bad,&M,0x10000000,A,CR_MDSS_DPU_CURSOR_BYTES)==CR_BAD_BUFFER_SIZE);
 printf("Actual SM8450 DMA2 cursor source/QoS/alpha/clip/route + halt drain: %u acquisition callback failures, %u stop failures PASS\n",Faults,Max);return 0;
}
