/* Actual graph/compiler/GPR/GLINK source tests with a synthetic remote peer.
 * Genuine ACDB queries and oracle bodies remain unmodified. BSD-3-Clause */
#include "../Tools/AudioGraphSession/AudioGraphPeer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long Checks;
static UINT32 ClosedPlans,ExternalPlans,CompilerRejected,LargePlans;
static CONST CHAR8 *Case="initial";
#define CHECK(X) do{++Checks;if(!(X)){fprintf(stderr,"%s:%d: %s\n",Case,__LINE__,#X);abort();}}while(0)
typedef struct {
  AUDIO_GRAPH_PEER Peer;
  CR_AUDIO_GRAPH_SESSION Session;
  CR_AUDIO_GRAPH_STORAGE Storage;
  CR_AUDIO_GRAPH_PORT Port;
  UINT64 Now;
} FIXTURE;
static UINT8 *Source;
static UINTN SourceBytes;
static CR_AUDIO_CAL_REQUEST Profile;
static VOID *Workspace;
static UINTN WorkBytes;
static UINT32 ReadWord(FILE *F){UINT8 B[4];CHECK(fread(B,1,4,F)==4);return AudioPeerWord(B);}
static UINT8 *ReadBytes(FILE *F,UINT32 N){UINT8 *P=malloc(N?N:1);CHECK(P);CHECK(fread(P,1,N,F)==N);return P;}
static FIXTURE *New(UINT32 Handshake){
  FIXTURE *F=calloc(1,sizeof(*F));CHECK(F);AudioPeerInit(&F->Peer,Handshake);
  CHECK(CrAudioGraphPortInit(&F->Port,F,0x700)==CR_SUCCESS);
  CHECK(CrAudioGraphInit(&F->Session,&F->Port,F,&F->Peer.Binding,&F->Peer.Epoch,1000,Source,SourceBytes,&Profile,Workspace,WorkBytes,&F->Storage)==CR_SUCCESS);
  AudioPeerAttach(&F->Peer,&F->Session);return F;
}
static VOID Step(FIXTURE *F){CrAudioGraphTick(&F->Session,F->Now++);AudioPeerProgress(&F->Peer);}
static VOID Until(FIXTURE *F,UINT32 Phase){
  for(UINT32 I=0;I<2000&&F->Session.Phase!=Phase;++I)Step(F);
  CHECK(F->Session.Phase==Phase);
}
static VOID Pending(FIXTURE *F,UINT32 Phase){
  F->Peer.DropPhase=Phase;
  for(UINT32 I=0;I<1000&&(!F->Session.Pending||F->Session.PendingPhase!=Phase||!F->Peer.Sent[Phase]);++I)Step(F);
  CHECK(F->Session.Pending&&F->Session.PendingPhase==Phase&&F->Peer.Sent[Phase]==1);
}
static VOID Dispose(FIXTURE *F){
  CHECK(!F->Session.GraphPossible&&!F->Session.Cookie&&!F->Peer.Graphs&&!F->Peer.Cookie&&F->Session.Phase==CrAudioGraphEnded);
  if(F->Peer.Service.Registered)AudioPeerDetach(&F->Peer);free(F);
}
static VOID Close(FIXTURE *F){CHECK(CrAudioGraphRequestClose(&F->Session)==CR_SUCCESS);Until(F,CrAudioGraphEnded);Dispose(F);}
static VOID ResetRetire(FIXTURE *F){
  AudioPeerResetGraph(&F->Peer);CrAudioGraphTick(&F->Session,F->Now++);CHECK(F->Session.Phase==CrAudioGraphEnded);Dispose(F);
}
static VOID Corpus(CONST CHAR8 *Path){
  FILE *F=fopen(Path,"rb");CHECK(F);UINT8 *Magic=ReadBytes(F,4);CHECK(!memcmp(Magic,"AGS1",4));free(Magic);
  UINT32 NameBytes=ReadWord(F);UINT8 *Name=ReadBytes(F,NameBytes+1);CHECK(Name[NameBytes]==0);
  FILE *Acdb=fopen((char *)Name,"rb");CHECK(Acdb);CHECK(!fseek(Acdb,0,SEEK_END));long Length=ftell(Acdb);CHECK(Length>0);rewind(Acdb);
  UINT8 *Data=ReadBytes(Acdb,(UINT32)Length);fclose(Acdb);free(Name);
  UINT32 Count=ReadWord(F);CR_AUDIO_GRAPH_PORT Port={0};CHECK(CrAudioGraphPortInit(&Port,&Port,0x700)==CR_SUCCESS);
  for(UINT32 Index=0;Index<Count;++Index){
    UINT32 Id=ReadWord(F),Expected=ReadWord(F);(void)Id;
    CR_AUDIO_CAL_REQUEST R;CHECK(fread(&R,1,sizeof(R),F)==sizeof(R));
    UINT32 N[3];UINT8 *Oracle[3];for(UINT32 I=0;I<3;++I){N[I]=ReadWord(F);Oracle[I]=ReadBytes(F,N[I]);}
    FIXTURE *T=calloc(1,sizeof(*T));CHECK(T);AudioPeerInit(&T->Peer,Index%3);
    CR_STATUS Status=CrAudioGraphInit(&T->Session,&Port,&Port,&T->Peer.Binding,&T->Peer.Epoch,1000,Data,(UINTN)Length,&R,Workspace,WorkBytes,&T->Storage);
    if(Expected!=1){CHECK(Status!=CR_SUCCESS);CHECK(!Port.Session&&T->Session.Self==0);if(Expected==2)++ExternalPlans;else ++CompilerRejected;AudioPeerDetach(&T->Peer);free(T);}
    else{
      ++ClosedPlans;CHECK(Status==CR_SUCCESS);
      CONST CR_AUDIO_CAL_PLAN *P=&T->Storage.Plan;
      CHECK(P->OpenPropertyBytes==N[0]&&P->OpenBytes==40+N[0]+N[1]&&P->CalibrationBytes==40+N[2]);
      CHECK(!memcmp(P->Open+40,Oracle[0],N[0])&&!memcmp(P->Open+40+N[0],Oracle[1],N[1])&&!memcmp(P->Calibration+40,Oracle[2],N[2]));
      /* The known closed fixture becomes the failure-injection source. */
      if(!Source&&Index==1){Source=malloc((size_t)Length);CHECK(Source);memcpy(Source,Data,(size_t)Length);SourceBytes=(UINTN)Length;Profile=R;}
      T->Peer.SyncReply=(Index&1)!=0;AudioPeerAttach(&T->Peer,&T->Session);Until(T,CrAudioGraphConfigured);
      CHECK(T->Session.Replies==4&&T->Peer.Graphs==P->SubgraphCount);
      CHECK(T->Peer.Fragments>=4);
      if(T->Peer.MaxPacket>1024){++LargePlans;CHECK(T->Peer.Fragments>4);}
      for(UINT32 Intent=1;Intent<=20;++Intent){AudioPeerUnrelated(&T->Peer,Intent);Step(T);}
      CHECK(T->Peer.Pmic==20&&T->Peer.Qrtr==20&&T->Session.Ignored>=20);
      CHECK(CrAudioGraphRequestClose(&T->Session)==CR_SUCCESS);Until(T,CrAudioGraphEnded);
      CHECK(T->Session.Replies==5&&T->Peer.OpenCount==1&&T->Peer.CloseCount==1&&!Port.Session);
      Dispose(T);
    }
    for(UINT32 I=0;I<3;++I)free(Oracle[I]);
  }
  CHECK(fgetc(F)==EOF);fclose(F);free(Data);
}
static VOID Failures(VOID){
  const UINT32 Phases[]={1,2,3,4,6};
  for(UINT32 I=0;I<5;++I){UINT32 Phase=Phases[I];
    Case="remote-error";FIXTURE *F=New(I%3);F->Peer.ErrorPhase=Phase;F->Peer.DspError=0xdead0042;
    if(Phase==6){Until(F,CrAudioGraphConfigured);CrAudioGraphRequestClose(&F->Session);}
    for(UINT32 J=0;J<100;++J)Step(F);
    CHECK(F->Session.Failed&&F->Session.Error==CR_DEVICE_ERROR);
    if(Phase==6){CHECK(F->Session.GraphPossible&&F->Session.Cookie&&F->Session.CleanupError==CR_DEVICE_ERROR);CHECK(F->Peer.CloseCount==1);ResetRetire(F);}
    else{CHECK(F->Session.Phase==CrAudioGraphEnded);if(Phase>1)CHECK(F->Peer.CloseCount==1);Dispose(F);}
    Case="timeout-late";F=New(I%3);
    if(Phase==6){Until(F,CrAudioGraphConfigured);CrAudioGraphRequestClose(&F->Session);}
    Pending(F,Phase);F->Now=F->Session.Deadline;Step(F);CHECK(F->Session.Failed&&F->Session.Error==CR_TIMEOUT);
    if(Phase==1){CHECK(F->Session.Phase==CrAudioGraphEnded);Dispose(F);}
    else{CHECK(F->Session.GraphPossible&&F->Session.Cookie&&F->Session.Pending);UINT32 Before=F->Peer.CloseCount;
      for(UINT32 J=0;J<10;++J)Step(F);CHECK(F->Peer.CloseCount==Before);
      F->Peer.DropPhase=0;AudioPeerLateReply(&F->Peer);Until(F,CrAudioGraphEnded);CHECK(F->Session.Error==CR_TIMEOUT);Dispose(F);}
    Case="rejected-deadline";F=New(I%3);F->Peer.RejectPhase=Phase;
    if(Phase==6){Until(F,CrAudioGraphConfigured);CrAudioGraphRequestClose(&F->Session);}
    for(UINT32 J=0;J<100;++J)Step(F);CHECK(!F->Peer.Sent[Phase]&&F->Session.Pending&&F->Session.Acceptance==CrAudioGraphNotAccepted);
    F->Now=F->Session.Deadline;Step(F);
    if(Phase==6){UINT32 Attempts=F->Peer.Attempts[6];for(UINT32 J=0;J<10;++J)Step(F);CHECK(F->Peer.Attempts[6]==Attempts&&F->Session.GraphPossible);ResetRetire(F);}
    else{Until(F,CrAudioGraphEnded);Dispose(F);}
    Case="unknown-accepted";F=New(I%3);F->Peer.UnknownPhase=Phase;F->Peer.DropPhase=Phase;
    if(Phase==6){Until(F,CrAudioGraphConfigured);CrAudioGraphRequestClose(&F->Session);}
    Pending(F,Phase);CHECK(F->Session.Acceptance==CrAudioGraphUnknown&&F->Session.Failed);
    if(Phase==1){F->Now=F->Session.Deadline;Until(F,CrAudioGraphEnded);Dispose(F);}
    else{UINT32 Sent=F->Peer.Sent[Phase];for(UINT32 J=0;J<10;++J)Step(F);CHECK(F->Peer.Sent[Phase]==Sent);ResetRetire(F);}
  }
}
static VOID Epochs(VOID){
  for(UINT32 Phase=1;Phase<=4;++Phase)for(UINT32 Field=0;Field<3;++Field)for(UINT32 Mode=0;Mode<3;++Mode){
    Case="epoch-loss";FIXTURE *F=New(Field);Pending(F,Phase);CR_AUDIO_GRAPH_EPOCH E=F->Peer.Epoch;
    if(Field==0)E.Channel=Mode==0?0:Mode==1?E.Channel-1:E.Channel+1;
    if(Field==1)E.Adsp=Mode==0?0:Mode==1?E.Adsp-1:E.Adsp+1;
    if(Field==2)E.AudioPd=Mode==0?0:Mode==1?E.AudioPd-1:E.AudioPd+1;
    CHECK(CrAudioGraphState(&F->Session,&E,TRUE)==CR_SUCCESS);Step(F);CHECK(F->Session.Lost);
    if(Phase==1){CHECK(F->Session.Phase==CrAudioGraphEnded);Dispose(F);}
    else{
      CHECK(F->Session.GraphPossible&&F->Session.Cookie);AudioPeerLateReply(&F->Peer);Step(F);CHECK(F->Session.GraphPossible);
      AudioPeerDetach(&F->Peer);CHECK(F->Session.GraphPossible&&F->Session.Cookie);ResetRetire(F);
    }
  }
  Case="admit-synchronous-loss";FIXTURE *F=New(1);F->Peer.LoseDuringAdmit=TRUE;Step(F);CHECK(!F->Peer.Attempts[1]);Step(F);CHECK(F->Session.Lost);Dispose(F);
  Case="send-synchronous-loss";F=New(2);Pending(F,2);F->Peer.DropPhase=0;AudioPeerLateReply(&F->Peer);F->Peer.LoseDuringSend=TRUE;Step(F);Step(F);CHECK(F->Session.Lost&&F->Session.GraphPossible);ResetRetire(F);
  Case="mailbox-overflow";F=New(0);Pending(F,2);CR_AUDIO_GRAPH_EPOCH E=F->Peer.Epoch;
  for(UINT32 I=0;I<CR_AUDIO_GRAPH_MAILBOX;++I)CHECK(CrAudioGraphState(&F->Session,&E,TRUE)==CR_SUCCESS);
  CHECK(CrAudioGraphState(&F->Session,&E,TRUE)==CR_OUT_OF_RESOURCES);Step(F);CHECK(F->Session.Lost&&F->Session.GraphPossible);ResetRetire(F);
}
static VOID Arguments(VOID){
  Case="arguments";FIXTURE *F=New(0);CR_AUDIO_GRAPH_STATUS Status;
  CHECK(CrAudioGraphPortInit(&F->Port,F,0x700)==CR_INVALID_PARAMETER);
  CHECK(CrAudioGraphInit(&F->Session,&F->Port,F,&F->Peer.Binding,&F->Peer.Epoch,1000,Source,SourceBytes,&Profile,Workspace,WorkBytes,&F->Storage)==CR_INVALID_PARAMETER);
  CHECK(CrAudioGraphGetStatus(&F->Session,(CR_AUDIO_GRAPH_STATUS *)&F->Storage)==CR_INVALID_PARAMETER);
  CHECK(CrAudioGraphGetStatus(&F->Session,&Status)==CR_SUCCESS&&Status.Session==1);
  CR_AUDIO_GRAPH_SESSION Copy=F->Session;CHECK(CrAudioGraphTick(&Copy,0)==CR_INVALID_PARAMETER);
  CHECK(CrAudioGraphReceive(&F->Session,&F->Peer.Epoch,F->Storage.Plan.Open,24)==CR_INVALID_PARAMETER);
  CHECK(CrAudioGraphState(&F->Session,&F->Session.Claim.Epoch,TRUE)==CR_INVALID_PARAMETER);
  CHECK(CrAudioGraphRequestClose(&F->Session)==CR_SUCCESS);Step(F);Dispose(F);
  Case="zero-exhausted-counter";
  for(UINT32 I=0;I<3;++I){F=calloc(1,sizeof(*F));CHECK(F);AudioPeerInit(&F->Peer,0);CHECK(CrAudioGraphPortInit(&F->Port,F,0x700)==CR_SUCCESS);
    if(I==0)F->Port.NextToken=0;else if(I==1)F->Port.NextToken=0xffffffffU-3;else F->Port.NextSession=~(UINT64)0;
    CHECK(CrAudioGraphInit(&F->Session,&F->Port,F,&F->Peer.Binding,&F->Peer.Epoch,1000,Source,SourceBytes,&Profile,Workspace,WorkBytes,&F->Storage)!=CR_SUCCESS);
    CHECK(!F->Peer.Attempts[1]&&!F->Port.Session&&!F->Session.Self);AudioPeerDetach(&F->Peer);free(F);}
  Case="last-valid-token-range";F=calloc(1,sizeof(*F));CHECK(F);AudioPeerInit(&F->Peer,1);CHECK(CrAudioGraphPortInit(&F->Port,F,0x700)==CR_SUCCESS);F->Port.NextToken=0xffffffffU-4;
  CHECK(CrAudioGraphInit(&F->Session,&F->Port,F,&F->Peer.Binding,&F->Peer.Epoch,1000,Source,SourceBytes,&Profile,Workspace,WorkBytes,&F->Storage)==CR_SUCCESS);
  CHECK(F->Port.NextToken==0x100000000ULL);AudioPeerAttach(&F->Peer,&F->Session);Until(F,CrAudioGraphConfigured);CrAudioGraphRequestClose(&F->Session);Until(F,CrAudioGraphEnded);
  CR_AUDIO_GRAPH_SESSION *Exhausted=calloc(1,sizeof(*Exhausted));CHECK(Exhausted);
  CHECK(CrAudioGraphInit(Exhausted,&F->Port,F,&F->Peer.Binding,&F->Peer.Epoch,1000,Source,SourceBytes,&Profile,Workspace,WorkBytes,&F->Storage)==CR_OUT_OF_RESOURCES);CHECK(!Exhausted->Self);free(Exhausted);Dispose(F);
  Case="retained-release";F=New(0);Until(F,CrAudioGraphConfigured);F->Peer.ReleaseBusy=TRUE;CrAudioGraphRequestClose(&F->Session);for(UINT32 I=0;I<30;++I)Step(F);
  CHECK(F->Session.Phase==CrAudioGraphReleasing&&F->Session.Cookie&&!F->Session.GraphPossible&&F->Port.Session==&F->Session);F->Peer.ReleaseBusy=FALSE;Step(F);Dispose(F);
  Case="admit-retained-failure";F=New(0);F->Peer.AdmitFailure=TRUE;F->Peer.AdmitRetained=TRUE;F->Peer.ReleaseBusy=TRUE;Step(F);
  CHECK(F->Session.Failed&&F->Session.Cookie&&!F->Peer.Attempts[1]);F->Peer.ReleaseBusy=FALSE;Step(F);Dispose(F);
  Case="backwards-clock";F=New(0);Pending(F,2);CHECK(F->Now>1);CrAudioGraphTick(&F->Session,F->Now-2);CHECK(F->Session.Lost&&F->Session.GraphPossible);ResetRetire(F);
  Case="deadline-overflow";F=New(0);CrAudioGraphTick(&F->Session,~(UINT64)0-1);CHECK(F->Session.Failed&&!F->Peer.Attempts[1]);CrAudioGraphTick(&F->Session,~(UINT64)0);Dispose(F);
}
static VOID Replies(VOID){
  Case="reply-matching";FIXTURE *F=New(1);Pending(F,2);UINT8 Saved[512];memcpy(Saved,F->Peer.SavedReply,F->Peer.ReplyBytes);
  CR_AUDIO_GRAPH_EPOCH E=F->Peer.Epoch;
  const UINT32 Offsets[]={4,5,8,12,16,20,24};
  for(UINT32 I=0;I<sizeof(Offsets)/sizeof(Offsets[0]);++I){UINT8 Reply[512];memcpy(Reply,Saved,F->Peer.ReplyBytes);Reply[Offsets[I]]^=0x40;CHECK(CrAudioGraphReceive(&F->Session,&E,Reply,F->Peer.ReplyBytes)==CR_SUCCESS);Step(F);CHECK(F->Session.Pending&&F->Session.PendingPhase==2);}
  for(UINT32 N=1;N<F->Peer.ReplyBytes;++N){CHECK(CrAudioGraphReceive(&F->Session,&E,Saved,N)==CR_SUCCESS);Step(F);CHECK(F->Session.Pending);}
  CHECK(F->Session.Malformed&&F->Session.Ignored);F->Peer.DropPhase=0;AudioPeerLateReply(&F->Peer);Until(F,CrAudioGraphConfigured);
  UINT32 Replies=F->Session.Replies;CHECK(CrAudioGraphReceive(&F->Session,&E,Saved,32)==CR_SUCCESS);Step(F);CHECK(F->Session.Replies==Replies);Close(F);
  Case="extended-reply";F=New(0);Pending(F,2);UINT8 Extended[68]={0};memcpy(Extended,F->Peer.SavedReply,24);memcpy(Extended+60,F->Peer.SavedReply+24,8);AudioPeerPut(Extended,(68U<<8)|0xf0);E=F->Peer.Epoch;
  CHECK(CrAudioGraphReceive(&F->Session,&E,Extended,sizeof(Extended))==CR_SUCCESS);F->Peer.DropPhase=0;Until(F,CrAudioGraphConfigured);Close(F);
  Case="cancel-pending-open";F=New(2);Pending(F,2);CrAudioGraphRequestClose(&F->Session);Step(F);CHECK(!F->Peer.CloseCount&&F->Session.Pending&&F->Session.GraphPossible);AudioPeerLateReply(&F->Peer);F->Peer.DropPhase=0;Until(F,CrAudioGraphEnded);Dispose(F);
}
static VOID Additional(VOID){
  const UINT32 Phases[]={1,2,3,4,6};
  for(UINT32 I=0;I<5;++I)for(UINT32 Error=1;Error<=30;++Error){
    Case="raw-dsp-error-retained";UINT32 Phase=Phases[I];FIXTURE *F=New(1);F->Peer.ErrorPhase=Phase;F->Peer.DspError=Error;
    if(Phase==6){Until(F,CrAudioGraphConfigured);CrAudioGraphRequestClose(&F->Session);}
    for(UINT32 J=0;J<30;++J)Step(F);
    CHECK(F->Session.Failed&&F->Session.DspStatus==Error);
    if(Phase==6){CHECK(F->Session.CleanupDspStatus==Error&&F->Session.CleanupDspOpcode==0x01001004);ResetRetire(F);}
    else{CHECK(F->Session.Phase==CrAudioGraphEnded&&F->Session.DspOpcode==(Phase==1?0x01001021:Phase==2?0x01001000:0x01001006));Dispose(F);}
  }
  Case="queued-response-after-deadline";FIXTURE *F=New(0);Pending(F,2);F->Peer.DropPhase=0;AudioPeerLateReply(&F->Peer);F->Now=F->Session.Deadline;Step(F);
  CHECK(F->Session.Failed&&F->Session.Error==CR_TIMEOUT&&!F->Peer.Sent[3]);Until(F,CrAudioGraphEnded);Dispose(F);
  for(UINT32 Mode=0;Mode<4;++Mode){
    Case="synchronous-reply-before-outcome";F=New(1);F->Peer.SyncReply=TRUE;F->Peer.OverridePhase=2;
    F->Peer.OverrideAcceptance=Mode==0?0:Mode==1?CrAudioGraphNotAccepted:Mode==2?CrAudioGraphAccepted:CrAudioGraphUnknown;
    F->Peer.OverrideStatus=Mode==2?(CR_STATUS)7:CR_SUCCESS;
    Until(F,CrAudioGraphEnded);CHECK(F->Session.Failed&&!F->Peer.Sent[3]&&F->Peer.OpenCount==1&&F->Peer.CloseCount==1);Dispose(F);
  }
  Case="spf-basic-success-is-not-ready";F=New(2);Pending(F,1);UINT8 Reply[32]={0};
  AudioPeerPut(Reply,(32U<<8)|0x60);Reply[4]=3;Reply[5]=2;AudioPeerPut(Reply+8,1);AudioPeerPut(Reply+12,F->Port.HostPort);AudioPeerPut(Reply+16,F->Session.TokenBase);AudioPeerPut(Reply+20,CR_AUDIO_GPR_BASIC_RESPONSE);AudioPeerPut(Reply+24,CR_AUDIO_APM_GET_SPF_STATE);
  CR_AUDIO_GRAPH_EPOCH E=F->Peer.Epoch;CHECK(CrAudioGraphReceive(&F->Session,&E,Reply,sizeof(Reply))==CR_SUCCESS);Step(F);CHECK(F->Session.Pending&&!F->Peer.OpenCount);
  F->Peer.DropPhase=0;AudioPeerLateReply(&F->Peer);Until(F,CrAudioGraphConfigured);Close(F);
  Case="permanent-port-next-session";F=New(1);Until(F,CrAudioGraphConfigured);CrAudioGraphRequestClose(&F->Session);Until(F,CrAudioGraphEnded);AudioPeerDetach(&F->Peer);
  UINT64 PreviousToken=F->Port.NextToken,PreviousSession=F->Port.NextSession;CHECK(PreviousToken==6&&PreviousSession==2);
  CR_AUDIO_GRAPH_SESSION *Next=calloc(1,sizeof(*Next));CR_AUDIO_GRAPH_STORAGE *Store=calloc(1,sizeof(*Store));CHECK(Next&&Store);
  AudioPeerInit(&F->Peer,1);CHECK(CrAudioGraphInit(Next,&F->Port,F,&F->Peer.Binding,&F->Peer.Epoch,1000,Source,SourceBytes,&Profile,Workspace,WorkBytes,Store)==CR_SUCCESS);
  CHECK(Next->TokenBase==PreviousToken&&Next->Claim.Session==PreviousSession&&F->Port.NextToken==11);
  AudioPeerAttach(&F->Peer,Next);CrAudioGraphRequestClose(Next);CrAudioGraphTick(Next,0);CHECK(Next->Phase==CrAudioGraphEnded);AudioPeerDetach(&F->Peer);free(Next);free(Store);free(F);
  Case="partial-fragment-old-command-retention";F=New(0);F->Peer.FreezeAfterFragments=2;
  for(UINT32 I=0;I<30&&F->Peer.Fragments<2;++I)Step(F);
  CHECK(F->Peer.Fragments==2&&F->Peer.WireBytes>0&&F->Peer.Sent[2]==0&&F->Session.GraphPossible);
  E=F->Peer.Epoch;E.AudioPd++;CHECK(CrAudioGraphState(&F->Session,&E,FALSE)==CR_SUCCESS);Step(F);
  AudioPeerResetGraph(&F->Peer);Step(F);CHECK(F->Session.GraphPossible&&F->Session.Cookie);
  F->Peer.FreezeAfterFragments=0;AudioPeerProgress(&F->Peer);CHECK(F->Peer.Graphs&&F->Peer.Sent[2]==1);Step(F);CHECK(F->Session.GraphPossible);ResetRetire(F);
  Case="close-completion-after-generation-change";F=New(0);Until(F,CrAudioGraphConfigured);CrAudioGraphRequestClose(&F->Session);Pending(F,6);E=F->Peer.Epoch;E.Channel++;
  CHECK(CrAudioGraphState(&F->Session,&E,TRUE)==CR_SUCCESS);AudioPeerLateReply(&F->Peer);Step(F);CHECK(F->Session.GraphPossible&&F->Session.Cookie);ResetRetire(F);
}
static VOID MutatedConnections(VOID){
  Case="synthetic-control-link-mutation";FIXTURE *Original=New(0);CONST CR_AUDIO_CAL_PLAN *Plan=&Original->Storage.Plan;
  UINT32 Found=0,Size=0,Peer1=0,Peer2=0;
  for(UINT32 At=40;At<40+Plan->OpenPropertyBytes&&!Found;){
    UINT32 N=AudioPeerWord(Plan->Open+At+8);
    if(AudioPeerWord(Plan->Open+At+4)==0x08001004&&N>=36){
      for(UINT32 Q=12;Q+N<=SourceBytes;++Q){
        if(AudioPeerWord(Source+Q-12)==1&&AudioPeerWord(Source+Q-8)==0x08001004&&AudioPeerWord(Source+Q-4)==N&&!memcmp(Source+Q,Plan->Open+At+16,N)){
          Found=Q;Size=N;Peer1=AudioPeerWord(Source+Q+4);Peer2=AudioPeerWord(Source+Q+12);break;
        }
      }
    }
    At+=16+((N+7)&~7U);
  }
  CHECK(Found&&Size>=36);CrAudioGraphRequestClose(&Original->Session);Step(Original);Dispose(Original);
  UINT8 *Mutated=malloc(SourceBytes);CHECK(Mutated);
  for(UINT32 Mode=0;Mode<4;++Mode){
    memcpy(Mutated,Source,SourceBytes);AudioPeerPut(Mutated+Found-8,0x08001061);
    memset(Mutated+Found,0,Size);AudioPeerPut(Mutated+Found,1);AudioPeerPut(Mutated+Found+4,Peer1);AudioPeerPut(Mutated+Found+8,1);AudioPeerPut(Mutated+Found+12,Peer2);AudioPeerPut(Mutated+Found+16,2);
    AudioPeerPut(Mutated+Found+20,1);AudioPeerPut(Mutated+Found+24,0x08001062);AudioPeerPut(Mutated+Found+28,Size-32);AudioPeerPut(Mutated+Found+32,(Size-36)/4);
    for(UINT32 I=36;I<Size;I+=4)AudioPeerPut(Mutated+Found+I,0x100+I);
    if(Mode==1)AudioPeerPut(Mutated+Found+4,0xdeedbeef);
    if(Mode==2)AudioPeerPut(Mutated+Found+12,0xdeedbeef);
    if(Mode==3)AudioPeerPut(Mutated+Found+28,0xffffffffU);
    FIXTURE *F=calloc(1,sizeof(*F));CHECK(F);AudioPeerInit(&F->Peer,0);CHECK(CrAudioGraphPortInit(&F->Port,F,0x700)==CR_SUCCESS);
    CR_STATUS Status=CrAudioGraphInit(&F->Session,&F->Port,F,&F->Peer.Binding,&F->Peer.Epoch,1000,Mutated,SourceBytes,&Profile,Workspace,WorkBytes,&F->Storage);
    if(Mode==0){CHECK(Status==CR_SUCCESS);AudioPeerAttach(&F->Peer,&F->Session);Until(F,CrAudioGraphConfigured);Close(F);}
    else{CHECK(Status==(Mode==3?CR_COMPROMISED_DATA:CR_UNSUPPORTED));CHECK(F->Port.NextToken==1&&!F->Port.Session&&!F->Session.Self&&!F->Peer.Attempts[1]);AudioPeerDetach(&F->Peer);free(F);}
  }
  free(Mutated);
  Case="primary-and-partial-cleanup-errors";FIXTURE *F=New(0);F->Peer.ErrorPhase=3;F->Peer.DspError=0x17;F->Peer.CloseDspError=0x19;
  for(UINT32 I=0;I<30;++I)Step(F);
  CHECK(F->Session.GraphPossible&&F->Session.Cookie&&F->Session.DspStatus==0x17&&F->Session.CleanupDspStatus==0x19&&F->Peer.CloseFailed&&F->Peer.Graphs<F->Storage.Plan.SubgraphCount);
  CHECK(F->Storage.Plan.SubgraphCount==F->Session.Claim.Plan->SubgraphCount);ResetRetire(F);
}
int main(int Argc,char **Argv){
  CHECK(Argc>1);WorkBytes=CrAudioCalWorkspaceBytes();Workspace=malloc(WorkBytes);CHECK(Workspace);
  for(int I=1;I<Argc;++I){Case=Argv[I];Corpus(Argv[I]);}
  CHECK(ClosedPlans==50&&ExternalPlans==66&&CompilerRejected==2357&&LargePlans>0&&Source);
  Failures();Epochs();Arguments();Replies();Additional();MutatedConnections();
  printf("AudioGraph actual compiler/GPR/GLINK: %lu checks; %u genuine closed plans, %u external rejections, %u compiler rejections PASS\n",Checks,ClosedPlans,ExternalPlans,CompilerRejected);
  free(Source);free(Workspace);return 0;
}
