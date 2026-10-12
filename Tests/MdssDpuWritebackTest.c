/* Actual portable WB2 and IRQ ledger; controlled shadow/latch/DMA model. MIT */
#include <Library/MdssDpuWritebackCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
#define DMA 0x26000U
#define CTL 0x17000U
#define WB 0x65000U
#define XINS 0x60U
#define ACKS (XINS<<16)
#define FULL 0xffffffffU
#define CHECK(x) do{++Checks;if(!(x)){fprintf(stderr,"line %d: %s phase %u fail %u\n",__LINE__,#x,Phase,Fail);abort();}}while(0)
static UINT64 Checks,FaultCases;
static UINT32 Dpu[0x8f000/4],Vbif[0x3000/4],OldDpu[0x8f000/4],OldVbif[0x3000/4];
static UINT32 Input[1024],Output[1024],Before[1024];
static UINT32 Phase,Fail,Writes,Delays,Parent,Pending,DropOffset,DropWindow;
static CR_STATUS Failure;
static BOOLEAN CommitError,Lost,InKick,StuckHigh,StuckLow,Busy,StuckReset,EarlyFlush;
static CR_MDSS_DPU_WRITEBACK Core;
static CR_MDSS_IRQ_CORE Irq;
static CR_MDSS_IRQ_TOKEN Token;
static CR_MDSS_WB_JOB Job;
static CR_MDSS_IRQ_EVENT Event;
static UINT32 Owner,IrqOwner;
static CR_STATUS Step(VOID){++Phase;return Lost?CR_NOT_READY:Phase==Fail?Failure:CR_SUCCESS;}
static void Advance(VOID){
 if(Dpu[(CTL+0x30)/4]&&!StuckReset){Dpu[(CTL+0x30)/4]=0;Dpu[(CTL+0x1c)/4]=0;Dpu[(CTL+0x18)/4]=0;Dpu[(CTL+0x108)/4]=0;Pending=0;}
 UINT32 Force=(Dpu[0x2b4/4]&(1U<<8))&&(Dpu[0x2bc/4]&(1U<<16));
 if(Force){
  if((Vbif[0x200/4]&XINS)==XINS&&!StuckLow)Vbif[0x204/4]|=ACKS;
  if(!(Vbif[0x200/4]&XINS)&&!StuckHigh)Vbif[0x204/4]&=~ACKS;
 }
 if(EarlyFlush)Dpu[(CTL+0x18)/4]=0;
}
static CR_STATUS Read(VOID *C,CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 *V){
 CHECK(C==Dpu&&W<2&&!(O&3)&&O<(W?sizeof(Vbif):sizeof(Dpu)));
 CR_STATUS S=Step();*V=(W?Vbif:Dpu)[O/4];return S;
}
static void Effect(CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 V){
 if(W==DropWindow&&O==DropOffset)return;
 if(!W&&O==CTL+0x1c&&V==1){
  CHECK(InKick);CHECK(Dpu[(CTL+8)/4]==0x1200000&&Dpu[(CTL+0xfc)/4]==2&&Dpu[(CTL+0xec)/4]==4);
  CHECK(Dpu[(WB+0x150)/4]%16==2&&Dpu[(DMA+0x30)/4]==0x236ff&&Dpu[(WB)/4]==0x277ff);
  CHECK(!(Vbif[0x204/4]&ACKS)&&Irq.Slots[0].Armed&&Irq.Slots[0].Job==&Job);
  UINT32 Wd=Dpu[DMA/4]&0xffff,H=Dpu[DMA/4]>>16,Is=Dpu[(DMA+0x24)/4]/4,Os=Dpu[(WB+0x1c)/4]/4;
  CHECK(Wd<=5&&H<=3&&Dpu[(DMA+0x14)/4]==(UINT32)Job.InputIova&&Dpu[(WB+0xc)/4]==(UINT32)Job.OutputIova);
  BOOLEAN Flip=(Dpu[(DMA+0x38)/4]&((1U<<13)|(1U<<14)))!=0;
  for(UINT32 Y=0;Y<H;++Y)for(UINT32 X=0;X<Wd;++X)Output[Y*Os+X]=Input[(Flip?H-1-Y:Y)*Is+(Flip?Wd-1-X:X)]|0xff000000;
  Pending=1;Dpu[(CTL+0x18)/4]=0;
 }
 if(!W&&O==CR_MDSS_IRQ_TOP0_CLEAR){Dpu[CR_MDSS_IRQ_TOP0_STATUS/4]&=~V;Parent=0;return;}
 (W?Vbif:Dpu)[O/4]=V;
}
static CR_STATUS Write(VOID *C,CR_MDSS_WB_WINDOW W,UINT32 O,UINT32 V){
 CHECK(C==Dpu&&W<2&&!(O&3));++Writes;CR_STATUS S=Step();if(S==CR_SUCCESS||CommitError)Effect(W,O,V);return S;
}
static CR_STATUS Fence(VOID *C){CHECK(C==Dpu);CR_STATUS S=Step();
 if(Pending){Dpu[(CTL+0x1c)/4]=0;Dpu[CR_MDSS_IRQ_TOP0_STATUS/4]|=CR_MDSS_IRQ_WB2;Parent=1;Pending=0;}
 if(EarlyFlush)Dpu[(CTL+0x18)/4]=0;return S;
}
static CR_STATUS Delay(VOID *C,UINT32 Us){CHECK(C==Dpu&&Us==50&&!InKick);++Delays;CR_STATUS S=Step();if(S==CR_SUCCESS)Advance();return S;}
static CR_STATUS IrqRead(VOID *C,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 *V){
 if(W==CrMdssIrqParent){CHECK(O==0x10);*V=Parent;return CR_SUCCESS;}
 CHECK(W==CrMdssIrqDpu);return Read(C,CrMdssWbDpu,O,V);
}
static CR_STATUS IrqWrite(VOID *C,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 V){CHECK(W==CrMdssIrqDpu);return Write(C,CrMdssWbDpu,O,V);}
static CR_MDSS_WB_IO Io(VOID){CR_MDSS_WB_IO V={Dpu,{0xae01000,0xaeb0000},{sizeof(Dpu),sizeof(Vbif)},Read,Write,Fence,Delay};return V;}
static void ResetModel(VOID){
 memset(&Core,0,sizeof(Core));memset(&Irq,0,sizeof(Irq));memset(&Token,0,sizeof(Token));memset(&Event,0,sizeof(Event));
 memset(Dpu,0,sizeof(Dpu));memset(Vbif,0,sizeof(Vbif));
 /* Inactive blocks retain nonzero ordinary configuration; shared neighbours
  * include the live cursor force bit and unrelated XIN halt/QoS fields. */
 Dpu[0x2b4/4]=0x80000000;Dpu[0x2bc/4]=1U<<8;
 Dpu[(WB+0x150)/4]=0xface000f;Dpu[(DMA+0x70)/4]=0x12340000;
 Dpu[(DMA+0x48)/4]=0x81;Dpu[(WB+0x84)/4]=0x5555;
 Dpu[0x4f000/4]=0x5;Dpu[0x4f004/4]=0x3;Dpu[(WB+0x154)/4]=0xabc00001;
 for(UINT32 O=0x550;O<=0x5c8;O+=8)Vbif[O/4]=0x8a8aaaaa;
 Vbif[0x200/4]=1U<<9;Vbif[0x204/4]=1U<<9;Vbif[0x160/4]=0xcccccccc;Vbif[0xc4/4]=0xfeed1234;Vbif[0xac/4]=0x55;
 memcpy(OldDpu,Dpu,sizeof(Dpu));memcpy(OldVbif,Vbif,sizeof(Vbif));
 for(UINT32 I=0;I<1024;++I){Input[I]=0xff000000|I;Output[I]=Before[I]=0x98765432;}
 CR_MDSS_WB_JOB J={&Owner,0x14000000,0x10000000,4096,0x2b00000,5,3,32,32,0,1000000};Job=J;
 Phase=Fail=Writes=Delays=Parent=Pending=0;DropWindow=DropOffset=FULL;
 Failure=CR_TIMEOUT;CommitError=Lost=InKick=StuckHigh=StuckLow=Busy=StuckReset=EarlyFlush=FALSE;
 CR_MDSS_WB_IO I=Io();CHECK(CrMdssDpuWritebackInit(&Core,&I)==CR_SUCCESS);
 CR_MDSS_IRQ_IO Q={Dpu,IrqRead,IrqWrite,Fence};CHECK(CrMdssIrqInit(&Irq,&Q,99)==CR_SUCCESS);
 BOOLEAN Owned=FALSE;CHECK(CrMdssIrqRegister(&Irq,&IrqOwner,CR_MDSS_IRQ_WB2,&Token,&Owned)==CR_SUCCESS&&Owned);
 Phase=Writes=0;
}
static CR_STATUS Kick(VOID){UINT64 Gen=0;CHECK(CrMdssIrqArm(&Irq,&IrqOwner,&Token,&Job,&Gen)==CR_SUCCESS);
 Phase=0;InKick=TRUE;CR_STATUS S=CrMdssDpuWritebackKick(&Core,&Job,&IrqOwner,&Token,Gen);InKick=FALSE;return S;}
static CR_STATUS Capture(VOID){BOOLEAN H=FALSE,P=FALSE;CHECK(CrMdssIrqCapture(&Irq,&H,&P)==CR_SUCCESS);
 if(!H){CHECK(!P);return CR_NOT_FOUND;}CHECK(P);CHECK(CrMdssIrqTake(&Irq,&IrqOwner,&Token,&Event)==CR_SUCCESS);
 return CrMdssDpuWritebackComplete(&Core,&Event);
}
static void Withdraw(VOID){
 CHECK(CrMdssIrqDisarm(&Irq,&IrqOwner,&Token)==CR_SUCCESS);CHECK(CrMdssIrqDiscard(&Irq,&IrqOwner,&Token)==CR_SUCCESS);
 if(Irq.Slots[0].Job)CHECK(CrMdssIrqRetireJob(&Irq,&IrqOwner,&Token,&Job)==CR_SUCCESS);
 CHECK(CrMdssIrqUnregister(&Irq,&IrqOwner,&Token)==CR_SUCCESS);
}
static void CheckRestored(VOID){
 for(UINT32 I=0;I<Core.JournalCount;++I){CR_MDSS_WB_REGISTER *J=&Core.Journal[I];
  CHECK(J->Restored);CHECK(((J->Window?Vbif:Dpu)[J->Offset/4]&J->Mask)==J->Original);
  CHECK((J->Original&J->Mask)==((J->Window?OldVbif:OldDpu)[J->Offset/4]&J->Mask));
  CHECK(J->Window!=CrMdssWbDpu||(J->Offset!=CTL+0x18&&J->Offset!=CTL+0x1c&&J->Offset!=CTL+0x30&&J->Offset!=CTL+0x108));
  CHECK(J->Window!=CrMdssWbVbif||(J->Offset!=0x200&&J->Offset!=0x204));
 }
 CHECK((Vbif[0x200/4]&XINS)==XINS&&(Vbif[0x204/4]&ACKS)==ACKS);
 CHECK((Vbif[0x200/4]&~XINS)==(OldVbif[0x200/4]&~XINS));
 CHECK(Dpu[0x2bc/4]==OldDpu[0x2bc/4]&&Dpu[0x2b4/4]==OldDpu[0x2b4/4]);
}
static void Retire(VOID){Fail=0;DropOffset=DropWindow=FULL;CommitError=FALSE;StuckReset=StuckHigh=StuckLow=EarlyFlush=FALSE;
 CHECK(CrMdssDpuWritebackStop(&Core)==CR_SUCCESS);if(Core.Touched)CheckRestored();Withdraw();
 CHECK(CrMdssDpuWritebackCheckStopped(&Core,&Job,Core.ArmGeneration)==CR_SUCCESS);
 CR_MDSS_WB_JOB Copy=Job;CHECK(CrMdssDpuWritebackCheckStopped(&Core,&Copy,Core.ArmGeneration)==CR_NOT_READY);
 CHECK(CrMdssDpuWritebackCheckStopped(&Core,&Job,Core.ArmGeneration+1)==CR_NOT_READY);
 CHECK(CrMdssDpuWritebackRetire(&Core)==CR_SUCCESS);CHECK(!Core.Identity&&Core.State==CrMdssWbIdle);
 CHECK(CrMdssDpuWritebackUninit(&Core)==CR_SUCCESS);CHECK(CrMdssIrqUninit(&Irq)==CR_SUCCESS);
}
static void Nominal(UINT32 Flip){
 ResetModel();Job.Reflections=Flip;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(Core.JournalCount<=CR_MDSS_WB_JOURNAL_MAX);
 CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CR_MDSS_WB_JOB Copy=Job;CHECK(CrMdssDpuWritebackPrepare(&Core,&Copy)==CR_BUSY);
 CHECK(CrMdssDpuWritebackRetire(&Core)==CR_BUSY);CHECK(Kick()==CR_SUCCESS);CHECK(Core.State==CrMdssWbRunning);
 for(UINT32 Y=0;Y<3;++Y)for(UINT32 X=0;X<8;++X)CHECK(Output[Y*8+X]==(X<5?Input[(Flip?2-Y:Y)*8+(Flip?4-X:X)]:Before[Y*8+X]));
 CHECK(Capture()==CR_SUCCESS&&Core.State==CrMdssWbIrqDone);CHECK(CrMdssDpuWritebackComplete(&Core,&Event)==CR_NOT_READY);
 CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_BUSY);Retire();
}
static void GeometryTests(VOID){
 ResetModel();UINT32 BeforeIo=Phase;CR_MDSS_WB_JOB Good=Job;
#define BAD(field,value) do{Job=Good;Job.field=(value);CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)!=CR_SUCCESS);CHECK(Phase==BeforeIo&&!Core.Identity);}while(0)
 BAD(Owner,NULL);BAD(Width,0);BAD(Width,2561);BAD(Width,4097);BAD(Height,0);BAD(Height,65536);BAD(InputStride,19);BAD(InputStride,65536);
 BAD(OutputStride,16);BAD(OutputStride,65535);BAD(InputIova,0);BAD(InputIova,1);BAD(OutputIova,0x100000001ULL);
 BAD(InputIova,0x100000000ULL);BAD(InputBytes,4097);BAD(InputBytes,0);BAD(OutputBytes,0xfffff000ULL);
 BAD(OutputIova,Good.InputIova);BAD(Reflections,1);BAD(PixelsPerSecond,0);
 Job=Good;Job.Height=65535;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)!=CR_SUCCESS&&Phase==BeforeIo);
#undef BAD
 Job=Good;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(Core.Job.OutputBytes==0x2b00000);Retire();
 ResetModel();Job.Width=2560;Job.Height=1;Job.InputStride=Job.OutputStride=10240;Job.InputBytes=12288;
 CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);Retire();
 ResetModel();CR_MDSS_WB_IO I=Io();CHECK(CrMdssDpuWritebackInit(&Core,&I)==CR_BUSY);Core.Initialized=FALSE;
 I.Physical[0]++;CHECK(CrMdssDpuWritebackInit(&Core,&I)==CR_UNSUPPORTED);
 I=Io();I.Bytes[1]=0x2000;CHECK(CrMdssDpuWritebackInit(&Core,&I)==CR_BAD_BUFFER_SIZE);
 CHECK(CrMdssDpuWritebackInit(&Core,(CR_MDSS_WB_IO *)&Core)==CR_INVALID_PARAMETER);
}
static void FailureMatrix(VOID){
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);UINT32 Prep=Phase;Retire();
 for(UINT32 Kind=0;Kind<3;++Kind)for(UINT32 Effects=0;Effects<2;++Effects)for(UINT32 F=1;F<=Prep;++F){
  ResetModel();Failure=Kind==0?CR_TIMEOUT:Kind==1?(CR_STATUS)1:CR_NOT_READY;CommitError=(BOOLEAN)Effects;Fail=F;
  CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)!=CR_SUCCESS);CHECK(Core.Identity==&Job&&Core.CleanupPending);
  CHECK(CrMdssDpuWritebackRetire(&Core)==CR_BUSY);CR_MDSS_WB_JOB Other=Job;CHECK(CrMdssDpuWritebackPrepare(&Core,&Other)==CR_BUSY);
  Retire();++FaultCases;
 }
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(Kick()==CR_SUCCESS);CHECK(Capture()==CR_SUCCESS);Phase=0;
 CHECK(CrMdssDpuWritebackStop(&Core)==CR_SUCCESS);UINT32 Stop=Phase;Withdraw();CHECK(CrMdssDpuWritebackRetire(&Core)==CR_SUCCESS);
 for(UINT32 Kind=0;Kind<3;++Kind)for(UINT32 Effects=0;Effects<2;++Effects)for(UINT32 F=1;F<=Stop;++F){
  ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(Kick()==CR_SUCCESS);CHECK(Capture()==CR_SUCCESS);
  Phase=0;Fail=F;Failure=Kind==0?CR_TIMEOUT:Kind==1?(CR_STATUS)1:CR_NOT_READY;CommitError=(BOOLEAN)Effects;
  CHECK(CrMdssDpuWritebackStop(&Core)!=CR_SUCCESS);CHECK(Core.Identity==&Job&&Core.CleanupPending&&Core.State==CrMdssWbStopping);
  CHECK(CrMdssDpuWritebackRetire(&Core)==CR_BUSY);Retire();++FaultCases;
 }
 printf("acquire phases=%u stop phases=%u faults=%llu\n",Prep,Stop,(unsigned long long)FaultCases);
}
static void KickFaults(VOID){
 /* A final enabling/START write can commit before failure, yet IRQ completion
  * must not erase the submission uncertainty. Exact real ledger is used. */
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(Kick()==CR_SUCCESS);UINT32 Phases=Phase;Retire();
 for(UINT32 Kind=0;Kind<2;++Kind)for(UINT32 Effects=0;Effects<2;++Effects)for(UINT32 F=1;F<=Phases;++F){
  ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);
  /* Arm first without injecting a ledger fault; inject only the typed kick. */
  UINT64 Gen=0;CHECK(CrMdssIrqArm(&Irq,&IrqOwner,&Token,&Job,&Gen)==CR_SUCCESS);
  Phase=0;Fail=F;Failure=Kind?(CR_STATUS)1:CR_TIMEOUT;CommitError=(BOOLEAN)Effects;InKick=TRUE;
  CHECK(CrMdssDpuWritebackKick(&Core,&Job,&IrqOwner,&Token,Gen)!=CR_SUCCESS);InKick=FALSE;Fail=0;
  CHECK(Core.State!=CrMdssWbRunning&&Core.State!=CrMdssWbIrqDone);CHECK(Capture()!=CR_SUCCESS);Retire();
  ++FaultCases;
 }
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);DropWindow=0;DropOffset=CTL+0x1c;
 CHECK(Kick()==CR_SUCCESS);CHECK(Capture()==CR_NOT_FOUND);CHECK(Core.State==CrMdssWbRunning);CHECK(!Core.InterruptObserved);Retire();
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(Kick()==CR_SUCCESS);
 BOOLEAN H,P;CHECK(CrMdssIrqCapture(&Irq,&H,&P)==CR_SUCCESS);CHECK(CrMdssIrqTake(&Irq,&IrqOwner,&Token,&Event)==CR_SUCCESS);
 CR_MDSS_IRQ_EVENT Good=Event;
#define BAD_EVENT(field,value) do{Event=Good;Event.field=(value);CHECK(CrMdssDpuWritebackComplete(&Core,&Event)!=CR_SUCCESS);CHECK(!Core.InterruptObserved);}while(0)
 BAD_EVENT(Identity,NULL);BAD_EVENT(Owner,&Owner);BAD_EVENT(Job,&Core);BAD_EVENT(Registration,0);BAD_EVENT(Generation,0);BAD_EVENT(Sequence,0);BAD_EVENT(Mask,1);
#undef BAD_EVENT
 Event=Good;Token.Incarnation++;CHECK(CrMdssDpuWritebackComplete(&Core,&Event)!=CR_SUCCESS);Token.Incarnation--;
 CHECK(CrMdssDpuWritebackComplete(&Core,&Event)==CR_SUCCESS);Retire();
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);Dpu[(DMA+0x14)/4]+=4096;
 CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)!=CR_SUCCESS);CHECK(Kick()!=CR_SUCCESS);CHECK(!Core.IrqIdentity);Retire();
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);Lost=TRUE;UINT32 N=Writes;
 CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_NOT_READY&&Writes==N);Lost=FALSE;Retire();
}
static void PhysicalFaults(VOID){
 ResetModel();Vbif[0x204/4]|=ACKS;StuckHigh=TRUE;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_TIMEOUT&&!Core.Halted);Retire();
 ResetModel();Vbif[0x204/4]|=XINS;StuckLow=TRUE;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_TIMEOUT&&!Core.Halted);Retire();
 ResetModel();StuckLow=TRUE;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_TIMEOUT);Retire();
 ResetModel();Dpu[0x3bc/4]|=1U<<20;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)!=CR_SUCCESS);Dpu[0x3bc/4]=0;Retire();
 ResetModel();EarlyFlush=TRUE;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)!=CR_SUCCESS&&!Core.FlushObserved);Retire();
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(Kick()==CR_SUCCESS);StuckReset=TRUE;
 CHECK(CrMdssDpuWritebackStop(&Core)==CR_TIMEOUT&&!Core.ResetAcknowledged);Retire();
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);DropWindow=0;DropOffset=CTL+0x30;
 CHECK(CrMdssDpuWritebackStop(&Core)!=CR_SUCCESS&&!Core.ResetAcknowledged);Retire();
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(CrMdssDpuWritebackStop(&Core)==CR_SUCCESS);
 Vbif[0x204/4]&=~ACKS;CHECK(CrMdssDpuWritebackRetire(&Core)!=CR_SUCCESS&&Core.Identity==&Job);Retire();
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);CHECK(Kick()==CR_SUCCESS);UINT32 N=Phase;
 Lost=TRUE;CrMdssDpuWritebackQuarantine(&Core);CHECK(CrMdssDpuWritebackStop(&Core)==CR_NOT_READY);
 CHECK(CrMdssDpuWritebackRetire(&Core)==CR_BUSY&&CrMdssDpuWritebackUninit(&Core)==CR_BUSY&&Phase==N);
 CHECK(Core.Identity==&Job&&Token.Provider==&Irq&&Core.CleanupPending);
}
static void DroppedConfiguration(VOID){
 ResetModel();CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_SUCCESS);
 CR_MDSS_WB_REGISTER Entries[CR_MDSS_WB_JOURNAL_MAX];UINT32 N=Core.JournalCount;memcpy(Entries,Core.Journal,sizeof(Entries));Retire();
 for(UINT32 I=0;I<N;++I){
  ResetModel();CR_MDSS_WB_REGISTER *J=&Entries[I];
  /* A dropped write is only observable when changing a value. */
  UINT32 *Reg=&(J->Window?Vbif:Dpu)[J->Offset/4];
  if((*Reg&J->Mask)==J->Desired)*Reg^=J->Mask;
  memcpy(OldDpu,Dpu,sizeof(Dpu));memcpy(OldVbif,Vbif,sizeof(Vbif));
  DropWindow=J->Window;DropOffset=J->Offset;
  CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)!=CR_SUCCESS);Retire();++FaultCases;
 }
 /* Whole quiet scan detects a live candidate in every foreign CTL/LM. */
 for(UINT32 C=0;C<6;++C)for(UINT32 L=0;L<6;++L){ResetModel();UINT32 O=0x15000+C*0x1000+(L==5?0x24:L*4);
  Dpu[O/4]=1U<<21;CHECK(CrMdssDpuWritebackPrepare(&Core,&Job)==CR_BUSY&&!Core.Touched);Retire();}
}
int main(void){Nominal(0);Nominal(3);GeometryTests();FailureMatrix();KickFaults();PhysicalFaults();DroppedConfiguration();
 printf("PASS SM8450 WB2 actual core+IRQ: %llu checks, %llu fault cases\n",(unsigned long long)Checks,(unsigned long long)FaultCases);return 0;}
