/* Actual raw transport: transaction identity, cancellation and wire shape. */
#include <ucsi/Ppm.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) {return memcpy(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V) {return memset(D,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) {return memcmp(A,B,N);}
#endif
static UINT8 Packet[64];static UINTN PacketSize;static CR_STATUS SendStatus;
static CR_STATUS Send(VOID *C,CONST VOID *D,UINTN N) {(void)C;assert(N<=64);memcpy(Packet,D,N);PacketSize=N;return SendStatus;}
static VOID Put(UINT8 *P,UINT32 V) {memcpy(P,&V,4);}
static VOID Reply(CR_UCSI_PPM *P,UINT16 Version,UINT32 Cci,UINT32 Status) {
  UINT8 B[64]={0};Put(B,32779);Put(B+4,1);Put(B+8,0x11);memcpy(B+12,&Version,2);Put(B+16,Cci);Put(B+60,Status);CrUcsiPpmReceive(P,B,64);
}
int main(void) {
  CR_UCSI_PPM P;UINT8 B[48]={0},Out[48]={0},R[24]={0};UINT32 Cci;
  CrUcsiPpmInitialize(&P,Send,NULL);assert(CrUcsiPpmRead(&P,0)==CR_NOT_READY);
  CrUcsiPpmPeer(&P,TRUE,1);assert(CrUcsiPpmWrite(&P,B,0)==CR_UNSUPPORTED);
  assert(CrUcsiPpmRead(&P,0)==CR_SUCCESS && PacketSize==12);assert(CrUcsiPpmRead(&P,1)==CR_BUSY);
  Reply(&P,0x120,0x80001000,0);assert(CrUcsiPpmTake(&P,Out)==CR_SUCCESS && P.Version==0x120);
  memset(B,0x5a,48);assert(CrUcsiPpmWrite(&P,B,100)==CR_SUCCESS && PacketSize==64);
  for(unsigned I=12;I<20;++I)assert(Packet[I]==0);
  for(unsigned I=20;I<28;++I)assert(Packet[I]==0x5a);
  for(unsigned I=28;I<44;++I)assert(Packet[I]==0);
  for(unsigned I=44;I<60;++I)assert(Packet[I]==0x5a);
  for(unsigned I=60;I<64;++I)assert(Packet[I]==0);
  CrUcsiPpmCancel(&P);assert(CrUcsiPpmRead(&P,101)==CR_BUSY);
  Put(R,32779);Put(R+4,2);Put(R+8,0x12);CrUcsiPpmReceive(&P,R,16);
  assert(CrUcsiPpmTake(&P,NULL)==CR_ABORT && !P.Faulted);
  assert(CrUcsiPpmRead(&P,200)==CR_SUCCESS);CrUcsiPpmTick(&P,5000200);
  assert(CrUcsiPpmTake(&P,NULL)==CR_TIMEOUT && P.Faulted);
  Reply(&P,0x120,0,0);assert(CrUcsiPpmRead(&P,6000000)==CR_NOT_READY);
  CrUcsiPpmPeer(&P,TRUE,1);assert(P.Faulted);
  CrUcsiPpmPeer(&P,FALSE,1);CrUcsiPpmPeer(&P,TRUE,2);assert(CrUcsiPpmRead(&P,6000000)==CR_SUCCESS);
  Reply(&P,0x200,0,0);assert(CrUcsiPpmTake(&P,NULL)==CR_UNSUPPORTED && P.Version==0);
  assert(CrUcsiPpmRead(&P,6000010)==CR_SUCCESS);Reply(&P,0x120,0x1100,0);
  assert(CrUcsiPpmTake(&P,NULL)==CR_COMPROMISED_DATA);
  assert(CrUcsiPpmRead(&P,6000020)==CR_SUCCESS);Reply(&P,0x120,0,7);
  assert(CrUcsiPpmTake(&P,NULL)==CR_DEVICE_ERROR);
  Put(R+8,0x13);Put(R+12,2);Put(R+16,1);CrUcsiPpmReceive(&P,R,24);assert(!CrUcsiPpmNotification(&P,&Cci));
  Put(R+16,0);for(unsigned I=0;I<16;++I){Put(R+12,I);CrUcsiPpmReceive(&P,R,24);}
  for(unsigned I=0;I<16;++I){assert(CrUcsiPpmNotification(&P,&Cci)&&Cci==I);}
  assert(!CrUcsiPpmNotification(&P,&Cci));
  assert(CrUcsiPpmRead(&P,7000000)==CR_SUCCESS);CrUcsiPpmPeer(&P,FALSE,2);assert(CrUcsiPpmTake(&P,NULL)==CR_NOT_READY);
  CrUcsiPpmPeer(&P,TRUE,3);SendStatus=CR_DEVICE_ERROR;
  assert(CrUcsiPpmRead(&P,8000000)==CR_DEVICE_ERROR && P.Faulted);
  puts("UCSI raw PPM cancellation, wire ownership and generation tests PASS");return 0;
}
