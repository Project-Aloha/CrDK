/* SPDX-License-Identifier: BSD-3-Clause */
#include <Library/AudioPullCrLib.h>
#include <oskal/cr_string.h>
#define MAX32 0xffffffffU
#define MAXN ((UINTN)-1)
#define SET_CFG 0x01001006U
#define REGISTER 0x0100100eU
#define WATERMARK 0x0800101cU
static UINT32 R32(CONST UINT8 *P){return (UINT32)P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static VOID W32(UINT8 *P,UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>(I*8));}
static VOID W16(UINT8 *P,UINT32 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
static BOOLEAN Range(CONST VOID *P,UINTN N){return P&&(UINTN)P<=MAXN-N;}
static BOOLEAN Apart(CONST VOID *P,UINTN N,CONST VOID *Q,UINTN M){return (UINTN)P>=(UINTN)Q+M||(UINTN)Q>=(UINTN)P+N;}
static BOOLEAN Zero(CONST VOID *P,UINTN N){CONST UINT8 *B=P;for(UINTN I=0;I<N;++I)if(B[I])return FALSE;return TRUE;}
static BOOLEAN Epoch(CONST CR_AUDIO_GRAPH_EPOCH *A,CONST CR_AUDIO_GRAPH_EPOCH *B){return A->Channel==B->Channel&&A->Adsp==B->Adsp&&A->AudioPd==B->AudioPd;}
static BOOLEAN Valid(CR_AUDIO_PULL_SESSION *S){return Range(S,sizeof(*S))&&!((UINTN)S&(sizeof(UINTN)-1))&&S->Self==(UINTN)S&&S->Graph&&S->Graph->Self==(UINTN)S->Graph&&S->Graph->Storage&&S->Graph->Port&&(S->Graph->Port->PullOwner==S||(S->Ended&&!S->Graph->Port->PullOwner))&&(S->Graph->Port->Session==S->Graph||(S->Graph->Phase==CrAudioGraphEnded&&!S->Graph->Port->Session))&&S->Graph->Claim.Session==S->GraphSession&&Epoch(&S->Epoch,&S->Graph->Claim.Epoch);}
static VOID Fail(CR_AUDIO_PULL_SESSION *S,CR_STATUS E){S->Failed=TRUE;S->Ready=FALSE;if(S->Error==CR_SUCCESS)S->Error=E;}
static VOID Header(CR_AUDIO_PULL_SESSION *S,CR_AUDIO_PULL_COMMAND *C,UINT32 Bytes,UINT32 Opcode,UINT32 Destination){
 cr_memset(C->Packet,0,sizeof(C->Packet));C->Bytes=Bytes;C->Opcode=Opcode;C->Destination=Destination;
 W32(C->Packet,(Bytes<<8)|0x60);C->Packet[4]=2;C->Packet[5]=3;W32(C->Packet+8,S->Graph->Claim.HostPort);W32(C->Packet+12,Destination);W32(C->Packet+16,C->Token);W32(C->Packet+20,Opcode);W32(C->Packet+36,Bytes-40);
}
static CR_STATUS Build(CR_AUDIO_PULL_SESSION *S,UINT32 Index){
 CR_AUDIO_PULL_COMMAND *C=&S->Commands[Index];if(C->Prepared)return CR_SUCCESS;
 if(Index<2||Index>=5){UINT32 Map=Index<2?Index:6-Index;CR_AUDIO_PCM_REQUEST R={0};
  R.Operation=Index<2?CrAudioPcmMap:CrAudioPcmUnmap;R.HostPort=S->Graph->Claim.HostPort;R.EndpointPort=1;R.Token=C->Token;
  if(Index<2){R.Address=Map?S->Memory.PositionAddress:S->Memory.RingAddress;R.Bytes=Map?4096:S->Memory.RingBytes;R.Flags=Map?2:0;}else R.MapHandle=S->MapHandle[Map];
  CR_STATUS E=CrAudioPcmBuild(&R,C->Packet,sizeof(C->Packet));if(E!=CR_SUCCESS)return E;
  C->Bytes=Index<2?CR_AUDIO_PCM_MAP_PACKET_BYTES:CR_AUDIO_PCM_UNMAP_PACKET_BYTES;C->Opcode=Index<2?CR_AUDIO_PCM_MAP:CR_AUDIO_PCM_UNMAP;C->Destination=1;
 }else if(Index==2){Header(S,C,88,SET_CFG,S->Instance);W32(C->Packet+40,S->Instance);W32(C->Packet+44,0x0800100a);W32(C->Packet+48,28);
  W32(C->Packet+56,(UINT32)S->Memory.RingAddress);W32(C->Packet+60,(UINT32)(S->Memory.RingAddress>>32));W32(C->Packet+64,S->Memory.RingBytes);W32(C->Packet+68,S->MapHandle[0]);
  W32(C->Packet+72,(UINT32)S->Memory.PositionAddress);W32(C->Packet+76,(UINT32)(S->Memory.PositionAddress>>32));W32(C->Packet+80,S->MapHandle[1]);
 }else if(Index==3){Header(S,C,88,SET_CFG,S->Instance);W32(C->Packet+40,S->Instance);W32(C->Packet+44,0x0800100c);W32(C->Packet+48,30);
  W32(C->Packet+56,1);W32(C->Packet+60,0x09001000);W32(C->Packet+64,18);W32(C->Packet+68,48000);W16(C->Packet+72,16);C->Packet[74]=1;C->Packet[75]=1;
  W16(C->Packet+76,16);W16(C->Packet+78,15);W16(C->Packet+80,1);W16(C->Packet+82,2);C->Packet[84]=1;C->Packet[85]=2;
 }else {Header(S,C,72,REGISTER,S->Instance);W32(C->Packet+40,S->Instance);W32(C->Packet+44,WATERMARK);W32(C->Packet+48,1);W32(C->Packet+56,8);W32(C->Packet+64,1);W32(C->Packet+68,S->Memory.Watermark);}
 C->Prepared=TRUE;return CR_SUCCESS;
}
CR_STATUS CrAudioPullInit(CR_AUDIO_PULL_SESSION *S,CR_AUDIO_GRAPH_SESSION *G,CONST CR_AUDIO_PULL_BINDING *B,CONST CR_AUDIO_PULL_MEMORY *M,UINT64 Timeout){
 CONST VOID *P[]={S,G,B,M};UINTN N[]={sizeof(*S),sizeof(*G),sizeof(*B),sizeof(*M)};
 for(UINT32 I=0;I<4;++I){if(!Range(P[I],N[I])||((UINTN)P[I]&(sizeof(UINTN)-1)))return CR_INVALID_PARAMETER;for(UINT32 J=0;J<I;++J)if(!Apart(P[I],N[I],P[J],N[J]))return CR_INVALID_PARAMETER;}
 if(!Zero(S,sizeof(*S))||!Timeout||!B->Context||!B->Current||!B->Send||G->Self!=(UINTN)G||!G->Port||G->Port->Self!=(UINTN)G->Port||G->Port->Session!=G||!G->Storage||G->Phase!=CrAudioGraphConfigured||G->Failed||G->Lost||!G->Cookie)return CR_INVALID_PARAMETER;
 if(G->Port->PullOwner)return CR_BUSY;
 if(!Apart(S,sizeof(*S),G->Port,sizeof(*G->Port))||!Apart(S,sizeof(*S),G->Storage,sizeof(*G->Storage)))return CR_INVALID_PARAMETER;
 CONST CR_AUDIO_CAL_PLAYBACK_ENDPOINTS *E=&G->Storage->Playback;
 if(E->MemoryModule!=0x07001006||!E->MemoryInstance||E->CodecModule!=0x07001023||!E->CodecInstance)return CR_UNSUPPORTED;
 if((M->RingAddress>>32)!=1||(M->PositionAddress>>32)!=1||!M->RingBytes||M->RingBytes>1024*1024||(M->RingBytes&4095)||(M->RingAddress&4095)||(M->PositionAddress&4095)||
    (UINT32)M->RingAddress<0x10000000U||(UINT32)M->RingAddress>0x20000000U-M->RingBytes||(UINT32)M->PositionAddress<0x10000000U||(UINT32)M->PositionAddress>0x1ffff000U||
    (M->RingAddress<M->PositionAddress+4096&&M->PositionAddress<M->RingAddress+M->RingBytes)||!M->Watermark||M->Watermark>=M->RingBytes||(M->Watermark&3))return CR_INVALID_PARAMETER;
 if(G->Port->NextToken>MAX32-(CR_AUDIO_PULL_COMMANDS-1U))return CR_OUT_OF_RESOURCES;
 G->Port->PullOwner=S;S->Self=(UINTN)S;S->GraphSession=G->Claim.Session;S->Graph=G;S->Epoch=G->Claim.Epoch;S->Binding=*B;S->Memory=*M;S->Timeout=Timeout;S->Instance=E->MemoryInstance;
 for(UINT32 I=0;I<CR_AUDIO_PULL_COMMANDS;++I)S->Commands[I].Token=(UINT32)G->Port->NextToken++;
 return CR_SUCCESS;
}
CR_STATUS CrAudioPullReceive(CR_AUDIO_PULL_SESSION *S,CONST CR_AUDIO_GRAPH_EPOCH *E,CONST VOID *Data,UINT32 Bytes){
 if(!Valid(S)||!Range(E,sizeof(*E))||((UINTN)E&7)||!Range(Data,Bytes)||!Bytes||Bytes>CR_AUDIO_PULL_RX_BYTES||!Apart(S,sizeof(*S),Data,Bytes)||!Apart(S,sizeof(*S),E,sizeof(*E)))return CR_INVALID_PARAMETER;
 if(S->Ended||S->Lost||!Epoch(E,&S->Epoch))return CR_NOT_READY;
 if(S->Count==CR_AUDIO_PULL_MAILBOX){S->Overflow=TRUE;return CR_OUT_OF_RESOURCES;}
 UINT32 At=(S->Read+S->Count)%CR_AUDIO_PULL_MAILBOX;S->Rx[At].Bytes=Bytes;cr_memcpy(S->Rx[At].Data,Data,Bytes);++S->Count;return CR_SUCCESS;
}
static VOID Reply(CR_AUDIO_PULL_SESSION *S,CONST UINT8 *Data,UINT32 Bytes){
 CR_AUDIO_GPR_HEADER H;if(CrAudioGprDecode(Data,Bytes,&H)!=CR_SUCCESS||H.SourceDomain!=2||H.DestinationDomain!=3||H.DestinationPort!=S->Graph->Claim.HostPort)return;
 CONST UINT8 *P=Data+H.HeaderBytes;UINT32 N=Bytes-H.HeaderBytes;
 if(H.Opcode==0x03001000&&H.SourcePort==S->Instance&&S->Commands[4].Complete&&N==12&&R32(P)==WATERMARK&&R32(P+4)==4&&R32(P+8)==S->Memory.Watermark){
  if(S->WatermarkEvents==~(UINT64)0)Fail(S,CR_OUT_OF_RESOURCES);else{++S->WatermarkEvents;S->LastWatermark=R32(P+8);}return;
 }
 for(UINT32 I=0;I<CR_AUDIO_PULL_COMMANDS;++I){CR_AUDIO_PULL_COMMAND *C=&S->Commands[I];
  if(!C->Attempted||C->Acceptance==CrAudioGraphNotAccepted||C->Complete||H.Token!=C->Token||H.SourcePort!=C->Destination)continue;
  if(I<2&&H.Opcode==CR_AUDIO_PCM_MAP_RESPONSE){if(N!=4||!R32(P)){Fail(S,CR_COMPROMISED_DATA);return;}S->MapHandle[I]=R32(P);C->Complete=TRUE;if(S->MapHandle[1-I]==R32(P)){S->Uncertain=TRUE;Fail(S,CR_COMPROMISED_DATA);}return;}
  if(H.Opcode!=CR_AUDIO_GPR_BASIC_RESPONSE)return;if(N!=8){Fail(S,CR_COMPROMISED_DATA);return;}if(R32(P)!=C->Opcode)return;
  if(R32(P+4)){Fail(S,CR_DEVICE_ERROR);return;}if(I<2)return;
  C->Complete=TRUE;if(I>=5){UINT32 Map=6-I;S->Possible&=~(1U<<Map);S->MapHandle[Map]=0;}return;
 }
}
CR_STATUS CrAudioPullClose(CR_AUDIO_PULL_SESSION *S){if(!Valid(S))return CR_INVALID_PARAMETER;S->Closing=TRUE;S->Ready=FALSE;return CR_SUCCESS;}
CR_STATUS CrAudioPullTick(CR_AUDIO_PULL_SESSION *S,UINT64 Now){
 if(!Valid(S)||S->Active)return CR_INVALID_PARAMETER;if(S->Ended)return CR_SUCCESS;
 S->Active=TRUE;if(S->Lost||S->Uncertain)goto Done;if(Now<S->LastNow){Fail(S,CR_INVALID_PARAMETER);goto Done;}S->LastNow=Now;
 if(!S->Binding.Current(S->Binding.Context)){S->Lost=TRUE;Fail(S,CR_NOT_READY);goto Done;}
 if(S->Overflow)Fail(S,CR_OUT_OF_RESOURCES);
 for(UINT32 I=0;I<CR_AUDIO_PULL_COMMANDS;++I){CR_AUDIO_PULL_COMMAND *C=&S->Commands[I];if(C->Prepared&&C->Deadline&&Now>=C->Deadline&&!C->Complete){C->TimedOut=TRUE;Fail(S,CR_TIMEOUT);}}
 if(S->Count){UINT32 At=S->Read;S->Read=(At+1)%CR_AUDIO_PULL_MAILBOX;--S->Count;Reply(S,S->Rx[At].Data,S->Rx[At].Bytes);}
 if(S->Uncertain)goto Done;
 UINT32 Next=0;
 if(S->Closing||S->Failed){
  CR_AUDIO_GRAPH_SESSION *G=S->Graph;
  if(G->Phase!=CrAudioGraphEnded||G->GraphPossible||G->Pending||G->Cookie)goto Done;
  if(!S->Possible){S->Ended=TRUE;S->Graph->Port->PullOwner=NULL;goto Done;}
  if((S->Possible&2)&&!S->MapHandle[1])goto Done;
  Next=(S->Possible&2)?5:6;if(!S->MapHandle[6-Next])goto Done;
 }else{
  while(Next<5&&S->Commands[Next].Complete)++Next;
  if(Next==5){S->Ready=TRUE;goto Done;}
  if(S->Graph->Phase!=CrAudioGraphConfigured){Fail(S,CR_NOT_READY);goto Done;}
 }
 S->Next=Next;CR_AUDIO_PULL_COMMAND *C=&S->Commands[Next];
 if(C->Complete||C->TimedOut||(C->Attempted&&C->Acceptance!=CrAudioGraphNotAccepted))goto Done;
 CR_STATUS E=Build(S,Next);if(E!=CR_SUCCESS){Fail(S,E);goto Done;}
 if(!C->Deadline){if(Now>~(UINT64)0-S->Timeout){Fail(S,CR_INVALID_PARAMETER);goto Done;}C->Deadline=Now+S->Timeout;}
 C->Acceptance=CrAudioGraphNotAccepted;C->Attempted=TRUE;if(Next<2)S->Possible|=1U<<Next;
 CR_AUDIO_GRAPH_SEND_RESULT R=S->Binding.Send(S->Binding.Context,C->Packet,C->Bytes);C->Acceptance=R.Acceptance;
 if(R.Acceptance<CrAudioGraphNotAccepted||R.Acceptance>CrAudioGraphUnknown||(R.Status==CR_SUCCESS&&R.Acceptance!=CrAudioGraphAccepted)){C->Acceptance=CrAudioGraphUnknown;Fail(S,CR_COMPROMISED_DATA);}
 if(Next<2&&C->Acceptance==CrAudioGraphNotAccepted)S->Possible&=~(1U<<Next);
 if(R.Status!=CR_SUCCESS&&R.Status!=CR_NOT_READY&&R.Status!=CR_BUSY)Fail(S,R.Status);
 if(R.Status==CR_SUCCESS&&C->Acceptance!=CrAudioGraphAccepted)Fail(S,CR_COMPROMISED_DATA);
 Done:S->Active=FALSE;return S->Error;
}
