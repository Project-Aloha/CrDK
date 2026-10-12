/* Actual typed ACDB/graph/Pull runtime. The supplied synthetic ACDB qualifies
 * code only. GLINK graph peer and Pull remote firmware replies are controlled. */
#include "AudioGraphPeer.h"
#include <Library/AudioPullCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {AUDIO_GRAPH_PEER Peer;CR_AUDIO_GRAPH_PORT Port;CR_AUDIO_GRAPH_SESSION Graph;CR_AUDIO_GRAPH_STORAGE Storage;CR_AUDIO_PULL_SESSION Pull;UINT64 Now;UINT32 Attempts[7],Drop,Reject,Error,Unknown,Sent;BOOLEAN Lost;UINT8 Late[128];UINT32 LateBytes;} FIXTURE;
static UINT8 *Acdb;static UINTN Bytes,WorkBytes;static VOID *Work;static CR_AUDIO_CAL_REQUEST Request;
static BOOLEAN Current(VOID *Context){return !((FIXTURE*)Context)->Lost;}
static VOID Reply(FIXTURE *F,CR_AUDIO_PULL_COMMAND *C,UINT32 Status){
 UINT32 I=(UINT32)(C-F->Pull.Commands),N=I<2&&!Status?28:32;UINT8 *P=F->Late;memset(P,0,128);AudioPeerPut(P,(N<<8)|0x60);P[4]=3;P[5]=2;AudioPeerPut(P+8,C->Destination);AudioPeerPut(P+12,F->Graph.Claim.HostPort);AudioPeerPut(P+16,C->Token);AudioPeerPut(P+20,N==28?CR_AUDIO_PCM_MAP_RESPONSE:CR_AUDIO_GPR_BASIC_RESPONSE);AudioPeerPut(P+24,N==28?0x9001+I:C->Opcode);if(N==32)AudioPeerPut(P+28,Status);F->LateBytes=N;
}
static CR_AUDIO_GRAPH_SEND_RESULT Send(VOID *Context,CONST VOID *Packet,UINT32 Bytes_){
 FIXTURE *F=Context;CR_AUDIO_PULL_SESSION *S=&F->Pull;UINT32 I=S->Next;CR_AUDIO_PULL_COMMAND *C=&S->Commands[I];CONST UINT8 *P=Packet;assert(I<7&&Packet==C->Packet&&Bytes_==C->Bytes&&S->Active&&C->Attempted&&!F->Lost);++F->Attempts[I];
 assert(AudioPeerWord(P)==((Bytes_<<8)|0x60)&&P[4]==2&&P[5]==3&&AudioPeerWord(P+8)==0x2010&&AudioPeerWord(P+16)==C->Token);
 if(F->Reject==I+1)return (CR_AUDIO_GRAPH_SEND_RESULT){CR_NOT_READY,CrAudioGraphNotAccepted};
 if(I<2){assert(AudioPeerWord(P+12)==1&&AudioPeerWord(P+20)==CR_AUDIO_PCM_MAP&&Bytes_==44&&P[24]==3&&P[26]==1&&AudioPeerWord(P+28)==(I?2:0));assert(AudioPeerWord(P+32)==(I?0x10004000:0x10000000)&&AudioPeerWord(P+36)==1&&AudioPeerWord(P+40)==(I?4096:16384));assert(S->Possible&(1U<<I));}
 else if(I==2){assert(Bytes_==88&&AudioPeerWord(P+12)==S->Instance&&AudioPeerWord(P+44)==0x0800100a&&AudioPeerWord(P+48)==28&&AudioPeerWord(P+68)==0x9001&&AudioPeerWord(P+80)==0x9002&&AudioPeerWord(P+64)==16384);}
 else if(I==3){assert(Bytes_==88&&AudioPeerWord(P+12)==S->Instance&&AudioPeerWord(P+44)==0x0800100c&&AudioPeerWord(P+48)==30&&AudioPeerWord(P+56)==1&&AudioPeerWord(P+60)==0x09001000&&AudioPeerWord(P+64)==18&&AudioPeerWord(P+68)==48000&&P[72]==16&&P[74]==1&&P[75]==1&&P[76]==16&&P[78]==15&&P[80]==1&&P[82]==2&&P[84]==1&&P[85]==2);}
 else if(I==4){assert(Bytes_==72&&AudioPeerWord(P+12)==S->Instance&&AudioPeerWord(P+20)==0x0100100e&&AudioPeerWord(P+44)==0x0800101c&&AudioPeerWord(P+48)==1&&AudioPeerWord(P+56)==8&&AudioPeerWord(P+64)==1&&AudioPeerWord(P+68)==8192);}
 else{assert(F->Graph.Phase==CrAudioGraphEnded&&!F->Graph.GraphPossible&&!F->Graph.Cookie&&Bytes_==28&&AudioPeerWord(P+20)==CR_AUDIO_PCM_UNMAP&&AudioPeerWord(P+24)==(I==5?0x9002:0x9001));}
 ++F->Sent;Reply(F,C,F->Error==I+1?1:0);CR_AUDIO_GRAPH_EPOCH Epoch=S->Epoch;if(F->Drop!=I+1)assert(CrAudioPullReceive(S,&Epoch,F->Late,F->LateBytes)==CR_SUCCESS);
 return (CR_AUDIO_GRAPH_SEND_RESULT){F->Unknown==I+1?CR_DEVICE_ERROR:CR_SUCCESS,F->Unknown==I+1?CrAudioGraphUnknown:CrAudioGraphAccepted};
}
static VOID GraphStep(FIXTURE *F){(VOID)CrAudioGraphTick(&F->Graph,F->Now++);AudioPeerProgress(&F->Peer);}
static VOID GraphUntil(FIXTURE *F,UINT32 Phase){for(UINT32 I=0;I<1000&&F->Graph.Phase!=Phase;++I)GraphStep(F);assert(F->Graph.Phase==Phase);}
static FIXTURE *New(VOID){FIXTURE *F=calloc(1,sizeof(*F));assert(F);AudioPeerInit(&F->Peer,0);assert(CrAudioGraphPortInit(&F->Port,F,0x2010)==CR_SUCCESS);assert(CrAudioGraphInitPull(&F->Graph,&F->Port,F,&F->Peer.Binding,&F->Peer.Epoch,1000,Acdb,Bytes,&Request,Work,WorkBytes,&F->Storage)==CR_SUCCESS);AudioPeerAttach(&F->Peer,&F->Graph);GraphUntil(F,CrAudioGraphConfigured);
 CR_AUDIO_PULL_BINDING B={F,Current,Send};CR_AUDIO_PULL_MEMORY M={0x110000000ULL,0x110004000ULL,16384,8192};UINT64 Before=F->Port.NextToken;assert(CrAudioPullInit(&F->Pull,&F->Graph,&B,&M,1000)==CR_SUCCESS&&F->Port.NextToken==Before+7);return F;}
static VOID Step(FIXTURE *F){(VOID)CrAudioPullTick(&F->Pull,F->Now++);}
static VOID Ready(FIXTURE *F){for(UINT32 I=0;I<20&&!F->Pull.Ready;++I)Step(F);assert(F->Pull.Ready&&F->Pull.Possible==3);}
static VOID GraphClose(FIXTURE *F){assert(CrAudioGraphRequestClose(&F->Graph)==CR_SUCCESS);GraphUntil(F,CrAudioGraphEnded);}
static VOID Retire(FIXTURE *F){assert(CrAudioPullClose(&F->Pull)==CR_SUCCESS);if(F->Graph.Phase!=CrAudioGraphEnded)GraphClose(F);for(UINT32 I=0;I<20&&!F->Pull.Ended;++I)Step(F);assert(F->Pull.Ended&&!F->Pull.Possible&&!F->Pull.MapHandle[0]&&!F->Pull.MapHandle[1]);AudioPeerDetach(&F->Peer);free(F);}
static VOID Normal(VOID){FIXTURE *F=New();Ready(F);assert(CrAudioGraphRequestPrepare(&F->Graph)==CR_SUCCESS);GraphUntil(F,CrAudioGraphPrepared);assert(CrAudioGraphRequestStart(&F->Graph)==CR_SUCCESS);GraphUntil(F,CrAudioGraphRunning);assert(CrAudioGraphRequestStop(&F->Graph)==CR_SUCCESS);GraphUntil(F,CrAudioGraphStopped);Retire(F);}
static VOID Failures(VOID){for(UINT32 Phase=0;Phase<7;++Phase){FIXTURE *F=New();F->Drop=Phase+1;if(Phase>=5){Ready(F);GraphClose(F);assert(CrAudioPullClose(&F->Pull)==CR_SUCCESS);}for(UINT32 I=0;I<20&&!F->Attempts[Phase];++I)Step(F);assert(F->Attempts[Phase]==1);F->Now=F->Pull.Commands[Phase].Deadline;Step(F);assert(F->Pull.Failed&&!F->Pull.Ready);UINT32 Before=F->Sent;for(UINT32 I=0;I<5;++I)Step(F);assert(F->Sent==Before);CR_AUDIO_GRAPH_EPOCH Epoch=F->Pull.Epoch;assert(CrAudioPullReceive(&F->Pull,&Epoch,F->Late,F->LateBytes)==CR_SUCCESS);F->Drop=0;Retire(F);}}
static VOID Lost(VOID){FIXTURE *F=New();Step(F);F->Lost=TRUE;Step(F);assert(F->Pull.Lost&&F->Pull.Possible==1);F->Lost=FALSE;GraphClose(F);assert(CrAudioPullClose(&F->Pull)==CR_SUCCESS);UINT32 Before=F->Sent;for(UINT32 I=0;I<20;++I)Step(F);assert(!F->Pull.Ended&&F->Pull.Possible==1&&F->Sent==Before);AudioPeerDetach(&F->Peer);free(F);}
static VOID Reject(VOID){FIXTURE *F=New();F->Reject=1;for(UINT32 I=0;I<5;++I)Step(F);assert(!F->Pull.Possible&&!F->Sent&&F->Attempts[0]==5);Retire(F);}
int main(int Argc,char **Argv){assert(Argc==3);FILE *F=fopen(Argv[1],"rb");assert(F&&!fseek(F,0,SEEK_END));long N=ftell(F);assert(N>0);rewind(F);Bytes=N;Acdb=malloc(Bytes);assert(Acdb&&fread(Acdb,1,Bytes,F)==Bytes&&!fclose(F));F=fopen(Argv[2],"rb");assert(F&&fread(&Request,1,sizeof(Request),F)==sizeof(Request)&&fgetc(F)==EOF&&!fclose(F));WorkBytes=CrAudioCalWorkspaceBytes();Work=malloc(WorkBytes);assert(Work);Normal();Failures();Lost();Reject();free(Work);free(Acdb);puts("Actual typed graph/Pull: MAP/config/media/event/targeted graph-close/reverse UNMAP, 7 late timeout cases, sticky source loss, no-effect backpressure PASS");return 0;}
