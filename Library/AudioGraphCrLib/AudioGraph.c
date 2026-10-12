/** Retained in-band APM graph lifecycle. No hardware/OS transport owner.
 * Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause */
#include <Library/AudioGraphCrLib.h>
#include <oskal/cr_string.h>

#define MAX64 ((UINT64)~(UINT64)0)
#define MAX32 ((UINT32)~(UINT32)0)
#define OPEN 0x01001000U
#define PREPARE 0x01001001U
#define START 0x01001002U
#define STOP 0x01001003U
#define SET_CFG 0x01001006U
#define CLOSE 0x01001004U
#define SG_LIST 0x08001005U
#define CONNECTION 0x08001004U
#define CONTROL 0x08001061U

static UINT32 Read(CONST UINT8 *P) {
  return (UINT32)P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);
}
static VOID Write(UINT8 *P, UINT32 V) {
  for (UINT32 I=0; I<4; ++I) P[I]=(UINT8)(V>>(8*I));
}
static BOOLEAN Range(CONST VOID *P, UINTN N) {
  return P && N && N<=~(UINTN)0-(UINTN)P;
}
static BOOLEAN Apart(CONST VOID *A, UINTN An, CONST VOID *B, UINTN Bn) {
  return Range(A,An)&&Range(B,Bn)&&!((UINTN)A<(UINTN)B+Bn&&(UINTN)B<(UINTN)A+An);
}
static BOOLEAN Aligned(CONST VOID *P, UINTN N) { return P&&((UINTN)P&(N-1))==0; }
static BOOLEAN Zero(CONST VOID *P, UINTN N) {
  CONST UINT8 *B=P; for (UINTN I=0; I<N; ++I) if(B[I])return FALSE; return TRUE;
}
static BOOLEAN EpochValid(CONST CR_AUDIO_GRAPH_EPOCH *E) { return E->Channel&&E->Adsp&&E->AudioPd; }
static BOOLEAN EpochSame(CONST CR_AUDIO_GRAPH_EPOCH *A, CONST CR_AUDIO_GRAPH_EPOCH *B) {
  return A->Channel==B->Channel&&A->Adsp==B->Adsp&&A->AudioPd==B->AudioPd;
}
static BOOLEAN PortValid(CONST CR_AUDIO_GRAPH_PORT *P) {
  return Aligned(P,sizeof(UINTN))&&Range(P,sizeof(*P))&&P->Self==(UINTN)P&&P->Owner&&P->HostPort&&P->HostPort!=MAX32&&P->NextToken&&P->NextSession;
}
static BOOLEAN Valid(CONST CR_AUDIO_GRAPH_SESSION *S) {
  return Aligned(S,sizeof(UINTN))&&Range(S,sizeof(*S))&&S->Self==(UINTN)S&&PortValid(S->Port)&&S->Storage&&
    S->Claim.Owner==S->Port->Owner&&(S->Port->Session==S||S->Phase==CrAudioGraphEnded);
}
static BOOLEAN Outside(CONST CR_AUDIO_GRAPH_SESSION *S, CONST VOID *P, UINTN N) {
  return Apart(P,N,S,sizeof(*S))&&Apart(P,N,S->Port,sizeof(*S->Port))&&Apart(P,N,S->Storage,sizeof(*S->Storage));
}
static VOID Fail(CR_AUDIO_GRAPH_SESSION *S, CR_STATUS Error) {
  if(!S->Failed)S->Error=Error; S->Failed=TRUE; S->Stop=TRUE;
}
static UINT32 Opcode(UINT32 Phase) {
  return Phase==CrAudioGraphSpf?CR_AUDIO_APM_GET_SPF_STATE:
    Phase==CrAudioGraphPreparing?PREPARE:Phase==CrAudioGraphStarting?START:
    Phase==CrAudioGraphStopping?STOP:
    Phase==CrAudioGraphOpen?OPEN:Phase==CrAudioGraphClose?CLOSE:SET_CFG;
}
static BOOLEAN Control(UINT32 Phase) {
  return Phase==CrAudioGraphPreparing||Phase==CrAudioGraphStarting||Phase==CrAudioGraphStopping;
}
static BOOLEAN Stable(UINT32 Phase) {
  return Phase==CrAudioGraphConfigured||Phase==CrAudioGraphPrepared||
    Phase==CrAudioGraphRunning||Phase==CrAudioGraphStopped;
}
static UINT32 Token(CONST CR_AUDIO_GRAPH_SESSION *S, UINT32 Phase) {
  if(Control(Phase))return S->ControlToken;
  return S->TokenBase+(Phase==CrAudioGraphClose?4U:Phase-CrAudioGraphSpf);
}
static BOOLEAN HasModule(CONST CR_AUDIO_CAL_PLAN *P, UINT32 Iid) {
  for(UINT32 I=0;I<P->ModuleCount;++I)if(P->Modules[I].Instance==Iid)return TRUE;
  return FALSE;
}
/* C1a validates all nested object framing. This independent admission walk
 * additionally requires every data/control endpoint in the owned module map. */
static CR_STATUS Closed(CONST CR_AUDIO_CAL_PLAN *P) {
  UINT32 At=40;
  while(At<P->OpenBytes) {
    if(P->OpenBytes-At<16)return CR_COMPROMISED_DATA;
    UINT32 Iid=Read(P->Open+At),Pid=Read(P->Open+At+4),N=Read(P->Open+At+8),End;
    if(N>P->OpenBytes-At-16)return CR_COMPROMISED_DATA;
    End=At+16+N;
    if(Iid==1&&(Pid==CONNECTION||Pid==CONTROL)) {
      if(N<4)return CR_COMPROMISED_DATA;
      UINT32 Count=Read(P->Open+At+16),Pos=At+20;
      if(Count>N/16)return CR_COMPROMISED_DATA;
      for(UINT32 I=0;I<Count;++I) {
        if(End-Pos<16)return CR_COMPROMISED_DATA;
        if(!HasModule(P,Read(P->Open+Pos))||!HasModule(P,Read(P->Open+Pos+8)))return CR_UNSUPPORTED;
        Pos+=16;
        if(Pid==CONTROL) {
          if(End-Pos<4)return CR_COMPROMISED_DATA;
          UINT32 Props=Read(P->Open+Pos);Pos+=4;
          if(Props>(End-Pos)/8)return CR_COMPROMISED_DATA;
          for(UINT32 J=0;J<Props;++J) {
            if(End-Pos<8)return CR_COMPROMISED_DATA;
            UINT32 Size=Read(P->Open+Pos+4);Pos+=8;
            if(Size>End-Pos)return CR_COMPROMISED_DATA;Pos+=Size;
          }
        }
      }
      if(Pos!=End)return CR_COMPROMISED_DATA;
    }
    UINT32 Pad=(8-(N&7))&7;
    if(Pad>P->OpenBytes-End)return CR_COMPROMISED_DATA;At=End+Pad;
  }
  return At==P->OpenBytes?CR_SUCCESS:CR_COMPROMISED_DATA;
}
static VOID Header(UINT8 *P, UINT32 Size, UINT32 HostPort, UINT32 WireToken, UINT32 Op) {
  cr_memset(P,0,40);Write(P,(Size<<8)|0x60);P[4]=2;P[5]=3;
  Write(P+8,HostPort);Write(P+12,1);Write(P+16,WireToken);Write(P+20,Op);Write(P+36,Size-40);
}
static CR_STATUS ClosePacket(CR_AUDIO_GRAPH_STORAGE *B, UINT32 HostPort, UINT32 WireToken, UINT32 *Size) {
  CONST CR_AUDIO_CAL_PLAN *P=&B->Plan;
  if(!P->SubgraphCount||P->SubgraphCount>CR_AUDIO_CAL_MAX_SUBGRAPHS||P->OpenBytes<40||P->OpenPropertyBytes>P->OpenBytes-40)return CR_COMPROMISED_DATA;
  UINT32 N=4*(1+P->SubgraphCount),AlignedN=(N+7)&~7U;
  UINT32 Connections=P->OpenBytes-40-P->OpenPropertyBytes, Total=40+16+AlignedN;
  if(Connections>CR_AUDIO_CAL_MAX_PACKET_BYTES-Total)return CR_BAD_BUFFER_SIZE;
  Total+=Connections;cr_memset(B->Close,0,Total);Header(B->Close,Total,HostPort,WireToken,CLOSE);
  Write(B->Close+40,1);Write(B->Close+44,SG_LIST);Write(B->Close+48,N);Write(B->Close+56,P->SubgraphCount);
  for(UINT32 I=0;I<P->SubgraphCount;++I)Write(B->Close+60+4*I,P->Subgraphs[I].Id);
  cr_memcpy(B->Close+56+AlignedN,P->Open+40+P->OpenPropertyBytes,Connections);*Size=Total;return CR_SUCCESS;
}
static CR_STATUS ControlPacket(CR_AUDIO_GRAPH_SESSION *S) {
  CONST CR_AUDIO_CAL_PLAN *P=&S->Storage->Plan;
  if(!P->SubgraphCount||P->SubgraphCount>CR_AUDIO_CAL_MAX_SUBGRAPHS)return CR_COMPROMISED_DATA;
  if(S->Port->NextToken>MAX32)return CR_OUT_OF_RESOURCES;
  UINT32 N=4*(1+P->SubgraphCount),Total=56+((N+7)&~7U);
  UINT8 *D=S->Storage->Control;
  S->ControlToken=(UINT32)S->Port->NextToken++;
  cr_memset(D,0,Total);Header(D,Total,S->Claim.HostPort,S->ControlToken,Opcode(S->Phase));
  Write(D+40,1);Write(D+44,SG_LIST);Write(D+48,N);Write(D+56,P->SubgraphCount);
  for(UINT32 I=0;I<P->SubgraphCount;++I)Write(D+60+4*I,P->Subgraphs[I].Id);
  S->ControlBytes=Total;return CR_SUCCESS;
}
CR_STATUS CrAudioGraphPortInit(CR_AUDIO_GRAPH_PORT *P, CONST VOID *Owner, UINT32 HostPort) {
  if(!Aligned(P,sizeof(UINTN))||!Range(P,sizeof(*P))||!Owner||!HostPort||HostPort==MAX32||!Zero(P,sizeof(*P)))return CR_INVALID_PARAMETER;
  P->Self=(UINTN)P;P->Owner=Owner;P->HostPort=HostPort;P->NextToken=1;P->NextSession=1;return CR_SUCCESS;
}
static CR_STATUS GraphInit(BOOLEAN Pull, CR_AUDIO_GRAPH_SESSION *S, CR_AUDIO_GRAPH_PORT *P, CONST VOID *Owner,
                          CONST CR_AUDIO_GRAPH_BINDING *B, CONST CR_AUDIO_GRAPH_EPOCH *E, UINT64 Timeout,
                          CONST VOID *Data, UINTN Bytes, CONST CR_AUDIO_CAL_REQUEST *R,
                          VOID *Work, UINTN WorkBytes, CR_AUDIO_GRAPH_STORAGE *Store) {
  CONST VOID *Objects[]={S,P,B,E,Data,R,Work,Store};
  UINTN Sizes[]={sizeof(*S),sizeof(*P),sizeof(*B),sizeof(*E),Bytes,sizeof(*R),WorkBytes,sizeof(*Store)};
  for(UINT32 I=0;I<8;++I) {
    if(!Range(Objects[I],Sizes[I]))return CR_INVALID_PARAMETER;
    for(UINT32 J=0;J<I;++J)if(!Apart(Objects[I],Sizes[I],Objects[J],Sizes[J]))return CR_INVALID_PARAMETER;
  }
  if(!Aligned(S,sizeof(UINTN))||!Aligned(P,sizeof(UINTN))||!Aligned(B,sizeof(UINTN))||!Aligned(E,8)||
     !Aligned(R,4)||!Aligned(Work,sizeof(UINTN))||!Aligned(Store,4)||!Zero(S,sizeof(*S))||!PortValid(P)||
     Owner!=P->Owner||P->Session||!EpochValid(E)||!Timeout||!B->Context||!B->Send||!B->Admit||!B->Retired||!B->Release)return CR_INVALID_PARAMETER;
  if(P->PullOwner)return CR_BUSY;
  if(P->NextToken>MAX32-4U||P->NextSession==MAX64)return CR_OUT_OF_RESOURCES;
  CR_AUDIO_CAL_REQUEST Request=*R;UINT32 Base=(UINT32)P->NextToken,CloseBytes=0;
  Request.HostPort=P->HostPort;Request.OpenToken=Base+1;Request.CalibrationToken=Base+2;Request.EndpointToken=Base+3;
  CR_AUDIO_CAL_RESULT Cal=Pull?CrAudioCalBuildPullPlan(Data,Bytes,&Request,Work,WorkBytes,&Store->Plan,&Store->Playback):
    CrAudioCalBuildPlan(Data,Bytes,&Request,Work,WorkBytes,&Store->Plan);
  if(Cal!=CrAudioCalOk)return Cal==CrAudioCalMalformed?CR_COMPROMISED_DATA:Cal==CrAudioCalInvalidArgument?CR_INVALID_PARAMETER:CR_UNSUPPORTED;
  CR_STATUS Status=Closed(&Store->Plan);if(Status!=CR_SUCCESS)return Status;
  Status=ClosePacket(Store,P->HostPort,Base+4,&CloseBytes);if(Status!=CR_SUCCESS)return Status;
  if(!Pull)cr_memset(&Store->Playback,0,sizeof(Store->Playback));
  /* No failure follows mutation of the permanent allocator. */
  S->Self=(UINTN)S;S->Port=P;S->Storage=Store;S->Binding=*B;
  S->Claim=(CR_AUDIO_GRAPH_CLAIM){Owner,P->NextSession,P->HostPort,*E,&Store->Plan};
  S->Timeout=Timeout;S->TokenBase=Base;S->Phase=CrAudioGraphSpf;S->CloseBytes=CloseBytes;
  CrAudioGprBuildSpfQuery(P->HostPort,Base,S->Spf,sizeof(S->Spf));
  P->NextToken+=5;P->NextSession++;P->Session=S;return CR_SUCCESS;
}
CR_STATUS CrAudioGraphInit(CR_AUDIO_GRAPH_SESSION *S, CR_AUDIO_GRAPH_PORT *P, CONST VOID *Owner,
    CONST CR_AUDIO_GRAPH_BINDING *B, CONST CR_AUDIO_GRAPH_EPOCH *E, UINT64 Timeout,
    CONST VOID *Data, UINTN Bytes, CONST CR_AUDIO_CAL_REQUEST *R,
    VOID *Work, UINTN WorkBytes, CR_AUDIO_GRAPH_STORAGE *Store) {
  return GraphInit(FALSE,S,P,Owner,B,E,Timeout,Data,Bytes,R,Work,WorkBytes,Store);
}
CR_STATUS CrAudioGraphInitPull(CR_AUDIO_GRAPH_SESSION *S, CR_AUDIO_GRAPH_PORT *P, CONST VOID *Owner,
    CONST CR_AUDIO_GRAPH_BINDING *B, CONST CR_AUDIO_GRAPH_EPOCH *E, UINT64 Timeout,
    CONST VOID *Data, UINTN Bytes, CONST CR_AUDIO_CAL_REQUEST *R,
    VOID *Work, UINTN WorkBytes, CR_AUDIO_GRAPH_STORAGE *Store) {
  return GraphInit(TRUE,S,P,Owner,B,E,Timeout,Data,Bytes,R,Work,WorkBytes,Store);
}
static CR_STATUS Enqueue(CR_AUDIO_GRAPH_SESSION *S, CONST CR_AUDIO_GRAPH_EPOCH *E,
                         CONST VOID *Data, UINT32 Bytes, BOOLEAN State, BOOLEAN Online) {
  if(!Valid(S)||!Aligned(E,8)||!Outside(S,E,sizeof(*E)))return CR_INVALID_PARAMETER;
  if(S->Phase==CrAudioGraphEnded)return CR_NOT_READY;
  if(!State&&(!Bytes||Bytes>CR_AUDIO_GRAPH_RX_BYTES||!Outside(S,Data,Bytes)||!Apart(Data,Bytes,E,sizeof(*E))))return CR_INVALID_PARAMETER;
  if(S->Count==CR_AUDIO_GRAPH_MAILBOX){S->Overflow=TRUE;return CR_OUT_OF_RESOURCES;}
  CR_AUDIO_GRAPH_EVENT *V=&S->Events[(S->Head+S->Count)%CR_AUDIO_GRAPH_MAILBOX];
  V->Epoch=*E;V->State=State;V->Online=Online;V->Bytes=Bytes;
  if(!State)cr_memcpy(V->Data,Data,Bytes);++S->Count;return CR_SUCCESS;
}
CR_STATUS CrAudioGraphReceive(CR_AUDIO_GRAPH_SESSION *S, CONST CR_AUDIO_GRAPH_EPOCH *E, CONST VOID *D, UINT32 N) {
  return Enqueue(S,E,D,N,FALSE,FALSE);
}
CR_STATUS CrAudioGraphState(CR_AUDIO_GRAPH_SESSION *S, CONST CR_AUDIO_GRAPH_EPOCH *E, BOOLEAN Online) {
  return Enqueue(S,E,NULL,0,TRUE,Online);
}
static VOID Complete(CR_AUDIO_GRAPH_SESSION *S, UINT32 Remote) {
  UINT32 Phase=S->PendingPhase;S->Pending=FALSE;++S->Replies;
  if(Remote&&!S->DspStatus){S->DspStatus=Remote;S->DspOpcode=Opcode(Phase);}
  if(Phase==CrAudioGraphClose){S->CleanupDspStatus=Remote;S->CleanupDspOpcode=CLOSE;}
  if(Remote)Fail(S,CR_DEVICE_ERROR);
  if(Phase==CrAudioGraphClose) {
    if(Remote){S->CleanupError=CR_DEVICE_ERROR;S->Phase=CrAudioGraphRecovery;}
    else {S->GraphPossible=FALSE;S->OpenKnown=FALSE;S->Phase=CrAudioGraphReleasing;}
    return;
  }
  if(Phase==CrAudioGraphSpf) {
    if(Remote||S->Failed||S->Stop){S->Phase=CrAudioGraphReleasing;return;}
    S->Phase=CrAudioGraphOpen;return;
  }
  if(Phase==CrAudioGraphOpen&&!Remote)S->OpenKnown=TRUE;
  if(S->Failed||S->Stop){S->Phase=CrAudioGraphClose;return;}
  if(Control(Phase)) {
    S->Phase=Phase==CrAudioGraphPreparing?CrAudioGraphPrepared:
      Phase==CrAudioGraphStarting?CrAudioGraphRunning:CrAudioGraphStopped;
    return;
  }
  S->Phase=Phase==CrAudioGraphOpen?CrAudioGraphCalibration:
    Phase==CrAudioGraphCalibration?CrAudioGraphEndpoint:CrAudioGraphConfigured;
}
static VOID Response(CR_AUDIO_GRAPH_SESSION *S, CONST CR_AUDIO_GRAPH_EVENT *V) {
  if(S->Lost||!EpochSame(&V->Epoch,&S->Claim.Epoch)||!S->Pending||S->Acceptance==CrAudioGraphNotAccepted){++S->Ignored;return;}
  CR_AUDIO_GPR_HEADER H;CR_STATUS Status=CrAudioGprDecode(V->Data,V->Bytes,&H);
  if(Status!=CR_SUCCESS){++S->Malformed;return;}
  if(H.SourceDomain!=2||H.DestinationDomain!=3||H.SourcePort!=1||H.DestinationPort!=S->Claim.HostPort||H.Token!=Token(S,S->PendingPhase)){++S->Ignored;return;}
  if(S->PendingPhase==CrAudioGraphSpf) {
    CR_AUDIO_SPF_REPLY Reply;
    Status=CrAudioGprMatchSpfReply(V->Data,V->Bytes,S->Claim.HostPort,H.Token,&Reply);
    if(Status!=CR_SUCCESS){if(Status==CR_NOT_FOUND)++S->Ignored;else ++S->Malformed;return;}
    if(Reply.Kind==CrAudioSpfBasicAckWithoutState){++S->Ignored;return;}
    if(Reply.Kind==CrAudioSpfNotReady){Fail(S,CR_NOT_READY);Complete(S,0);return;}
    Complete(S,Reply.Kind==CrAudioSpfDspError?Reply.Value:0);return;
  }
  if(H.Opcode!=CR_AUDIO_GPR_BASIC_RESPONSE){++S->Ignored;return;}
  if(H.PacketBytes-H.HeaderBytes!=8){++S->Malformed;return;}
  if(Read(V->Data+H.HeaderBytes)!=Opcode(S->PendingPhase)){++S->Ignored;return;}
  Complete(S,Read(V->Data+H.HeaderBytes+4));
}
static VOID Events(CR_AUDIO_GRAPH_SESSION *S) {
  while(S->Count) {
    CR_AUDIO_GRAPH_EVENT *V=&S->Events[S->Head];
    if(V->State) {
      if(!V->Online||!EpochSame(&V->Epoch,&S->Claim.Epoch)) {
        S->Online=FALSE;S->Lost=TRUE;Fail(S,CR_NOT_READY);
      } else if(!S->Lost)S->Online=TRUE;
    } else Response(S,V);
    S->Head=(S->Head+1)%CR_AUDIO_GRAPH_MAILBOX;--S->Count;
  }
}
static CR_STATUS Release(CR_AUDIO_GRAPH_SESSION *S) {
  S->Phase=CrAudioGraphReleasing;
  if(S->Cookie) {
    CR_STATUS Status=S->Binding.Release(S->Binding.Context,&S->Claim,S->Cookie);
    if(Status!=CR_SUCCESS){S->CleanupError=Status;return Status;}
    S->Cookie=0;
  }
  S->Phase=CrAudioGraphEnded;S->Pending=FALSE;S->Port->Session=NULL;
  return S->Failed?S->Error:CR_SUCCESS;
}
static CR_STATUS Step(CR_AUDIO_GRAPH_SESSION *S, UINT64 Now) {
  if(S->Phase==CrAudioGraphEnded&&!S->Cookie)return S->Failed?S->Error:CR_SUCCESS;
  if(S->TimeKnown&&Now<S->Now){S->Lost=TRUE;Fail(S,CR_INVALID_PARAMETER);}
  S->TimeKnown=TRUE;S->Now=Now;
  if(S->Overflow){S->Lost=TRUE;Fail(S,CR_OUT_OF_RESOURCES);}
  if(S->Pending&&Now>=S->Deadline) {
    Fail(S,CR_TIMEOUT);
    if(S->PendingPhase==CrAudioGraphClose)S->CleanupError=CR_TIMEOUT;
    if(S->Acceptance==CrAudioGraphNotAccepted) {
      S->Pending=FALSE;
      if(S->PendingPhase==CrAudioGraphClose)S->Phase=CrAudioGraphRecovery;
    }
    else if(S->PendingPhase!=CrAudioGraphSpf){S->Phase=CrAudioGraphRecovery;}
    else S->Pending=FALSE; /* Read-only query; its token remains fenced. */
  }
  Events(S);
  if(S->Lost)S->Phase=CrAudioGraphRecovery;
  if(S->Failed&&S->GraphPossible&&S->Cookie) {
    CR_STATUS Retired=S->Binding.Retired(S->Binding.Context,&S->Claim,S->Cookie);
    if(Retired==CR_SUCCESS){S->GraphPossible=FALSE;S->Pending=FALSE;return Release(S);}
  }
  if(S->Lost) {
    if(!S->GraphPossible)return Release(S);
    return S->Error;
  }
  if(S->Stop&&!S->Pending) {
    if(!S->GraphPossible)return Release(S);
    if(S->Phase!=CrAudioGraphRecovery)S->Phase=CrAudioGraphClose;
  }
  if(S->Phase==CrAudioGraphReleasing&&!S->GraphPossible)return Release(S);
  if(S->Pending&&S->Acceptance!=CrAudioGraphNotAccepted)return S->Failed?S->Error:CR_NOT_READY;
  if(S->Phase==CrAudioGraphRecovery)return S->Failed?S->Error:CR_NOT_READY;
  if(!S->Online)return CR_NOT_READY;
  if(Stable(S->Phase))return CR_SUCCESS;
  if(!S->Admitted) {
    if(S->Attempted)return Release(S);
    S->Attempted=TRUE;
    CR_STATUS Status=S->Binding.Admit(S->Binding.Context,&S->Claim,&S->Cookie);
    if(Status!=CR_SUCCESS||!S->Cookie){Fail(S,Status==CR_SUCCESS?CR_COMPROMISED_DATA:Status);return Release(S);}
    S->Admitted=TRUE;
  }
  /* Authority callbacks may synchronously queue a loss/reply. Dispatch it on
   * the next serialized step before issuing any new transport operation. */
  if(S->Count||S->Overflow)return CR_NOT_READY;
  if(!S->Pending) {
    if(Now>MAX64-S->Timeout){Fail(S,CR_INVALID_PARAMETER);S->Phase=CrAudioGraphRecovery;return S->Error;}
    if(Control(S->Phase)) {
      CR_STATUS Status=ControlPacket(S);
      if(Status!=CR_SUCCESS){Fail(S,Status);S->Phase=CrAudioGraphClose;return Status;}
    }
    S->Pending=TRUE;S->PendingPhase=S->Phase;S->Deadline=Now+S->Timeout;
    S->Acceptance=CrAudioGraphNotAccepted;
  }
  CONST VOID *Data;UINT32 Bytes;
  switch(S->PendingPhase) {
    case CrAudioGraphSpf:Data=S->Spf;Bytes=sizeof(S->Spf);break;
    case CrAudioGraphOpen:Data=S->Storage->Plan.Open;Bytes=S->Storage->Plan.OpenBytes;break;
    case CrAudioGraphCalibration:Data=S->Storage->Plan.Calibration;Bytes=S->Storage->Plan.CalibrationBytes;break;
    case CrAudioGraphEndpoint:Data=S->Storage->Plan.Endpoint;Bytes=S->Storage->Plan.EndpointBytes;break;
    case CrAudioGraphClose:Data=S->Storage->Close;Bytes=S->CloseBytes;break;
    case CrAudioGraphPreparing:case CrAudioGraphStarting:case CrAudioGraphStopping:
      Data=S->Storage->Control;Bytes=S->ControlBytes;break;
    default:Fail(S,CR_COMPROMISED_DATA);S->Phase=CrAudioGraphRecovery;return S->Error;
  }
  CR_AUDIO_GRAPH_SEND_RESULT Sent=S->Binding.Send(S->Binding.Context,&S->Claim,Data,Bytes);
  if(Sent.Acceptance<CrAudioGraphNotAccepted||Sent.Acceptance>CrAudioGraphUnknown||
     (Sent.Acceptance==CrAudioGraphNotAccepted&&Sent.Status==CR_SUCCESS))Sent.Acceptance=CrAudioGraphUnknown;
  S->Acceptance=Sent.Acceptance;
  if(S->PendingPhase==CrAudioGraphOpen&&Sent.Acceptance!=CrAudioGraphNotAccepted)S->GraphPossible=TRUE;
  if(Sent.Acceptance==CrAudioGraphUnknown||(Sent.Acceptance==CrAudioGraphAccepted&&Sent.Status!=CR_SUCCESS)) {
    Fail(S,Sent.Status==CR_SUCCESS?CR_DEVICE_ERROR:Sent.Status);S->Phase=CrAudioGraphRecovery;
  } else if(Sent.Acceptance==CrAudioGraphNotAccepted&&Sent.Status!=CR_NOT_READY&&Sent.Status!=CR_BUSY) {
    S->Pending=FALSE;Fail(S,Sent.Status);
    if(S->PendingPhase==CrAudioGraphClose){S->CleanupError=Sent.Status;S->Phase=CrAudioGraphRecovery;}
  }
  return S->Failed?S->Error:CR_NOT_READY;
}
CR_STATUS CrAudioGraphTick(CR_AUDIO_GRAPH_SESSION *S, UINT64 Now) {
  if(!Valid(S))return CR_INVALID_PARAMETER;if(S->Active)return CR_BUSY;
  S->Active=TRUE;CR_STATUS Status=Step(S,Now);S->Active=FALSE;return Status;
}
CR_STATUS CrAudioGraphRequestClose(CR_AUDIO_GRAPH_SESSION *S) {
  if(!Valid(S))return CR_INVALID_PARAMETER;if(S->Active)return CR_BUSY;
  if(S->Phase==CrAudioGraphEnded)return CR_SUCCESS;S->Stop=TRUE;
  if(S->Pending&&S->Acceptance==CrAudioGraphNotAccepted)S->Pending=FALSE;
  return CR_SUCCESS;
}
static CR_STATUS RequestControl(CR_AUDIO_GRAPH_SESSION *S,UINT32 Phase,UINT32 Target) {
  if(!Valid(S))return CR_INVALID_PARAMETER;
  if(S->Active)return CR_BUSY;
  if(S->Stop||S->Failed||S->Lost||!S->GraphPossible||!S->OpenKnown||!S->Cookie)return CR_NOT_READY;
  if(S->Phase==Target&&!S->Pending)return CR_SUCCESS;
  if(S->Phase==Phase)return CR_NOT_READY;
  if(S->Pending)return CR_BUSY;
  BOOLEAN Allowed=Phase==CrAudioGraphPreparing?S->Phase==CrAudioGraphConfigured:
    Phase==CrAudioGraphStarting?(S->Phase==CrAudioGraphPrepared||S->Phase==CrAudioGraphStopped):
    S->Phase==CrAudioGraphRunning;
  if(!Allowed)return CR_NOT_READY;
  S->Phase=Phase;return CR_SUCCESS;
}
CR_STATUS CrAudioGraphRequestPrepare(CR_AUDIO_GRAPH_SESSION *S) {
  return RequestControl(S,CrAudioGraphPreparing,CrAudioGraphPrepared);
}
CR_STATUS CrAudioGraphRequestStart(CR_AUDIO_GRAPH_SESSION *S) {
  return RequestControl(S,CrAudioGraphStarting,CrAudioGraphRunning);
}
CR_STATUS CrAudioGraphRequestStop(CR_AUDIO_GRAPH_SESSION *S) {
  return RequestControl(S,CrAudioGraphStopping,CrAudioGraphStopped);
}
CR_STATUS CrAudioGraphGetStatus(CONST CR_AUDIO_GRAPH_SESSION *S, CR_AUDIO_GRAPH_STATUS *Out) {
  if(!Valid(S)||!Aligned(Out,sizeof(UINTN))||!Outside(S,Out,sizeof(*Out)))return CR_INVALID_PARAMETER;
  CR_AUDIO_GRAPH_STATUS V={S->Failed&&S->Phase!=CrAudioGraphEnded?CrAudioGraphRecovery:S->Phase,
    S->PendingPhase,S->Pending?Token(S,S->PendingPhase):0,S->Acceptance,
    S->DspStatus,S->DspOpcode,S->CleanupDspStatus,S->CleanupDspOpcode,S->Replies,S->Ignored,S->Malformed,S->Error,S->CleanupError,
    S->Cookie,S->Claim.Session,S->Claim.Epoch,S->Pending,S->Failed,S->Lost,S->GraphPossible,S->Cookie!=0};
  *Out=V;return CR_SUCCESS;
}
