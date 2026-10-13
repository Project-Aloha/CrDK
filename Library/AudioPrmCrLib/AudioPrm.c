/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors.
 * Packet layout and resource values from OnePlus audio-kernel 877203a7cf9b7997,
 * audio_prm.c/.h and audio-ext-clk-up.c. See README.md. */
#include <Library/AudioPrmCrLib.h>
CONST CR_AUDIO_PRM_TARGET CrAudioPrmSm8450[CR_AUDIO_PRM_VOTES] = {
  {CR_AUDIO_PRM_CORE, 1, 0}, {CR_AUDIO_PRM_CORE, 2, 0},
  {CR_AUDIO_PRM_CLOCK, 0x30c, 19200000}, {CR_AUDIO_PRM_CLOCK, 0x312, 19200000},
  {CR_AUDIO_PRM_CLOCK, 0x314, 19200000}
};
static UINT32 R32(CONST UINT8 *P) { return P[0] | ((UINT32)P[1]<<8) | ((UINT32)P[2]<<16) | ((UINT32)P[3]<<24); }
static VOID W32(UINT8 *P, UINT32 V) { for (UINT32 I=0;I<4;++I) P[I]=(UINT8)(V>>(I*8)); }
static BOOLEAN Span(CONST VOID *P, UINTN N) { return P && (UINTN)P <= (UINTN)-1-N; }
static BOOLEAN Apart(CONST VOID *A, UINTN N, CONST VOID *B, UINTN M) {
  return Span(A,N)&&Span(B,M)&&((UINTN)A+N<=(UINTN)B||(UINTN)B+M<=(UINTN)A);
}
static BOOLEAN Valid(CONST CR_AUDIO_PRM *P) { return P && P->Self==P; }
static BOOLEAN SourceValid(CONST CR_AUDIO_PRM_SOURCE *S) {
  return S->Identity&&S->Channel&&S->Boot&&S->Image&&S->Binding&&S->Adsp&&S->AudioPd;
}
static BOOLEAN Equal(CONST CR_AUDIO_PRM_SOURCE *A, CONST CR_AUDIO_PRM_SOURCE *B) {
  return A->Identity==B->Identity&&A->Channel==B->Channel&&A->Boot==B->Boot&&A->Image==B->Image&&
    A->Binding==B->Binding&&A->Adsp==B->Adsp&&A->AudioPd==B->AudioPd;
}
CR_STATUS CrAudioPrmBuild(UINT32 Resource, BOOLEAN Release, UINT32 Token, VOID *Buffer, UINTN Capacity, UINT32 *Bytes) {
  if(Resource>=CR_AUDIO_PRM_VOTES||Release>1||!Token||!Bytes||((UINTN)Bytes&3)||
     !Apart(Buffer,Capacity,Bytes,sizeof(*Bytes))) return CR_INVALID_PARAMETER;
  CONST CR_AUDIO_PRM_TARGET *T=&CrAudioPrmSm8450[Resource];
  UINT32 N=T->Parameter==CR_AUDIO_PRM_CORE?60:Release?64:76;
  if(Capacity<N)return CR_BUFFER_TOO_SMALL;
  UINT8 P[CR_AUDIO_PRM_MAX_BYTES]={0};
  W32(P,(N<<8)|0x60);P[4]=2;P[5]=3;W32(P+8,3);W32(P+12,2);W32(P+16,Token);
  W32(P+20,Release?CR_AUDIO_PRM_RELEASE:CR_AUDIO_PRM_REQUEST);
  W32(P+36,N-40);W32(P+40,2);W32(P+44,T->Parameter);W32(P+48,N-56);
  if(T->Parameter==CR_AUDIO_PRM_CORE)W32(P+56,T->Id);
  else { W32(P+56,1);W32(P+60,T->Id);if(!Release){W32(P+64,T->Frequency);W32(P+68,1);} }
  for(UINT32 I=0;I<N;++I)((UINT8 *)Buffer)[I]=P[I];*Bytes=N;return CR_SUCCESS;
}
CR_STATUS CrAudioPrmValidate(CONST VOID *Data, UINTN Bytes, UINT32 *Resource, BOOLEAN *Release, UINT32 *Token) {
  if(!Resource||!Release||!Token||((UINTN)Resource&3)||((UINTN)Token&3)||
     !Apart(Data,Bytes,Resource,sizeof(*Resource))||!Apart(Data,Bytes,Release,sizeof(*Release))||
     !Apart(Data,Bytes,Token,sizeof(*Token))||!Apart(Resource,sizeof(*Resource),Release,sizeof(*Release))||
     !Apart(Resource,sizeof(*Resource),Token,sizeof(*Token))||!Apart(Release,sizeof(*Release),Token,sizeof(*Token)))return CR_INVALID_PARAMETER;
  CR_AUDIO_GPR_HEADER H;CR_STATUS S=CrAudioGprDecode(Data,Bytes,&H);if(S!=CR_SUCCESS)return S;
  if(H.HeaderBytes!=24||H.SourceDomain!=3||H.DestinationDomain!=2||H.SourcePort!=3||H.DestinationPort!=2||
     !H.Token||(H.Opcode!=CR_AUDIO_PRM_REQUEST&&H.Opcode!=CR_AUDIO_PRM_RELEASE))return CR_INVALID_PARAMETER;
  UINT8 P[CR_AUDIO_PRM_MAX_BYTES];UINT32 N;BOOLEAN Rel=H.Opcode==CR_AUDIO_PRM_RELEASE;
  for(UINT32 I=0;I<CR_AUDIO_PRM_VOTES;++I){
    S=CrAudioPrmBuild(I,Rel,H.Token,P,sizeof(P),&N);if(S!=CR_SUCCESS||Bytes!=N)continue;
    UINT32 J=0;for(;J<N&&P[J]==((CONST UINT8 *)Data)[J];++J){}
    if(J==N){*Resource=I;*Release=Rel;*Token=H.Token;return CR_SUCCESS;}
  }
  return CR_INVALID_PARAMETER;
}
CR_STATUS CrAudioPrmInit(CR_AUDIO_PRM *P, CONST CR_AUDIO_PRM_IO *Io) {
  if(!P||P->Self||!Io||!Io->Context||!Io->Current||!Io->Reserve||!Io->Send||!Apart(P,sizeof(*P),Io,sizeof(*Io)))return CR_INVALID_PARAMETER;
  *P=(CR_AUDIO_PRM){.Self=P,.Io=*Io};return CR_SUCCESS;
}
CR_STATUS CrAudioPrmSetDesired(CR_AUDIO_PRM *P, UINT32 Mask) {
  if(!Valid(P)||Mask&~CR_AUDIO_PRM_ALL)return CR_INVALID_PARAMETER;
  if(P->Inside)return CR_BUSY;
  if(P->Lost&&Mask)return CR_NOT_READY;
  /* All clock operations require both hardware cores. Release closes clocks
   * first; a caller cannot accidentally remove a prerequisite still in use. */
  if((Mask&0x1c)&&((Mask&3)!=3))return CR_INVALID_PARAMETER;
  P->Desired=Mask;return CR_SUCCESS;
}
BOOLEAN CrAudioPrmEmpty(CONST CR_AUDIO_PRM *P) {
  if(!Valid(P))return FALSE;
  for(UINT32 I=0;I<CR_AUDIO_PRM_VOTES;++I)if(P->Votes[I].Phase!=CrPrmOff)return FALSE;
  return TRUE;
}
static CR_STATUS Current(CR_AUDIO_PRM *P) {
  if(P->Lost)return CR_NOT_READY;
  CR_AUDIO_PRM_SOURCE S={0};P->Inside=TRUE;CR_STATUS R=P->Io.Current(P->Io.Context,&S);P->Inside=FALSE;
  if(R!=CR_SUCCESS||!SourceValid(&S)){
    if(P->Bound)P->Lost=TRUE;
    return R==CR_SUCCESS?CR_COMPROMISED_DATA:R;
  }
  if(P->Bound&&!Equal(&S,&P->Source)){P->Lost=TRUE;return CR_NOT_READY;}
  if(!P->Bound){P->Source=S;P->Bound=TRUE;}return CR_SUCCESS;
}
CR_STATUS CrAudioPrmStep(CR_AUDIO_PRM *P, UINT64 NowUs) {
  if(!Valid(P))return CR_INVALID_PARAMETER;
  if(P->Inside)return CR_BUSY;
  if(NowUs<P->LastNow){P->Lost=TRUE;return CR_INVALID_PARAMETER;}P->LastNow=NowUs;
  CR_STATUS S=Current(P);if(S!=CR_SUCCESS)return S;
  for(UINT32 I=0;I<CR_AUDIO_PRM_VOTES;++I){
    CR_AUDIO_PRM_VOTE *V=&P->Votes[I];
    if(V->Phase==CrPrmRequestPending||V->Phase==CrPrmReleasePending){
      if(NowUs>=V->Deadline){V->Phase=V->Phase==CrPrmRequestPending?CrPrmRequestUncertain:CrPrmReleaseUncertain;V->LastError=CR_TIMEOUT;}
      return CR_NOT_READY;
    }
    if(V->Phase==CrPrmRequestUncertain||V->Phase==CrPrmReleaseUncertain)return CR_BUSY;
  }
  if(NowUs<P->RetryAt)return CR_NOT_READY;
  UINT32 Index=CR_AUDIO_PRM_VOTES;BOOLEAN Release=FALSE;
  for(UINT32 I=CR_AUDIO_PRM_VOTES;I;--I)if(!(P->Desired&(1U<<(I-1)))&&P->Votes[I-1].Phase==CrPrmGranted){Index=I-1;Release=TRUE;break;}
  if(Index==CR_AUDIO_PRM_VOTES)for(UINT32 I=0;I<CR_AUDIO_PRM_VOTES;++I)if((P->Desired&(1U<<I))&&P->Votes[I].Phase==CrPrmOff){Index=I;break;}
  if(Index==CR_AUDIO_PRM_VOTES)return CR_SUCCESS;
  if(NowUs>(UINT64)-1-500000)return CR_BAD_BUFFER_SIZE;
  UINT32 Token=0;P->Inside=TRUE;S=P->Io.Reserve(P->Io.Context,&P->Source,Index,Release,&Token);P->Inside=FALSE;
  if(S!=CR_SUCCESS||!Token)return S==CR_SUCCESS?CR_COMPROMISED_DATA:S;
  UINT8 Packet[CR_AUDIO_PRM_MAX_BYTES];UINT32 Bytes;
  S=CrAudioPrmBuild(Index,Release,Token,Packet,sizeof(Packet),&Bytes);if(S!=CR_SUCCESS)return S;
  CR_AUDIO_PRM_VOTE *V=&P->Votes[Index];
  *V=(CR_AUDIO_PRM_VOTE){.Phase=Release?CrPrmReleasePending:CrPrmRequestPending,.Token=Token,.Deadline=NowUs+500000};
  /* Journal before Send; even an error after accepted copy retains the vote. */
  CR_AUDIO_PRM_SEND_RESULT Result={CR_DEVICE_ERROR,CrPrmUnknown};
  P->Inside=TRUE;S=P->Io.Send(P->Io.Context,&P->Source,Packet,Bytes,&Result);P->Inside=FALSE;
  V->LastError=S!=CR_SUCCESS?S:Result.Status;
  if(Result.Acceptance==CrPrmNotAccepted){V->Phase=Release?CrPrmGranted:CrPrmOff;P->RetryAt=NowUs+100000;return V->LastError==CR_SUCCESS?CR_NOT_READY:V->LastError;}
  if(Result.Acceptance!=CrPrmAccepted)V->Phase=Release?CrPrmReleaseUncertain:CrPrmRequestUncertain;
  return CR_NOT_READY;
}
CR_STATUS CrAudioPrmReceive(CR_AUDIO_PRM *P, CONST CR_AUDIO_PRM_SOURCE *Source, CONST VOID *Data, UINTN Bytes) {
  if(!Valid(P)||!Source)return CR_INVALID_PARAMETER;
  if(P->Inside)return CR_BUSY;
  if(!P->Bound||P->Lost||!Equal(Source,&P->Source))return CR_NOT_FOUND;
  CR_AUDIO_GPR_HEADER H;CR_STATUS S=CrAudioGprDecode(Data,Bytes,&H);if(S!=CR_SUCCESS)return S;
  if(H.SourceDomain!=2||H.DestinationDomain!=3||H.SourcePort!=2||H.DestinationPort!=3)return CR_NOT_FOUND;
  for(UINT32 I=0;I<CR_AUDIO_PRM_VOTES;++I){
    CR_AUDIO_PRM_VOTE *V=&P->Votes[I];
    BOOLEAN Rel=V->Phase==CrPrmReleasePending||V->Phase==CrPrmReleaseUncertain;
    if(V->Token!=H.Token||(!Rel&&V->Phase!=CrPrmRequestPending&&V->Phase!=CrPrmRequestUncertain))continue;
    UINT32 N=H.PacketBytes-H.HeaderBytes;CONST UINT8 *B=(CONST UINT8 *)Data+H.HeaderBytes;
    UINT32 Op=Rel?CR_AUDIO_PRM_RELEASE:CR_AUDIO_PRM_REQUEST;
    if(H.Opcode==CR_AUDIO_GPR_BASIC_RESPONSE){
      if(N!=8)return CR_COMPROMISED_DATA;if(R32(B)!=Op)return CR_NOT_FOUND;
      if(R32(B+4)==0)return CR_NOT_READY; /* BASIC success never grants a vote. */
    }else if(H.Opcode==(Rel?CR_AUDIO_PRM_RELEASE_RESPONSE:CR_AUDIO_PRM_REQUEST_RESPONSE)){
      if(N<8||N%4)return CR_COMPROMISED_DATA;if(R32(B)!=CrAudioPrmSm8450[I].Parameter)return CR_NOT_FOUND;
      if(R32(B+4)==0){if(N!=8)return CR_COMPROMISED_DATA;V->Phase=Rel?CrPrmOff:CrPrmGranted;V->LastError=CR_SUCCESS;return CR_SUCCESS;}
    }else return CR_NOT_FOUND;
    /* PRM permits partial failures. A nonzero status is not proof that the
     * resource was untouched or fully released, even for a one-resource vote. */
    V->RemoteStatus=R32(B+4);V->LastError=CR_DEVICE_ERROR;
    V->Phase=Rel?CrPrmReleaseUncertain:CrPrmRequestUncertain;return CR_DEVICE_ERROR;
  }
  return CR_NOT_FOUND;
}
CR_STATUS CrAudioPrmCheck(CR_AUDIO_PRM *P, UINT32 Mask) {
  if(!Valid(P)||Mask&~CR_AUDIO_PRM_ALL)return CR_INVALID_PARAMETER;
  if(P->Inside)return CR_BUSY;
  CR_STATUS S=Current(P);if(S!=CR_SUCCESS)return S;
  for(UINT32 I=0;I<CR_AUDIO_PRM_VOTES;++I)if((Mask&(1U<<I))&&P->Votes[I].Phase!=CrPrmGranted)return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrAudioPrmSourceLost(CR_AUDIO_PRM *P) {
  if(!Valid(P))return CR_INVALID_PARAMETER;if(P->Inside)return CR_BUSY;
  P->Lost=TRUE;P->Desired=0;return CR_SUCCESS;
}
CR_STATUS CrAudioPrmRetire(CR_AUDIO_PRM *P) {
  if(!Valid(P))return CR_INVALID_PARAMETER;if(P->Inside)return CR_BUSY;
  if(!P->Lost||P->Desired)return CR_BUSY;
  if(!CrAudioPrmEmpty(P)){
    if(!P->Io.Retired)return CR_BUSY;
    P->Inside=TRUE;CR_STATUS S=P->Io.Retired(P->Io.Context,&P->Source);P->Inside=FALSE;
    if(S!=CR_SUCCESS)return S;
  }
  for(UINT32 I=0;I<CR_AUDIO_PRM_VOTES;++I)P->Votes[I]=(CR_AUDIO_PRM_VOTE){0};
  P->Source=(CR_AUDIO_PRM_SOURCE){0};P->Bound=P->Lost=FALSE;P->RetryAt=0;return CR_SUCCESS;
}
