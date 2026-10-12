/* Host-only scripted GLINK/APM authority and wire peer. BSD-3-Clause.
 * Real compiler/session/GPR/GLINK run unchanged; only remote hardware is modeled.
 * This file is intentionally absent from production INF/props. */
#include "AudioGraphPeer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _KERNEL_MODE
#include <Library/DebugLib.h>
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D, UINTN N){return memset(D,0,N);}
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V){return memset(D,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N){return memcmp(A,B,N);}
VOID EFIAPI MemoryFence(VOID){__sync_synchronize();}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(void)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F,...){(void)L;(void)F;}
#endif
UINT32 AudioPeerWord(CONST UINT8 *P){return (UINT32)P[0]|(UINT32)P[1]<<8|(UINT32)P[2]<<16|(UINT32)P[3]<<24;}
VOID AudioPeerPut(UINT8 *P, UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
static CR_STATUS Get(VOID *C, UINT32 Host, UINT32 Id, UINT32 *N, VOID **A){
  AUDIO_GRAPH_PEER *P=C;assert(Host==2);
  if(Id==478){*N=sizeof(P->Descriptor);*A=P->Descriptor;}
  else if(Id==479){*N=sizeof(P->Tx);*A=P->Tx;}
  else if(Id==480){*N=sizeof(P->Rx);*A=P->Rx;}else return CR_NOT_FOUND;
  return CR_SUCCESS;
}
static CR_STATUS Allocate(VOID *C,UINT32 H,UINT32 Id,UINT32 N,VOID **A){
  (void)C;assert(H==2&&(Id==478||Id==479));assert(N==(Id==478?32:AUDIO_PEER_FIFO));*A=NULL;return CR_ALREADY_STARTED;
}
static VOID Packet(AUDIO_GRAPH_PEER *P,UINT16 Cmd,UINT16 Ch,UINT32 Param,CONST VOID *Data,UINT32 N){
  UINT8 H[8]={(UINT8)Cmd,(UINT8)(Cmd>>8),(UINT8)Ch,(UINT8)(Ch>>8)};AudioPeerPut(H+4,Param);
  UINT32 Size=(N+15)&~7U,Used=(P->Descriptor[3]+AUDIO_PEER_FIFO-P->Descriptor[2])%AUDIO_PEER_FIFO;
  assert(Size<=AUDIO_PEER_FIFO-Used-8);
  for(UINT32 I=0;I<Size;++I)P->Rx[(P->Descriptor[3]+I)%AUDIO_PEER_FIFO]=I<8?H[I]:I<N+8?((CONST UINT8 *)Data)[I-8]:0;
  P->Descriptor[3]=(P->Descriptor[3]+Size)%AUDIO_PEER_FIFO;
}
static VOID Data(AUDIO_GRAPH_PEER *P,UINT16 Ch,UINT32 Intent,CONST UINT8 *B,UINT32 N){
  UINT8 Payload[520];assert(N<=512);AudioPeerPut(Payload,N);AudioPeerPut(Payload+4,0);memcpy(Payload+8,B,N);Packet(P,9,Ch,Intent,Payload,N+8);
}
static VOID Poll(AUDIO_GRAPH_PEER *P){assert(CrGlinkPoll(&P->Link)==CR_SUCCESS);}
static CR_STATUS Receive(VOID *C,CONST UINT8 *B,UINTN N,UINT64 G){
  AUDIO_GRAPH_PEER *P=C;assert(!P->InCallback&&G==P->Service.Generation);P->InCallback=TRUE;++P->Received;
  CR_STATUS Status=CR_SUCCESS;
  if(P->Session&&P->Session->Phase!=CrAudioGraphEnded){CR_AUDIO_GRAPH_EPOCH E=P->Epoch;E.Channel=G;Status=CrAudioGraphReceive(P->Session,&E,B,(UINT32)N);}
  P->InCallback=FALSE;return Status;
}
static VOID State(VOID *C,BOOLEAN Online,UINT64 G,CR_STATUS Reason){
  AUDIO_GRAPH_PEER *P=C;(void)Reason;assert(!P->InCallback);P->InCallback=TRUE;P->Epoch.Channel=G;
  if(P->Session&&P->Session->Phase!=CrAudioGraphEnded){CR_AUDIO_GRAPH_EPOCH E=P->Epoch;CrAudioGraphState(P->Session,&E,Online);}
  P->InCallback=FALSE;
}
static VOID Pmic(VOID *C,CONST UINT8 *B,UINTN N){AUDIO_GRAPH_PEER *P=C;assert(N==1&&B[0]==0xa5);++P->Pmic;}
static CR_STATUS Qrtr(VOID *C,CONST UINT8 *B,UINTN N){AUDIO_GRAPH_PEER *P=C;assert(N==1&&B[0]==0x5a);++P->Qrtr;return CR_SUCCESS;}
static VOID CheckClaim(AUDIO_GRAPH_PEER *P,CONST CR_AUDIO_GRAPH_CLAIM *C,UINT64 Cookie){
  assert(Cookie&&Cookie==P->Cookie&&C->Owner==P->Claim.Owner&&C->Session==P->Claim.Session&&C->HostPort==P->Claim.HostPort&&C->Plan==P->Claim.Plan);
  assert(C->Epoch.Channel==P->Claim.Epoch.Channel&&C->Epoch.Adsp==P->Claim.Epoch.Adsp&&C->Epoch.AudioPd==P->Claim.Epoch.AudioPd);
}
static CR_STATUS Admit(VOID *C,CONST CR_AUDIO_GRAPH_CLAIM *Claim,UINT64 *Cookie){
  AUDIO_GRAPH_PEER *P=C;assert(!P->Cookie&&!P->Graphs&&P->Service.Online);assert(P->Session&&Claim->Owner==P->Session->Port->Owner);
  if(P->AdmitFailure&&!P->AdmitRetained)return CR_DEVICE_ERROR;
  P->Claim=*Claim;P->Cookie=0x10000+Claim->Session;*Cookie=P->Cookie;
  if(P->LoseDuringAdmit){CR_AUDIO_GRAPH_EPOCH E=P->Epoch;E.AudioPd++;assert(CrAudioGraphState(P->Session,&E,FALSE)==CR_SUCCESS);}
  return P->AdmitFailure?CR_DEVICE_ERROR:CR_SUCCESS;
}
static CR_STATUS Retired(VOID *C,CONST CR_AUDIO_GRAPH_CLAIM *Claim,UINT64 Cookie){
  AUDIO_GRAPH_PEER *P=C;CheckClaim(P,Claim,Cookie);
  /* Only AudioPeerResetGraph explicitly retires this retained simulated claim. */
  return P->RetireTestGeneration&&!P->Graphs&&!P->Service.TxLength&&!P->WireBytes&&!P->Link.QueueCount&&P->Descriptor[0]==P->Descriptor[1]?CR_SUCCESS:CR_NOT_READY;
}
static CR_STATUS Release(VOID *C,CONST CR_AUDIO_GRAPH_CLAIM *Claim,UINT64 Cookie){
  AUDIO_GRAPH_PEER *P=C;CheckClaim(P,Claim,Cookie);assert(!P->Graphs);
  if(P->ReleaseBusy)return CR_BUSY;P->Cookie=0;return CR_SUCCESS;
}
static VOID Drain(AUDIO_GRAPH_PEER *P);
static CR_AUDIO_GRAPH_SEND_RESULT Send(VOID *C,CONST CR_AUDIO_GRAPH_CLAIM *Claim,CONST VOID *B,UINT32 N){
  AUDIO_GRAPH_PEER *P=C;CheckClaim(P,Claim,P->Cookie);assert(!P->InCallback&&P->Session->Pending&&P->Session->Active);
  UINT32 Phase=P->Session->PendingPhase;assert(Phase<7);++P->Attempts[Phase];
  assert(CrAudioGraphTick(P->Session,P->Session->Now)==CR_BUSY);
  assert(CrAudioGraphRequestClose(P->Session)==CR_BUSY);
  if(Phase==P->RejectPhase)return (CR_AUDIO_GRAPH_SEND_RESULT){CR_NOT_READY,CrAudioGraphNotAccepted};
  CR_STATUS S=CrGlinkServiceSend(&P->Link,&P->Service,Claim->Epoch.Channel,B,N);
  if(P->LoseDuringSend){CR_AUDIO_GRAPH_EPOCH E=P->Epoch;++E.AudioPd;assert(CrAudioGraphState(P->Session,&E,FALSE)==CR_SUCCESS);}
  if(P->SyncReply&&S==CR_SUCCESS){for(UINT32 I=0;I<100&&(P->Service.TxLength||P->Link.QueueCount);++I){assert(CrGlinkServiceTx(&P->Link)==CR_SUCCESS);Drain(P);}Poll(P);}
  if(Phase==P->OverridePhase&&S==CR_SUCCESS)return (CR_AUDIO_GRAPH_SEND_RESULT){P->OverrideStatus,P->OverrideAcceptance};
  if(Phase==P->UnknownPhase&&S==CR_SUCCESS)return (CR_AUDIO_GRAPH_SEND_RESULT){CR_DEVICE_ERROR,CrAudioGraphUnknown};
  return (CR_AUDIO_GRAPH_SEND_RESULT){S,S==CR_SUCCESS?CrAudioGraphAccepted:S==CR_NOT_READY||S==CR_BUSY?CrAudioGraphNotAccepted:CrAudioGraphUnknown};
}
static VOID Reply(AUDIO_GRAPH_PEER *P,CONST UINT8 *Request,UINT32 Phase){
  UINT32 Op=AudioPeerWord(Request+20),Status=Phase==P->ErrorPhase?P->DspError:0;
  if(Phase==CrAudioGraphClose&&P->CloseDspError)Status=P->CloseDspError;
  UINT32 N=Phase==CrAudioGraphSpf&&!Status?28:32;memset(P->SavedReply,0,N);
  AudioPeerPut(P->SavedReply,(N<<8)|0x60);P->SavedReply[4]=3;P->SavedReply[5]=2;
  AudioPeerPut(P->SavedReply+8,1);AudioPeerPut(P->SavedReply+12,AudioPeerWord(Request+8));AudioPeerPut(P->SavedReply+16,AudioPeerWord(Request+16));
  AudioPeerPut(P->SavedReply+20,N==28?CR_AUDIO_APM_SPF_STATE_RESPONSE:CR_AUDIO_GPR_BASIC_RESPONSE);
  AudioPeerPut(P->SavedReply+24,N==28?1:Op);if(N==32)AudioPeerPut(P->SavedReply+28,Status);P->ReplyBytes=N;
  if(Phase!=P->DropPhase)Data(P,91,1+(P->Intent++%20),P->SavedReply,N);
}
/* Independent peer decoder/observer: command bodies are copied from actual
 * outbound FIFO, not from the session or its Send argument. */
static VOID Command(AUDIO_GRAPH_PEER *P){
  UINT32 N=P->WireBytes,Op=AudioPeerWord(P->Wire+20),Phase=P->Session->PendingPhase;
  assert(N>=24&&AudioPeerWord(P->Wire)==((N<<8)|0x60)&&P->Wire[4]==2&&P->Wire[5]==3);
  assert(Op==(Phase==CrAudioGraphSpf?0x01001021:Phase==CrAudioGraphOpen?0x01001000:Phase==CrAudioGraphClose?0x01001004:0x01001006));
  ++P->Sent[Phase];if(N>P->MaxPacket)P->MaxPacket=N;
  if(Phase==CrAudioGraphOpen){assert(!P->Graphs);P->Graphs=P->Claim.Plan->SubgraphCount;++P->OpenCount;}
  if(Phase==CrAudioGraphClose){
    /* Validate actual GSL SUB_GRAPH_LIST plus exact inter-SG connection bytes. */
    CONST CR_AUDIO_CAL_PLAN *Plan=P->Claim.Plan;UINT32 Count=AudioPeerWord(P->Wire+56),Bytes=4*(1+Count),Padded=(Bytes+7)&~7U;
    assert(AudioPeerWord(P->Wire+40)==1&&AudioPeerWord(P->Wire+44)==0x08001005&&AudioPeerWord(P->Wire+48)==Bytes&&AudioPeerWord(P->Wire+52)==0&&Count==Plan->SubgraphCount);
    for(UINT32 I=0;I<Count;++I)assert(AudioPeerWord(P->Wire+60+4*I)==Plan->Subgraphs[I].Id);
    for(UINT32 I=Bytes;I<Padded;++I)assert(P->Wire[56+I]==0);
    UINT32 Rest=Plan->OpenBytes-40-Plan->OpenPropertyBytes;assert(N==56+Padded+Rest);
    assert(!memcmp(P->Wire+56+Padded,Plan->Open+40+Plan->OpenPropertyBytes,Rest));
    if(Phase!=P->ErrorPhase&&!P->CloseDspError)P->Graphs=0;else {P->CloseFailed=TRUE;if(P->Graphs>1)--P->Graphs;}++P->CloseCount;
  }
  Reply(P,P->Wire,Phase);
}
static VOID Drain(AUDIO_GRAPH_PEER *P){
  while(P->Descriptor[0]!=P->Descriptor[1]){
    if(P->FreezeAfterFragments&&P->Fragments>=P->FreezeAfterFragments)return;
    UINT32 Tail=P->Descriptor[0],Available=(P->Descriptor[1]+AUDIO_PEER_FIFO-Tail)%AUDIO_PEER_FIFO;UINT8 H[16];assert(Available>=8);
    for(UINT32 I=0;I<16;++I)H[I]=P->Tx[(Tail+I)%AUDIO_PEER_FIFO];
    UINT32 Cmd=H[0]|(UINT32)H[1]<<8,Ch=H[2]|(UINT32)H[3]<<8,Param=AudioPeerWord(H+4),N=8;
    if(Cmd==2)N+=Param&0xffff;if(Cmd==5)N+=Param*8;
    if(Cmd==9||Cmd==12){
      UINT32 Chunk=AudioPeerWord(H+8),Left=AudioPeerWord(H+12);N+=8+Chunk;assert(Ch==P->Service.Local&&Chunk<=1024&&Chunk<=sizeof(P->Wire)-P->WireBytes);
      assert(Cmd==(P->WireBytes?12U:9U));for(UINT32 I=0;I<Chunk;++I)P->Wire[P->WireBytes+I]=P->Tx[(Tail+16+I)%AUDIO_PEER_FIFO];
      P->WireBytes+=Chunk;++P->Fragments;assert(P->WireBytes+Left<=sizeof(P->Wire));
      if(!Left){Command(P);P->WireBytes=0;Packet(P,14,91,Param,NULL,0);}
    }
    N=(N+7)&~7U;assert(N<=Available);P->Descriptor[0]=(Tail+N)%AUDIO_PEER_FIFO;
  }
}
VOID AudioPeerProgress(AUDIO_GRAPH_PEER *P){
  assert(CrGlinkServiceTx(&P->Link)==CR_SUCCESS);Drain(P);Poll(P);Drain(P);
}
VOID AudioPeerInit(AUDIO_GRAPH_PEER *P,UINT32 Handshake){
  memset(P,0,sizeof(*P));P->Handshake=Handshake;P->Epoch.Adsp=7;P->Epoch.AudioPd=11;P->DspError=1;
  P->Binding=(CR_AUDIO_GRAPH_BINDING){P,Send,Admit,Retired,Release};P->Smem=(CR_GLINK_SMEM){Get,Allocate,P};P->Link.Context=P;P->Link.Receive=Pmic;P->Link.QrtrReceive=Qrtr;
  assert(CrGlinkBind(&P->Link,&P->Smem)==CR_SUCCESS);Poll(P);Drain(P);
  Packet(P,0,1,1,NULL,0);Packet(P,1,1,1,NULL,0);
  Packet(P,2,77,sizeof("PMIC_RTR_ADSP_APPS"),"PMIC_RTR_ADSP_APPS",sizeof("PMIC_RTR_ADSP_APPS"));Packet(P,4,1,0,NULL,0);
  Packet(P,2,78,sizeof("IPCRTR"),"IPCRTR",sizeof("IPCRTR"));Packet(P,4,2,0,NULL,0);Poll(P);Drain(P);assert(P->Link.Ready);
  if(Handshake==0){Packet(P,2,91,sizeof("adsp_apps"),"adsp_apps",sizeof("adsp_apps"));Packet(P,4,3,0,NULL,0);Poll(P);Drain(P);}
  CR_GLINK_SERVICE_CONFIG Config={0};memcpy(Config.Name,"adsp_apps",sizeof("adsp_apps"));Config.IntentCount=20;Config.IntentBytes=512;Config.RxStorage=P->IntentStorage;Config.RxBytes=sizeof(P->IntentStorage);Config.TxStorage=P->SendStorage;Config.TxBytes=sizeof(P->SendStorage);Config.Receive=Receive;Config.State=State;Config.Context=P;
  assert(CrGlinkServiceRegister(&P->Link,&P->Service,&Config)==CR_SUCCESS);
  if(Handshake==1){Poll(P);Drain(P);}
  if(Handshake){Packet(P,2,91,sizeof("adsp_apps"),"adsp_apps",sizeof("adsp_apps"));Packet(P,4,P->Service.Local,0,NULL,0);}
  Poll(P);Drain(P);assert(P->Service.Online);
  UINT8 Intent[8];AudioPeerPut(Intent,65536);AudioPeerPut(Intent+4,0);Packet(P,5,91,1,Intent,sizeof(Intent));Poll(P);Drain(P);
}
VOID AudioPeerAttach(AUDIO_GRAPH_PEER *P,CR_AUDIO_GRAPH_SESSION *S){P->Session=S;CR_AUDIO_GRAPH_EPOCH E=P->Epoch;assert(CrAudioGraphState(S,&E,TRUE)==CR_SUCCESS);}
VOID AudioPeerLateReply(AUDIO_GRAPH_PEER *P){assert(P->ReplyBytes);Data(P,91,1+(P->Intent++%20),P->SavedReply,P->ReplyBytes);Poll(P);Drain(P);}
VOID AudioPeerUnrelated(AUDIO_GRAPH_PEER *P,UINT32 Intent){
  UINT8 R[24]={0};AudioPeerPut(R,(24<<8)|0x60);R[4]=3;R[5]=2;AudioPeerPut(R+20,0x03000001);
  Data(P,91,Intent,R,sizeof(R));UINT8 A=0xa5,B=0x5a;Data(P,77,1,&A,1);Data(P,78,1,&B,1);Poll(P);Drain(P);
}
VOID AudioPeerResetGraph(AUDIO_GRAPH_PEER *P){assert(P->Cookie);P->Graphs=0;P->RetireTestGeneration=TRUE;}
VOID AudioPeerDetach(AUDIO_GRAPH_PEER *P){
  /* Deliberately allowed before graph retirement in tests: byte End must not
   * make the session/authority forget a graph. Storage remains allocated. */
  P->Session=NULL;CR_STATUS S=CrGlinkServiceUnregister(&P->Link,&P->Service);assert(S==CR_NOT_READY||S==CR_SUCCESS);
  if(S!=CR_SUCCESS){assert(CrGlinkServiceTx(&P->Link)==CR_SUCCESS);Drain(P);Packet(P,11,P->Service.Local,0,NULL,0);Packet(P,3,91,0,NULL,0);Poll(P);Drain(P);assert(CrGlinkServiceUnregister(&P->Link,&P->Service)==CR_SUCCESS);}
  assert(!P->Service.Registered&&P->Link.Ready);
}
