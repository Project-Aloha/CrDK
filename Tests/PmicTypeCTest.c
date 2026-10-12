/* Independent raw Qualcomm frames; portable policy, not a remote simulator. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/PmicTypeCCrLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *B,UINTN N,UINT8 V){return memset(B,V,N);}
#endif
STATIC CR_PMIC_TYPEC P;
STATIC UINT8 Sent[24];STATIC UINT32 Sends;STATIC CR_STATUS SendStatus;
STATIC CR_STATUS Send(VOID *C,CONST UINT8 *D,UINTN N){assert(C==&P&&N==24);memcpy(Sent,D,N);++Sends;return SendStatus;}
STATIC VOID Put(UINT8 *D,UINT32 V){for(UINT32 I=0;I<4;++I)D[I]=(UINT8)(V>>(I*8));}
STATIC VOID Header(UINT8 *D,UINT32 Owner,UINT32 Op){Put(D,Owner);Put(D+4,1);Put(D+8,Op);}
STATIC VOID Fixture(VOID){Sends=0;SendStatus=CR_SUCCESS;CrPmicTypeCInit(&P,Send,&P);CrPmicTypeCPeer(&P,TRUE,3,7);}
STATIC VOID Ack(VOID){UINT8 D[16]={0};Header(D,32780,0x15);CrPmicTypeCReceive(&P,D,sizeof(D),0);}
STATIC VOID Pan(UINT8 Orientation,UINT8 Mux,UINT8 Pin,UINT8 Hpd,UINT8 Irq){UINT8 D[32]={0};Header(D,32780,(0xff01U<<16)|0x16);D[13]=Orientation;D[14]=Mux;D[20]=Pin|(Hpd?64:0)|(Irq?128:0);CrPmicTypeCReceive(&P,D,sizeof(D),0);}
STATIC VOID Read(VOID){UINT8 D[12]={0};Header(D,32779,0x11);CrPmicTypeCObserveSend(&P,D,12,CR_SUCCESS,0);}
STATIC VOID Status(UINT32 Word){UINT8 W[64]={0},A[16]={0},R[64]={0};Header(W,32779,0x12);Put(W+20,0x10012);CrPmicTypeCObserveSend(&P,W,64,CR_SUCCESS,0);
 Header(A,32779,0x12);CrPmicTypeCReceive(&P,A,16,0);Read();Header(R,32779,0x11);R[12]=0x20;R[13]=1;Put(R+16,0x80000900);Put(R+28,Word);CrPmicTypeCReceive(&P,R,64,0);}
STATIC CR_USB_TYPEC_STATE Get(UINT64 *G){CR_USB_TYPEC_STATE S;assert(CrPmicTypeCGet(&P,&S,G)==CR_SUCCESS);return S;}
int main(VOID){
 Fixture();CR_USB_TYPEC_STATE S,Old;UINT64 G=0,Next;memset(&S,0xa7,sizeof(S));Old=S;
 assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY&&memcmp(&S,&Old,sizeof(S))==0&&!G);
 assert(CrPmicTypeCTick(&P,0)==CR_SUCCESS&&Sends==1&&!P.PanEnabled&&P.PanPending);
 assert(Sent[0]==0x0c&&Sent[1]==0x80&&Sent[4]==1&&Sent[8]==0x15&&Sent[12]==0x10);
 for(UINT32 I=13;I<24;++I)assert(!Sent[I]);Ack();assert(P.PanEnabled&&!P.PanPending);
 Pan(0,3,4,1,0);Status((1U<<19)|(1U<<21)|(2U<<29));S=Get(&G);
 assert(S.PdReady&&S.Connected&&S.DataRole==UsbTypeCHost&&S.Orientation==UsbTypeCNormal&&S.DpActive&&S.DpLanes==2&&S.Usb3&&S.Hpd&&S.PinAssignment==4);
 assert(CrPmicTypeCTick(&P,1)==CR_SUCCESS&&Sent[12]==0x11&&P.PanPending);Ack();UINT32 N=Sends;
 assert(CrPmicTypeCTick(&P,2)==CR_SUCCESS&&Sends==N);Ack();assert(!P.Acks&&!P.PanPending);
 Pan(0,3,4,0,1);S=Get(&Next);assert(Next==G&&!S.Hpd&&S.HpdIrqSequence==1);
 // Replayed route packet carries no transaction sequence: it leaves route
 // generation unchanged, while a real HPD IRQ remains an event observation.
 Pan(0,3,4,0,0);S=Get(&Next);assert(Next==G&&S.HpdIrqSequence==1);
 Pan(1,3,4,1,0);assert(CrPmicTypeCGet(&P,&S,&Next)==CR_NOT_READY);Status((1U<<19)|(1U<<21)|(2U<<29));S=Get(&Next);assert(Next>G&&S.Orientation==UsbTypeCReverse);G=Next;
 Status(0);S=Get(&Next);assert(!S.Connected&&!S.DpActive&&!S.Usb3&&S.Orientation==UsbTypeCUnoriented&&Next>G);
 UINT32 Cases=0;
 for(UINT32 O=0;O<3;++O)for(UINT32 M=0;M<5;++M)for(UINT32 Pin=0;Pin<8;++Pin){
  Fixture();CrPmicTypeCTick(&P,0);Ack();Pan((UINT8)O,(UINT8)M,(UINT8)Pin,1,0);Status((1U<<19)|(2U<<29));S=Get(&G);
  BOOLEAN Dp=O<2&&((M==2&&(Pin==3||Pin==5))||(M==3&&(Pin==4||Pin==6)));
  assert(S.DpActive==Dp&&S.Usb3==(M==1||(Dp&&M==3)));if(Dp)assert(S.DpLanes==(M==2?4:2));++Cases;
 }
 // Malformed owners/types/size/port/reserved CCI/version cannot establish a
 // status. These reads were never preceded by the matching accepted command.
 for(UINT32 I=0;I<64;++I){Fixture();UINT8 D[64]={0};Header(D,32779,0x11);D[12]=0x20;D[13]=1;Put(D+16,0x80000900);Put(D+28,(1U<<19)|(2U<<29));
  CrPmicTypeCReceive(&P,D,I,0);assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);++Cases;}
 Fixture();Status((1U<<19)|(1U<<29));S=Get(&G);assert(S.DataRole==UsbTypeCDevice&&!S.Usb3);
 UINT8 W[64]={0},A[16]={0},R[64]={0};Header(W,32779,0x12);Put(W+20,0x10012);Header(A,32779,0x12);Header(R,32779,0x11);R[12]=0x20;R[13]=1;Put(R+16,0x80000900);Put(R+28,(1U<<19)|(2U<<29));
 CrPmicTypeCInvalidateUsbc(&P);CrPmicTypeCObserveSend(&P,W,64,CR_SUCCESS,0);
 CrPmicTypeCReceive(&P,R,64,0);assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);CrPmicTypeCReceive(&P,A,16,0);
 for(UINT32 Bit=25;Bit<=30;++Bit){Put(R+16,0x80000900|(1U<<Bit));Read();CrPmicTypeCReceive(&P,R,64,0);assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);++Cases;}
 Put(R+16,0x80000900);Read();CrPmicTypeCReceive(&P,R,64,0);Get(&G);
 Header(W,32779,0x12);Put(W+20,0x10004);CrPmicTypeCObserveSend(&P,W,64,CR_SUCCESS,0);CrPmicTypeCReceive(&P,A,16,0);CrPmicTypeCInvalidateUsbc(&P);CrPmicTypeCReceive(&P,R,64,0);assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);
 // Every status snapshot requires a fresh, accepted OS READ. The reply has
 // no transaction ID: a completed command is consumed once, never replayed.
 Fixture();Header(W,32779,0x12);Put(W+20,0x10012);CrPmicTypeCObserveSend(&P,W,64,CR_SUCCESS,0);CrPmicTypeCReceive(&P,A,16,0);
 CrPmicTypeCReceive(&P,R,64,0);assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);
 Read();CrPmicTypeCReceive(&P,R,64,0);S=Get(&G);Next=P.Generation;
 Put(R+28,0);CrPmicTypeCReceive(&P,R,64,0);S=Get(&G);assert(S.Connected&&G==Next);
 Read();CrPmicTypeCReceive(&P,R,64,0);S=Get(&G);assert(S.Connected&&G==Next);++Cases;
 // Malformed known replies cannot promote a route even after correct write
 // completion. Re-prime independently to avoid borrowing a prior valid state.
 for(UINT32 V=0;V<14;++V){Fixture();memset(W,0,64);memset(A,0,16);memset(R,0,64);
  Header(W,32779,0x12);Put(W+20,0x10012);Header(A,32779,0x12);Header(R,32779,0x11);R[12]=0x20;R[13]=1;Put(R+16,0x80000900);Put(R+28,(1U<<19)|(2U<<29));
  if(V==0)W[27]=1; // full-width Control, not a low-bit connector mask
  if(V==1)Put(W+20,0x20012); // unsupported connector2
  CrPmicTypeCObserveSend(&P,W,64,CR_SUCCESS,0);
  if(V==2)Put(A+12,1);CrPmicTypeCReceive(&P,A,16,0);Read();
  if(V==3)R[12]=0xff,R[13]=0;
  if(V==4)R[12]=0,R[13]=2;
  if(V==5)Put(R+60,1);
  if(V==6)Put(R+16,0x80000800);
  if(V==7)Put(R+16,0x80001100);
  if(V==8)Put(R+16,0x00000900);
  if(V==9)Put(R,32778);
  if(V==10)Put(R+4,3);
  if(V==11)Put(R+8,0x12);
  CrPmicTypeCReceive(&P,R,V==12?63:V==13?16:64,0);
  assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);++Cases;
 }
 // Invalid PAN metadata is never acknowledged or promoted. Wrong port and
 // orientation/length/type leave the exact previous route untouched.
 for(UINT32 V=0;V<5;++V){Fixture();CrPmicTypeCTick(&P,0);Ack();Status((1U<<19)|(2U<<29));S=Get(&G);Old=S;Next=P.Generation;
  UINT8 D[32]={0};Header(D,32780,(0xff01U<<16)|0x16);D[14]=3;D[20]=4|64;
  if(V==0)D[12]=1;if(V==1)D[13]=3;if(V==2)Put(D+4,3);if(V==3)Put(D,32778);
  CrPmicTypeCReceive(&P,D,V==4?31:32,0);S=Get(&G);
  assert(!P.Acks&&!P.AltValid&&G==Next&&!memcmp(&S,&Old,sizeof(S)));++Cases;
 }
 for(UINT32 V=0;V<2;++V){Fixture();SendStatus=V?CR_DEVICE_ERROR:(CR_STATUS)1;
  assert(CrPmicTypeCTick(&P,0)==(V?CR_DEVICE_ERROR:CR_NOT_READY)&&P.PanFaulted&&!P.PanPending);
  UINT32 Before=Sends;Ack();assert(!P.PanEnabled&&CrPmicTypeCTick(&P,1)==CR_NOT_READY&&Sends==Before);++Cases;
 }
 Fixture();Status((1U<<19)|(2U<<29));S=Get(&G);CrPmicTypeCPeer(&P,FALSE,3,7);assert(CrPmicTypeCGet(&P,&S,&Next)==CR_NOT_READY);
 CrPmicTypeCPeer(&P,TRUE,4,7);assert(CrPmicTypeCGet(&P,&S,&Next)==CR_NOT_READY);CrPmicTypeCReceive(&P,R,64,0);assert(CrPmicTypeCGet(&P,&S,&Next)==CR_NOT_READY);++Cases;
 // Receive expires before interpreting a late response, even if no timer
 // ran in between. No public cached snapshot may outlive an unanswered OS
 // read/write operation in this same authenticated firmware generation.
 for(UINT32 V=0;V<3;++V){Fixture();memset(W,0,64);memset(A,0,16);memset(R,0,64);Header(W,32779,0x12);Put(W+20,0x10012);Header(A,32779,0x12);Header(R,32779,0x11);R[12]=0x20;R[13]=1;Put(R+16,0x80000900);Put(R+28,(1U<<19)|(2U<<29));
  CrPmicTypeCObserveSend(&P,W,64,CR_SUCCESS,0);
  if(V==0)CrPmicTypeCReceive(&P,A,16,5000000);
  else {CrPmicTypeCReceive(&P,A,16,0);Read();CrPmicTypeCReceive(&P,R,64,V==1?5000000:4999999);}
  if(V==2){S=Get(&G);assert(S.Connected&&!P.UsbcFaulted);}
  else {assert(P.UsbcFaulted&&CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);Status((1U<<19)|(2U<<29));assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);CrPmicTypeCPeer(&P,TRUE,4,8);Status((1U<<19)|(2U<<29));Get(&G);}
  ++Cases;
 }
 Fixture();Status((1U<<19)|(2U<<29));S=Get(&G);Header(W,32779,0x12);Put(W+20,0x10012);CrPmicTypeCObserveSend(&P,W,64,CR_SUCCESS,100);
 CrPmicTypeCExpire(&P,5000100);assert(P.UsbcFaulted&&CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);++Cases;
 Fixture();Read();Read();assert(P.UsbcFaulted&&CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);++Cases;
 Fixture();Read();CrPmicTypeCPeer(&P,FALSE,3,0);assert(P.UsbcFaulted&&P.AdspGeneration==7);CrPmicTypeCPeer(&P,TRUE,4,7);Status((1U<<19)|(2U<<29));assert(P.UsbcFaulted&&CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY);
 CrPmicTypeCPeer(&P,TRUE,5,8);Status((1U<<19)|(2U<<29));Get(&G);++Cases;
 Fixture();CrPmicTypeCTick(&P,0);CrPmicTypeCPeer(&P,FALSE,3,0);assert(P.PanUncertain&&P.PanFaulted&&P.AdspGeneration==7);CrPmicTypeCPeer(&P,TRUE,4,7);assert(P.PanUncertain&&CrPmicTypeCTick(&P,1)==CR_NOT_READY);++Cases;
 Fixture();SendStatus=CR_NOT_READY;assert(CrPmicTypeCTick(&P,0)==CR_NOT_READY&&!P.PanPending&&!P.PanFaulted);assert(CrPmicTypeCTick(&P,1)==CR_SUCCESS&&Sends==1);SendStatus=CR_SUCCESS;assert(CrPmicTypeCTick(&P,20000)==CR_SUCCESS&&Sends==2);
 assert(CrPmicTypeCTick(&P,5020000)==CR_TIMEOUT&&P.PanFaulted&&!P.PanPending);Ack();assert(!P.PanEnabled);CrPmicTypeCPeer(&P,FALSE,3,7);CrPmicTypeCPeer(&P,TRUE,4,7);assert(CrPmicTypeCTick(&P,6000000)==CR_NOT_READY&&Sends==2);
 CrPmicTypeCPeer(&P,TRUE,5,8);assert(CrPmicTypeCTick(&P,6000001)==CR_SUCCESS&&Sends==3);Ack();assert(P.PanEnabled);
 CrPmicTypeCClose(&P);N=Sends;assert(CrPmicTypeCTick(&P,6000002)==CR_NOT_READY);CrPmicTypeCPeer(&P,TRUE,6,9);Status((1U<<19)|(2U<<29));assert(CrPmicTypeCGet(&P,&S,&G)==CR_NOT_READY&&Sends==N);
 printf("PMIC Type-C observer PASS: %u raw-frame vectors; sole PAN, OS UCSI correlation, HPD route generation, timeout poison and irreversible close\n",Cases);return 0;
}
