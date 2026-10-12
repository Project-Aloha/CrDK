/* Compose real QRTR/audio PD and real named GLINK over simulated SMEM. MIT */
#define main ExistingNamedPeerMain
#include "GlinkServicePeerTest.c"
#undef main
#define QRTR_AUDIO_COMPOSED
#include "QrtrAudioTest.c"

static CR_STATUS QG_Last;
static CR_QRTR_AUDIO_PD_INFORMATION QG_Observed;
static UINTN QG_Calls;
static BOOLEAN QG_ReleaseTxOnDown;
static BOOLEAN QG_QuiesceOnDown;
static UINT32 QG_HeadAtDown;
static CR_STATUS QG_Send(VOID *Context,CONST VOID *B,UINTN N){
  QCHECK(Context==&QA_R);CR_STATUS S=CrGlinkSendQrtr(&Link,B,N);
  if(S==CR_SUCCESS)QCHECK(QA_Send(Context,B,N)==CR_SUCCESS);return S;
}
static CR_STATUS QG_Receive(VOID *Context,CONST UINT8 *B,UINTN N){
  QCHECK(Context==&Link);++QG_Calls;QG_Last=CrQrtrReceive(&QA_R,B,N);
  QCHECK(CrQrtrGetAudioPd(&QA_R,&QG_Observed)==CR_SUCCESS);
  if(QG_ReleaseTxOnDown&&!QG_Observed.Ready){
    /* Peer consumption can advance while AP handles RX. This exposes the
     * existing Poll-after-RX TX gap; no graph retirement is inferred. */
    QG_HeadAtDown=Descriptor[1];Descriptor[0]=Descriptor[1];QG_ReleaseTxOnDown=FALSE;
    if(QG_QuiesceOnDown){
      UINT32 Before[8];memcpy(Before,Descriptor,sizeof(Before));
      QCHECK(CrGlinkServiceQuiesce(&Link,&Service,Service.Generation)==CR_SUCCESS);
      QCHECK(!memcmp(Before,Descriptor,sizeof(Before)));
      QCHECK(Service.Registered&&!Service.Online&&!Service.Detaching&&
             !Service.TxLength&&!Service.TxOffset&&Service.Config.TxStorage==OwnedTx);
    }
  }
  return QG_Last;
}
static void QG_State(VOID *Context,BOOLEAN Online){QCHECK(Context==&Link);CrQrtrOnline(&QA_R,Online);}
static CR_STATUS QG_Input(CONST UINT8 *B,UINTN N){
  QCHECK(N<=1024);Data(88,1,TRUE,B,(UINT32)N,0);PmicPacket();Pump();return QG_Last;
}
static void QG_Settle(void){for(unsigned I=0;I<12;++I){QA_Tick(++QA_Now);Pump();Drain();if(!QA_R.QueueCount&&!Link.QueueCount)return;}QCHECK(0);}
static void QG_Fixture(void){
  Case="audio PD actual QRTR/GLINK";Fixture();QA_Now=0;QA_SentCount=0;QA_SendStatus=CR_SUCCESS;QG_Calls=0;QG_ReleaseTxOnDown=FALSE;QG_QuiesceOnDown=FALSE;
  CrQrtrInitialize(&QA_R,QG_Send,&QA_R);QCHECK(CrQrtrEnableAudioPd(&QA_R)==CR_SUCCESS);
  Link.QrtrReceive=QG_Receive;Link.QrtrState=QG_State;
  Packet(2,88,sizeof("IPCRTR"),"IPCRTR",sizeof("IPCRTR"));Packet(4,2,0,NULL,0);
  UINT32 Pairs[128];for(unsigned I=0;I<64;++I){Pairs[2*I]=1024;Pairs[2*I+1]=300+I;}Packet(5,88,64,Pairs,sizeof(Pairs));Pump();Drain();QCHECK(QA_R.Online);
  Connect(20,512);QCHECK(Local==3);QA_Backend=QG_Input;QG_Settle();QA_Ctrl(4,2,42);QG_Settle();QCHECK(QA_R.Audio.Pending&&QA_R.Pending);
  QA_Response(2,42,0x4001,QA_R.PendingTransaction,0x1FFFFFFF,0,TRUE);QA_Response(2,42,0x4002,QA_R.Audio.PendingTransaction,0x1FFFFFFF,0,TRUE);QG_Settle();QCHECK(QG_Observed.Ready&&QA_R.PdReady&&Link.Ready&&Service.Online);
}
int main(void){
  QG_Fixture();UINT32 Gen=QG_Observed.Generation;UINTN PriorPmic=PmicCalls;
  QCHECK(QA_Ind(3,2,42,0x4002,0x0FFFFFFF,"msm/adsp/audio_pd",0x5501,1)==CR_SUCCESS);
  QCHECK(!QG_Observed.Ready&&QG_Observed.Generation>Gen&&QA_R.PdReady&&Link.Ready&&PmicCalls>PriorPmic);QG_Settle();
  QCHECK(QA_Ind(1,2,42,0x4002,0x1FFFFFFF,"msm/adsp/audio_pd",0x5502,0)==CR_SUCCESS);QG_Settle();QCHECK(QG_Observed.Ready);
  UINT8 Reply[28]={0};Put32(Reply,(28U<<8)|(6U<<4));Reply[4]=3;Reply[5]=2;Put32(Reply+8,1);Put32(Reply+12,3);Put32(Reply+16,1);Put32(Reply+20,0x02001007);Put32(Reply+24,1);
  UINTN PriorCalls=Calls;Data(Remote,1,TRUE,Reply,28,0);PmicPacket();Pump();Drain();QCHECK(Calls==PriorCalls+1&&CopiedBytes==28&&!memcmp(Copied,Reply,28)&&QA_State().Ready);
  QA_Ctrl(5,2,42);QCHECK(!QG_Observed.Ready&&!QG_Observed.ServerFound&&Service.Online&&Link.Ready);QG_Settle();
  CrGlinkReset(&Link);QCHECK(!QA_R.Online&&!QA_State().Ready&&QA_State().Enabled);
  printf("Actual QRTR/audio-PD + GLINK + sibling PMIC composition: %u checks, %zu QRTR callbacks PASS\n",QA_Checks,(size_t)QG_Calls);

  /* Identical actual-core wire interleaving with and without the callback
   * gate. The archived pre-API sources/receipts also retain the original gap. */
  for(unsigned Gate=0;Gate<2;++Gate){
    QG_Fixture();UINT32 Pair[2]={65536,900};Packet(5,Remote,1,Pair,sizeof(Pair));Pump();Drain();
    for(UINTN I=0;I<sizeof(Input);++I)Input[I]=(UINT8)(I*13+7);
    QCHECK(CrGlinkServiceSend(&Link,&Service,Service.Generation,Input,sizeof(Input))==CR_SUCCESS);
    Descriptor[0]=(Descriptor[1]+8U)%sizeof(Tx); /* blocked peer TX consumer */
    UINTN PmicBefore=PmicCalls,AcksBefore=QA_SentCount;
    QG_QuiesceOnDown=Gate!=0;QG_ReleaseTxOnDown=TRUE;
    QCHECK(QA_Ind(1,2,42,0x4002,0x0FFFFFFF,"msm/adsp/audio_pd",1,0)==CR_SUCCESS);
    QCHECK(!QG_Observed.Ready&&!QG_ReleaseTxOnDown&&Descriptor[1]!=QG_HeadAtDown);Drain();
    if(Gate){
      QCHECK(WireBytes==0&&WireFragments==0&&Commands[3]==1);
      QG_Settle();QCHECK(PmicCalls>PmicBefore&&QA_SentCount>AcksBefore&&QA_R.PdReady&&Link.Ready);
      QCHECK(Service.Registered&&!Service.Online&&Service.PeerClosed&&!Service.Detaching);
    }else QCHECK(WireBytes==15360&&WireFragments==15);
    printf("Actual QRTR DOWN %s gate: %zu old audio bytes / %zu fragments after callback; sibling PMIC/QRTR retained\n",Gate?"with local quiesce":"without",(size_t)WireBytes,(size_t)WireFragments);
  }
  return 0;
}
