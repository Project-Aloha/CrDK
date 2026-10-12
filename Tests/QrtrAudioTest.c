/* Independent QRTR/QMI audio and charger fixtures. SPDX-License-Identifier: MIT */
#include <ucsi/Qrtr.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#if !defined(_KERNEL_MODE) && !defined(QRTR_AUDIO_COMPOSED)
#include <Library/DebugLib.h>
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) {return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) {return memset(D,0,N);}
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) {return memset(D,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) {return memcmp(A,B,N);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L) {(void)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) {(void)L;(void)F;}
#endif
static unsigned QA_Checks;
#define QCHECK(X) do {++QA_Checks;if(!(X)){fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#X);assert(0);}} while(0)
static CR_QRTR QA_R;
static UINT8 QA_Sent[256][1024];
static UINTN QA_SentBytes[256], QA_SentCount;
static CR_STATUS QA_SendStatus;
static CR_STATUS (*QA_Backend)(CONST UINT8 *,UINTN);
static UINT64 QA_Now;
static void QA_W16(UINT8 *P,UINT16 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
static void QA_W32(UINT8 *P,UINT32 V){for(unsigned I=0;I<4;++I)P[I]=(UINT8)(V>>(I*8));}
static UINT16 QA_R16(CONST UINT8 *P){return P[0]|(UINT16)P[1]<<8;}
static UINT32 QA_R32(CONST UINT8 *P){return QA_R16(P)|(UINT32)QA_R16(P+2)<<16;}
static CR_STATUS QA_Send(VOID *C,CONST VOID *D,UINTN N){
  QCHECK(C==&QA_R&&N<=1024&&QA_SentCount<256);
  if(QA_SendStatus!=CR_SUCCESS)return QA_SendStatus;
  memcpy(QA_Sent[QA_SentCount],D,N);QA_SentBytes[QA_SentCount++]=N;return CR_SUCCESS;
}
static void QA_Tick(UINT64 Now){QCHECK(CrQrtrTick(&QA_R,Now)==CR_SUCCESS);QA_Now=Now;}
static void QA_Drain(void){for(unsigned I=0;QA_R.QueueCount&&I<16;++I)QA_Tick(++QA_Now);QCHECK(!QA_R.QueueCount);}
static CR_STATUS QA_Packet(UINT8 Version,UINT32 Type,UINT32 Node,UINT32 Port,UINT32 Dest,UINT32 Confirm,CONST UINT8 *D,UINTN N){
  UINT8 B[1100]={0};UINTN H=Version==1?32:20,Bytes=H+((N+3)&~(UINTN)3);
  QCHECK(Bytes<=sizeof(B));
  if(Version==1){QA_W32(B,1);QA_W32(B+4,Type);QA_W32(B+8,Node);QA_W32(B+12,Port);QA_W32(B+16,Confirm);QA_W32(B+20,(UINT32)N);QA_W32(B+24,1);QA_W32(B+28,Dest);}
  else{B[0]=3;B[1]=(UINT8)Type;B[2]=(UINT8)Confirm;B[3]=4;QA_W32(B+4,(UINT32)N);QA_W16(B+8,(UINT16)Node);QA_W16(B+10,(UINT16)Port);QA_W16(B+12,1);QA_W16(B+14,(UINT16)Dest);memset(B+16,0xA5,4);}
  memcpy(B+H,D,N);return QA_Backend?QA_Backend(B,Bytes):CrQrtrReceive(&QA_R,B,Bytes);
}
static void QA_Ctrl(UINT32 Type,UINT32 Node,UINT32 Port){UINT8 B[20];QA_W32(B,Type);QA_W32(B+4,0x42);QA_W32(B+8,0x4A01);QA_W32(B+12,Node);QA_W32(B+16,Port);QCHECK(QA_Packet(1,Type,Node,0xFFFFFFFE,0xFFFFFFFE,0,B,20)==CR_SUCCESS);}
static UINTN QA_Tlv(UINT8 *B,UINTN O,UINT8 T,CONST VOID *D,UINT16 N){B[O]=T;QA_W16(B+O+1,N);memcpy(B+O+3,D,N);return O+3+N;}
static void QA_Qmi(UINT8 *B,UINT8 T,UINT16 Token,UINT16 Op,UINTN N){B[0]=T;QA_W16(B+1,Token);QA_W16(B+3,Op);QA_W16(B+5,(UINT16)(N-7));}
static void QA_Response(UINT32 Node,UINT32 Port,UINT32 Dest,UINT16 Token,UINT32 State,UINT16 Result,BOOLEAN HaveState){
  UINT8 B[40],V[4];QA_W16(V,Result);QA_W16(V+2,Result?0x1F:0);UINTN N=QA_Tlv(B,7,2,V,4);
  if(HaveState){QA_W32(V,State);N=QA_Tlv(B,N,0x10,V,4);}QA_Qmi(B,2,Token,0x20,N);QCHECK(QA_Packet(1,1,Node,Port,Dest,0,B,N)==CR_SUCCESS);
}
static CR_STATUS QA_Ind(UINT8 Version,UINT32 Node,UINT32 Port,UINT32 Dest,UINT32 State,CONST char *Path,UINT16 Id,UINT32 Confirm){
  UINT8 B[128],V[4];QA_W32(V,State);UINTN N=QA_Tlv(B,7,1,V,4);N=QA_Tlv(B,N,2,Path,(UINT16)strlen(Path));QA_W16(V,Id);N=QA_Tlv(B,N,3,V,2);QA_Qmi(B,4,0,0x22,N);return QA_Packet(Version,1,Node,Port,Dest,Confirm,B,N);
}
static CR_QRTR_AUDIO_PD_INFORMATION QA_State(void){CR_QRTR_AUDIO_PD_INFORMATION S;QCHECK(CrQrtrGetAudioPd(&QA_R,&S)==CR_SUCCESS);return S;}
static void QA_Fixture(BOOLEAN Enable){
  QA_Backend=NULL;QA_SentCount=0;QA_SendStatus=CR_SUCCESS;QA_Now=0;
  CrQrtrInitialize(&QA_R,QA_Send,&QA_R);CrQrtrOnline(&QA_R,TRUE);
  if(Enable)QCHECK(CrQrtrEnableAudioPd(&QA_R)==CR_SUCCESS);
  QA_Tick(0);QA_Tick(1);QCHECK(QA_SentCount==2);
}
static void QA_Listen(void){QA_Ctrl(4,2,42);QA_Tick(++QA_Now);QA_Drain();QCHECK(QA_R.Audio.Pending&&QA_R.Pending);}
static void QA_Ready(void){
  QA_Fixture(TRUE);QA_Listen();QA_Response(2,42,0x4001,QA_R.PendingTransaction,0x1FFFFFFF,0,TRUE);QA_Response(2,42,0x4002,QA_R.Audio.PendingTransaction,0x1FFFFFFF,0,TRUE);
  QCHECK(QA_R.PdReady&&QA_State().Ready);
}
static CONST UINT8 *QA_Find(CONST UINT8 *B,UINTN N,UINT8 Type,UINT16 *Size){
  QCHECK(N>=7&&QA_R16(B+5)==N-7);
  for(UINTN O=7;O<N;){QCHECK(N-O>=3);UINT16 K=QA_R16(B+O+1);QCHECK(K<=N-O-3);if(B[O]==Type){*Size=K;return B+O+3;}O+=3+K;}
  *Size=0;return NULL;
}
static void QA_RegistrationWire(void){
  QA_Fixture(FALSE);QA_Ctrl(4,2,42);QA_Tick(2);QA_Drain();QCHECK(QA_SentCount==3&&!QA_State().Enabled);
  QCHECK(CrQrtrEnableAudioPd(&QA_R)==CR_SUCCESS);QCHECK(QA_State().ServerFound&&!QA_State().Ready);QA_Tick(++QA_Now);QA_Drain();
  QCHECK(QA_SentCount==4&&QA_R32(QA_Sent[3]+12)==0x4002&&QA_R32(QA_Sent[2]+12)==0x4001);
  for(unsigned I=2;I<4;++I){CONST UINT8 *B=QA_Sent[I]+32;UINT16 N;QCHECK(QA_R32(QA_Sent[I]+24)==2&&QA_R32(QA_Sent[I]+28)==42);QCHECK(B[0]==0&&QA_R16(B+3)==0x20);CONST UINT8 *V=QA_Find(B,QA_R32(QA_Sent[I]+20),1,&N);QCHECK(V&&N==1&&*V==1);V=QA_Find(B,QA_R32(QA_Sent[I]+20),2,&N);CONST char *Path=I==2?"msm/adsp/charger_pd":"msm/adsp/audio_pd";QCHECK(V&&N==strlen(Path)&&!memcmp(V,Path,N));}
  UINT64 Next=QA_R.Audio.NextTransaction;UINT32 Gen=QA_R.Audio.Generation;QCHECK(CrQrtrEnableAudioPd(&QA_R)==CR_SUCCESS&&QA_R.Audio.NextTransaction==Next&&QA_R.Audio.Generation==Gen);
  UINT16 Token=QA_R.Audio.PendingTransaction;
  QA_Response(2,42,0x4001,Token,0x1FFFFFFF,0,TRUE);QCHECK(QA_R.PdReady&&!QA_State().Ready&&QA_R.Audio.Pending);
  QA_Response(3,42,0x4002,Token,0x1FFFFFFF,0,TRUE);QA_Response(2,43,0x4002,Token,0x1FFFFFFF,0,TRUE);QA_Response(2,42,0x4002,Token+1,0x1FFFFFFF,0,TRUE);QCHECK(QA_R.Audio.Pending&&!QA_State().Ready);
  QA_Response(2,42,0x4002,Token,0,0,FALSE);QCHECK(QA_State().Registered&&!QA_State().Known&&!QA_State().Ready);
  QCHECK(QA_Ind(3,2,42,0x4002,0x1FFFFFFF,"msm/adsp/audio_pd",0x1234,1)==CR_SUCCESS);QCHECK(QA_State().Ready);QA_Drain();
  BOOLEAN Ack=FALSE,Flow=FALSE;for(UINTN I=4;I<QA_SentCount;++I){CONST UINT8 *B=QA_Sent[I];if(QA_R32(B+4)==7){Flow=TRUE;QCHECK(QA_R32(B+12)==0x4002);continue;}if(QA_R32(B+12)!=0x4002)continue;UINT16 N;CONST UINT8 *V=QA_Find(B+32,QA_R32(B+20),1,&N);QCHECK(V&&N==17&&!memcmp(V,"msm/adsp/audio_pd",17));V=QA_Find(B+32,QA_R32(B+20),2,&N);QCHECK(V&&N==2&&QA_R16(V)==0x1234&&QA_R16(B+35)==0x23);Ack=TRUE;}QCHECK(Ack&&Flow);
  Gen=QA_State().Generation;QCHECK(QA_Ind(1,2,42,0x4002,0x1FFFFFFF,"msm/adsp/audio_pd",0x1234,0)==CR_SUCCESS);QCHECK(QA_State().Generation==Gen);QA_Drain();
}
static void QA_Isolation(void){
  QA_Ready();UINT32 AudioGen=QA_State().Generation,ChargerGen=QA_R.PdGeneration;
  QCHECK(QA_Ind(1,2,42,0x4002,0x0FFFFFFF,"msm/adsp/charger_pd",1,0)==CR_SUCCESS&&QA_State().Ready&&QA_R.PdReady);
  QCHECK(QA_Ind(1,2,42,0x4001,0x0FFFFFFF,"msm/adsp/audio_pd",1,0)==CR_SUCCESS&&QA_State().Ready&&QA_R.PdReady);
  QCHECK(QA_Ind(1,2,42,0x4002,0x0FFFFFFF,"msm/adsp/audio_pd",2,0)==CR_SUCCESS&&!QA_State().Ready&&QA_R.PdReady&&QA_R.PdGeneration==ChargerGen);QA_Drain();
  QCHECK(QA_State().Generation>AudioGen);UINT32 States[]={0x2FFFFFFF,0x7FFFFFFF,0x12345678,0x1FFFFFFF};
  for(unsigned I=0;I<4;++I){QCHECK(QA_Ind(1,2,42,0x4002,States[I],"msm/adsp/audio_pd",(UINT16)(3+I),0)==CR_SUCCESS);QCHECK(QA_State().Ready==(I==3)&&QA_State().Known==(I!=2));QA_Drain();}
  QCHECK(QA_Ind(1,2,42,0x4001,0x0FFFFFFF,"msm/adsp/charger_pd",7,0)==CR_SUCCESS&&!QA_R.PdReady&&QA_State().Ready);QA_Drain();
}
static void QA_ReorderAndTimeout(void){
  QA_Fixture(TRUE);QA_Listen();UINT16 Token=QA_R.Audio.PendingTransaction;
  QCHECK(QA_Ind(1,2,42,0x4002,0x0FFFFFFF,"msm/adsp/audio_pd",1,0)==CR_SUCCESS);QA_Response(2,42,0x4002,Token,0x1FFFFFFF,0,TRUE);QCHECK(!QA_State().Ready&&QA_State().RawState==0x0FFFFFFF);QA_Drain();
  QA_Fixture(TRUE);QA_Listen();Token=QA_R.Audio.PendingTransaction;QCHECK(QA_Ind(1,2,42,0x4002,0x1FFFFFFF,"msm/adsp/audio_pd",1,0)==CR_SUCCESS&&!QA_State().Ready);QA_Response(2,42,0x4002,Token,0,0,FALSE);QCHECK(QA_State().Ready);QA_Drain();
  QA_Fixture(TRUE);QA_Listen();Token=QA_R.Audio.PendingTransaction;QA_Response(2,42,0x4002,Token,0x1FFFFFFF,1,TRUE);QCHECK(!QA_State().Registered&&!QA_State().Ready&&QA_State().LastError==CR_DEVICE_ERROR);QA_Tick(QA_R.Audio.RetryUs);QA_Drain();QCHECK(QA_R.Audio.Pending&&QA_R.Audio.PendingTransaction!=Token);
  Token=QA_R.Audio.PendingTransaction;QA_Tick(QA_R.Audio.DeadlineUs);QCHECK(!QA_R.Audio.Pending&&QA_State().LastError==CR_TIMEOUT);QA_Response(2,42,0x4002,Token,0x1FFFFFFF,0,TRUE);QCHECK(!QA_State().Ready);QA_Tick(QA_R.Audio.RetryUs);QA_Drain();QCHECK(QA_R.Audio.Pending&&QA_R.Audio.PendingTransaction!=Token);
}
static void QA_Incarnations(void){
  QA_Ready();UINT64 Next=QA_R.Audio.NextTransaction,Server=QA_State().ServerGeneration;UINT32 Gen=QA_State().Generation;
  QCHECK(QA_Ind(1,2,42,0x4002,0x0FFFFFFF,"msm/adsp/audio_pd",8,0)==CR_SUCCESS);QCHECK(QA_Ind(1,2,42,0x4001,0x1FFFFFFF,"msm/adsp/charger_pd",9,0)==CR_SUCCESS);QCHECK(QA_R.QueueCount==2);
  QA_Ctrl(4,2,43);QCHECK(!QA_State().Ready&&QA_State().ServerPort==43&&QA_State().ServerGeneration>Server&&QA_State().Generation>Gen);QCHECK(!QA_R.QueueCount);QA_Tick(++QA_Now);QA_Drain();
  UINT16 Token=QA_R.Audio.PendingTransaction;QA_Response(2,42,0x4002,Token,0x1FFFFFFF,0,TRUE);QCHECK(QA_R.Audio.Pending);QA_Response(2,43,0x4002,Token,0x1FFFFFFF,0,TRUE);QCHECK(QA_State().Ready);
  QA_Ctrl(5,2,42);QCHECK(QA_State().Ready);QA_Ctrl(5,2,43);QCHECK(!QA_State().ServerFound&&!QA_State().Ready);
  QA_Ctrl(4,2,43);QA_Tick(++QA_Now);QA_Drain();QA_Response(2,43,0x4002,QA_R.Audio.PendingTransaction,0x1FFFFFFF,0,TRUE);QCHECK(QA_State().Ready);
  QA_Ctrl(2,2,0);QCHECK(!QA_State().Ready&&!QA_State().ServerFound);QA_Ctrl(4,2,43);QA_Tick(++QA_Now);QA_Drain();QCHECK(QA_R.Audio.Pending);
  QA_Ctrl(3,2,0);QCHECK(!QA_State().Ready&&!QA_State().ServerFound);
  Gen=QA_State().Generation;CrQrtrOnline(&QA_R,FALSE);QCHECK(!QA_State().Ready&&QA_State().Generation>Gen);CrQrtrOnline(&QA_R,TRUE);QCHECK(QA_State().Enabled&&!QA_State().Ready&&QA_R.Audio.NextTransaction>Next);QA_Ctrl(4,2,43);QA_Tick(++QA_Now);QA_Drain();QCHECK(QA_R.Audio.Pending);
}
static void QA_PressureAndExhaustion(void){
  QA_Ready();for(unsigned I=0;I<15;++I)QCHECK(QA_Ind(1,2,42,0x4001,0x1FFFFFFF,"msm/adsp/charger_pd",(UINT16)I,0)==CR_SUCCESS);QCHECK(QA_R.QueueCount==15);
  QCHECK(QA_Ind(1,2,42,0x4002,0x0FFFFFFF,"msm/adsp/audio_pd",22,0)==CR_NOT_READY);QCHECK(QA_State().Faulted&&!QA_State().Ready&&QA_R.PdReady);QA_Drain();
  UINT64 Next=QA_R.Audio.NextTransaction;QCHECK(QA_Ind(1,2,42,0x4002,0x1FFFFFFF,"msm/adsp/audio_pd",22,0)==CR_SUCCESS);QCHECK(QA_R.Audio.NextTransaction==Next+1&&QA_R.QueueCount==1&&QA_State().Faulted&&!QA_State().Ready);QA_Drain();
  QCHECK(CrQrtrEnableAudioPd(&QA_R)==CR_OUT_OF_RESOURCES);CrQrtrOnline(&QA_R,FALSE);CrQrtrOnline(&QA_R,TRUE);QCHECK(QA_State().Faulted&&!QA_State().Ready);
  QA_Ready();QA_R.Audio.NextTransaction=0xFFFF;QCHECK(QA_Ind(1,2,42,0x4002,0x0FFFFFFF,"msm/adsp/audio_pd",1,0)==CR_SUCCESS);QA_Drain();QCHECK(QA_R.Audio.NextTransaction==0x10000);QCHECK(QA_Ind(1,2,42,0x4002,0x1FFFFFFF,"msm/adsp/audio_pd",2,0)==CR_SUCCESS);QCHECK(QA_State().Faulted&&!QA_State().Ready&&QA_R.PdReady&&!QA_R.QueueCount);
  QA_Ready();QA_R.Audio.Generation=0xFFFFFFFF;QCHECK(QA_Ind(1,2,42,0x4002,0x0FFFFFFF,"msm/adsp/audio_pd",1,0)==CR_SUCCESS);QCHECK(QA_State().Faulted&&QA_State().Generation==0xFFFFFFFF&&!QA_State().Ready);
  QA_Ready();QA_R.Audio.ServerGeneration=~(UINT64)0;QA_Ctrl(5,2,42);QCHECK(QA_State().Faulted&&QA_State().ServerGeneration==~(UINT64)0);
  QA_Fixture(TRUE);QA_Ctrl(4,2,42);QA_Tick(~(UINT64)0-1);QCHECK(QA_State().Faulted&&QA_State().LastError==CR_OUT_OF_RESOURCES&&!QA_R.Audio.Pending);
  QA_Ready();QA_Tick(QA_Now-1);QCHECK(QA_State().Faulted&&QA_State().LastError==CR_INVALID_PARAMETER&&QA_R.PdReady);
  QA_Fixture(TRUE);QA_Ctrl(4,2,42);QA_Tick(2);QCHECK(QA_R.QueueCount==2);UINT16 Old=QA_R.Audio.PendingTransaction;QA_Tick(QA_R.Audio.DeadlineUs);QCHECK(!QA_R.QueueCount&&!QA_R.Audio.Pending);QA_Response(2,42,0x4002,Old,0x1FFFFFFF,0,TRUE);QCHECK(!QA_State().Ready);
}
static void QA_Malformed(void){
  for(unsigned Kind=0;Kind<5;++Kind){QA_Ready();UINT8 B[96],V[4];QA_W32(V,0x0FFFFFFF);UINTN N=QA_Tlv(B,7,1,V,Kind==0?3:4);N=QA_Tlv(B,N,2,"msm/adsp/audio_pd",17);QA_W16(V,1);N=QA_Tlv(B,N,3,V,Kind==1?1:2);if(Kind==2)N=QA_Tlv(B,N,3,V,2);QA_Qmi(B,4,0,0x22,N);if(Kind==3)B[5]++;if(Kind==4){QA_W16(B+8,0xFFFF);}QCHECK(QA_Packet(1,1,2,42,0x4002,1,B,N)==CR_SUCCESS);QCHECK(QA_State().Faulted&&!QA_State().Ready&&QA_R.PdReady);QA_Drain();}
  QA_Ready();UINT8 B[32]={2};QA_W16(B+1,QA_R.Audio.PendingTransaction);QA_W16(B+3,0x20);QA_W16(B+5,25);QCHECK(QA_Packet(1,1,3,42,0x4002,0,B,32)==CR_SUCCESS&&QA_State().Ready);
  QCHECK(CrQrtrEnableAudioPd(NULL)==CR_INVALID_PARAMETER);QCHECK(CrQrtrGetAudioPd(NULL,(CR_QRTR_AUDIO_PD_INFORMATION *)B)==CR_INVALID_PARAMETER);QCHECK(CrQrtrGetAudioPd(&QA_R,NULL)==CR_INVALID_PARAMETER);QCHECK(CrQrtrGetAudioPd(&QA_R,(CR_QRTR_AUDIO_PD_INFORMATION *)&QA_R.Audio)==CR_INVALID_PARAMETER);
}
int QA_RunAll(void){QA_RegistrationWire();QA_Isolation();QA_ReorderAndTimeout();QA_Incarnations();QA_PressureAndExhaustion();QA_Malformed();printf("QRTR audio+charger independent wire/queue checks: %u PASS\n",QA_Checks);return 0;}
#ifndef QRTR_AUDIO_COMPOSED
int main(void){return QA_RunAll();}
#endif
