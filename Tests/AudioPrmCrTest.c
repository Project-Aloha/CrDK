/* SPDX-License-Identifier: GPL-2.0-only
 * Actual PRM owner; firmware transport is modeled. Packet payload oracle is
 * compiled from the pinned original Qualcomm header, not a mirrored struct. */
#include <Library/AudioPrmCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define __packed __attribute__((packed))
#include <dsp/audio_prm.h>
static unsigned Checks;
#define CHECK(X) do { ++Checks; if(!(X)){fprintf(stderr,"%u: %s\n",__LINE__,#X);abort();} }while(0)
static void Put(UINT8 *P,UINT32 V){for(unsigned I=0;I<4;++I)P[I]=(UINT8)(V>>(I*8));}
static void Encoding(void){
 for(unsigned I=0;I<5;++I)for(unsigned Rel=0;Rel<2;++Rel){
  UINT8 P[80];memset(P,0xa7,sizeof(P));UINT32 N=0,Resource=99,Token=0;BOOLEAN Release=9;
  CHECK(CrAudioPrmBuild(I,(BOOLEAN)Rel,0x12345678,P,sizeof(P),&N)==CR_SUCCESS);
  CHECK(CrAudioPrmValidate(P,N,&Resource,&Release,&Token)==CR_SUCCESS);
  CHECK(Resource==I&&Release==Rel&&Token==0x12345678&&P[N]==0xa7);
  if(I<2){
   prm_cmd_request_hw_core_t V={.payload_header={0,0,0,20},.module_payload_0={PRM_MODULE_INSTANCE_ID,PARAM_ID_RSC_HW_CORE,4,0},.hw_core_id=I?HW_CORE_ID_DCODEC:HW_CORE_ID_LPASS};
   CHECK(N==24+sizeof(V)&&!memcmp(P+24,&V,sizeof(V)));
  }else if(Rel){
   prm_cmd_release_rsc_t V={.payload_header={0,0,0,24},.module_payload_0={PRM_MODULE_INSTANCE_ID,PARAM_ID_RSC_AUDIO_HW_CLK,8,0},.num_clk_id_t={1},.clock_ids_t={{CrAudioPrmSm8450[I].Id}}};
   CHECK(N==24+sizeof(V)&&!memcmp(P+24,&V,sizeof(V)));
  }else{
   prm_cmd_request_rsc_t V={.payload_header={0,0,0,36},.module_payload_0={PRM_MODULE_INSTANCE_ID,PARAM_ID_RSC_AUDIO_HW_CLK,20,0},.num_clk_id_t={1},.clock_ids_t={{CrAudioPrmSm8450[I].Id,19200000,CLOCK_ATTRIBUTE_COUPLE_NO,CLOCK_ROOT_DEFAULT}}};
   CHECK(N==24+sizeof(V)&&!memcmp(P+24,&V,sizeof(V)));
  }
  for(unsigned J=0;J<N;++J){UINT8 Old=P[J];P[J]^=0x80;UINT32 R=99,T=0;BOOLEAN E=9;
   if(J>=16&&J<20){CHECK(CrAudioPrmValidate(P,N,&R,&E,&T)==CR_SUCCESS);}
   else {CHECK(CrAudioPrmValidate(P,N,&R,&E,&T)!=CR_SUCCESS&&R==99&&T==0&&E==9);}P[J]=Old;
  }
 }
}
typedef struct {
 CR_AUDIO_PRM P;CR_AUDIO_PRM_SOURCE Source;
 UINT32 Token,Sends,Index;BOOLEAN Release,Absent,Retired;
 UINT32 Acceptance;CR_STATUS SendStatus;
 UINT8 Packet[76];UINT32 Bytes;
} MODEL;
static CR_STATUS Current(VOID *X,CR_AUDIO_PRM_SOURCE *Out){MODEL *M=X;if(M->Absent)return CR_NOT_READY;*Out=M->Source;return CR_SUCCESS;}
static CR_STATUS Reserve(VOID *X,CONST CR_AUDIO_PRM_SOURCE *S,UINT32 I,BOOLEAN Rel,UINT32 *T){MODEL *M=X;CHECK(S->Boot==M->Source.Boot);M->Index=I;M->Release=Rel;*T=++M->Token;return CR_SUCCESS;}
static CR_STATUS Send(VOID *X,CONST CR_AUDIO_PRM_SOURCE *S,CONST VOID *D,UINT32 N,CR_AUDIO_PRM_SEND_RESULT *Out){
 MODEL *M=X;UINT32 I,T;BOOLEAN Rel;CHECK(S->Boot==M->Source.Boot);CHECK(CrAudioPrmValidate(D,N,&I,&Rel,&T)==CR_SUCCESS);
 CHECK(I==M->Index&&Rel==M->Release&&T==M->Token);CHECK(CrAudioPrmStep(&M->P,0)==CR_BUSY);
 CHECK(M->P.Votes[I].Phase==(Rel?CrPrmReleasePending:CrPrmRequestPending));
 memcpy(M->Packet,D,N);M->Bytes=N;++M->Sends;*Out=(CR_AUDIO_PRM_SEND_RESULT){M->SendStatus,M->Acceptance};return M->SendStatus;
}
static CR_STATUS Retired(VOID *X,CONST CR_AUDIO_PRM_SOURCE *S){MODEL *M=X;CHECK(S->Boot==7);return M->Retired?CR_SUCCESS:CR_BUSY;}
static void Init(MODEL *M){
 memset(M,0,sizeof(*M));M->Source=(CR_AUDIO_PRM_SOURCE){M,5,7,8,9,2,3};M->Acceptance=CrPrmAccepted;
 CR_AUDIO_PRM_IO Io={M,Current,Reserve,Send,Retired};CHECK(CrAudioPrmInit(&M->P,&Io)==CR_SUCCESS);
}
static CR_STATUS Reply(MODEL *M,UINT32 Op,UINT32 Parameter,UINT32 Status,UINT32 Token){
 UINT8 P[32]={0};Put(P,0x2060);P[4]=3;P[5]=2;Put(P+8,2);Put(P+12,3);Put(P+16,Token);Put(P+20,Op);Put(P+24,Parameter);Put(P+28,Status);
 return CrAudioPrmReceive(&M->P,&M->Source,P,sizeof(P));
}
static void Ack(MODEL *M){CHECK(Reply(M,M->Release?CR_AUDIO_PRM_RELEASE_RESPONSE:CR_AUDIO_PRM_REQUEST_RESPONSE,CrAudioPrmSm8450[M->Index].Parameter,0,M->Token)==CR_SUCCESS);}
static void Normal(void){
 MODEL M;Init(&M);CHECK(CrAudioPrmSetDesired(&M.P,4)==CR_INVALID_PARAMETER);CHECK(CrAudioPrmSetDesired(&M.P,31)==CR_SUCCESS);
 for(unsigned I=0;I<5;++I){CHECK(CrAudioPrmStep(&M.P,I*1000)==CR_NOT_READY&&M.Index==I&&!M.Release);CHECK(CrAudioPrmCheck(&M.P,31)==CR_NOT_READY);Ack(&M);}
 CHECK(CrAudioPrmCheck(&M.P,31)==CR_SUCCESS);CHECK(CrAudioPrmStep(&M.P,6000)==CR_SUCCESS&&M.Sends==5);
 CHECK(CrAudioPrmSetDesired(&M.P,0)==CR_SUCCESS);
 for(unsigned I=0;I<5;++I){CHECK(CrAudioPrmStep(&M.P,7000+I*1000)==CR_NOT_READY&&M.Index==4-I&&M.Release);Ack(&M);}
 CHECK(CrAudioPrmEmpty(&M.P));CHECK(CrAudioPrmStep(&M.P,12000)==CR_SUCCESS&&M.Sends==10);
}
static void Faults(void){
 for(unsigned Mode=0;Mode<12;++Mode){
  MODEL M;Init(&M);CHECK(CrAudioPrmSetDesired(&M.P,1)==CR_SUCCESS);
  if(Mode==0)M.Acceptance=CrPrmNotAccepted;
  if(Mode==1)M.Acceptance=CrPrmUnknown;
  if(Mode==2)M.SendStatus=CR_DEVICE_ERROR;
  CHECK(CrAudioPrmStep(&M.P,0)==CR_NOT_READY);
  if(Mode==0){CHECK(CrAudioPrmEmpty(&M.P));CHECK(CrAudioPrmStep(&M.P,99999)==CR_NOT_READY&&M.Sends==1);M.Acceptance=CrPrmAccepted;CHECK(CrAudioPrmStep(&M.P,100000)==CR_NOT_READY&&M.Token==2);Ack(&M);continue;}
  if(Mode==1){CHECK(CrAudioPrmStep(&M.P,1000)==CR_BUSY&&M.Sends==1);Ack(&M);CHECK(CrAudioPrmCheck(&M.P,1)==CR_SUCCESS);continue;}
  if(Mode==2){Ack(&M);CHECK(CrAudioPrmCheck(&M.P,1)==CR_SUCCESS);continue;}
  if(Mode==3){CHECK(Reply(&M,CR_AUDIO_GPR_BASIC_RESPONSE,CR_AUDIO_PRM_REQUEST,0,M.Token)==CR_NOT_READY);CHECK(CrAudioPrmCheck(&M.P,1)==CR_NOT_READY);}
  if(Mode==4){CHECK(Reply(&M,CR_AUDIO_PRM_RELEASE_RESPONSE,CR_AUDIO_PRM_CORE,0,M.Token)==CR_NOT_FOUND);CHECK(Reply(&M,CR_AUDIO_PRM_REQUEST_RESPONSE,CR_AUDIO_PRM_CLOCK,0,M.Token)==CR_NOT_FOUND);CHECK(Reply(&M,CR_AUDIO_PRM_REQUEST_RESPONSE,CR_AUDIO_PRM_CORE,0,M.Token+1)==CR_NOT_FOUND);}
  if(Mode==5){CHECK(CrAudioPrmStep(&M.P,500000)==CR_NOT_READY&&M.P.Votes[0].Phase==CrPrmRequestUncertain);Ack(&M);continue;}
  if(Mode==6){CHECK(Reply(&M,CR_AUDIO_PRM_REQUEST_RESPONSE,CR_AUDIO_PRM_CORE,1,M.Token)==CR_DEVICE_ERROR);CHECK(CrAudioPrmSetDesired(&M.P,0)==CR_SUCCESS);CHECK(CrAudioPrmStep(&M.P,1000)==CR_BUSY&&M.Sends==1);}
  if(Mode>=7&&Mode<=9){Ack(&M);CHECK(CrAudioPrmSetDesired(&M.P,0)==CR_SUCCESS);CHECK(CrAudioPrmStep(&M.P,1000)==CR_NOT_READY&&M.Release);
   if(Mode==7)CHECK(Reply(&M,CR_AUDIO_PRM_RELEASE_RESPONSE,CR_AUDIO_PRM_CORE,1,M.Token)==CR_DEVICE_ERROR);
   if(Mode==8)CHECK(CrAudioPrmStep(&M.P,501000)==CR_NOT_READY);
   if(Mode==9){Ack(&M);CHECK(CrAudioPrmEmpty(&M.P));continue;}
  }
  if(Mode==10){++M.Source.Boot;CHECK(CrAudioPrmStep(&M.P,1000)==CR_NOT_READY&&M.P.Lost);}
  if(Mode==11){M.Absent=TRUE;CHECK(CrAudioPrmStep(&M.P,1000)==CR_NOT_READY&&M.P.Lost);}
  CHECK(CrAudioPrmSourceLost(&M.P)==CR_SUCCESS);UINT32 Sends=M.Sends;
  CHECK(CrAudioPrmStep(&M.P,1000000)==CR_NOT_READY&&M.Sends==Sends);
  CHECK(CrAudioPrmRetire(&M.P)==CR_BUSY&&!CrAudioPrmEmpty(&M.P));
  M.Retired=TRUE;CHECK(CrAudioPrmRetire(&M.P)==CR_SUCCESS&&CrAudioPrmEmpty(&M.P));
 }
}
int main(void){Encoding();Normal();Faults();printf("Actual retained PRM owner: encoding + lifecycle/fault cases, %u checks PASS\n",Checks);return 0;}
