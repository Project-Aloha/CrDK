/* Actual BOT core against a deterministic asynchronous host. SPDX-License-Identifier: MIT */
#include <Library/UsbMscCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CR_USB_MSC C;
static UINT8 *Disk;
static unsigned Cases,Transactions;
static UINT32 L32(const UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static UINT32 B32(const UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
static void LE(UINT8 *P,UINT32 V){for(unsigned I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
static void BE(UINT8 *P,UINT32 V){for(unsigned I=0;I<4;++I)P[I]=(UINT8)(V>>(8*(3-I)));}
static CR_USB_MSC_ACTION Next(void){CR_USB_MSC_ACTION A;assert(CrUsbMscNext(&C,&A)==CR_SUCCESS);assert(A.Length<=65536);return A;}
static void Done(CR_USB_MSC_ACTION A){assert(CrUsbMscComplete(&C,TRUE,A.Length)==CR_SUCCESS);}
static void CBW(UINT8 Op,UINT8 Len,UINT32 Host,BOOLEAN In,UINT32 Count,UINT32 Lba){
  CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkOut&&A.Length==31);UINT8 *B=A.Buffer;memset(B,0,31);
  LE(B,0x43425355);LE(B+4,0x93510000U+Transactions++);LE(B+8,Host);B[12]=In?0x80:0;B[14]=Len;B[15]=Op;
  if(Op==0x28||Op==0x2a||Op==0x2f){BE(B+17,Lba);B[22]=(UINT8)(Count>>8);B[23]=(UINT8)Count;}
  if(Op==0xa8||Op==0xaa){BE(B+17,Lba);BE(B+21,Count);}
  if(Op==0x88||Op==0x8a){BE(B+21,Lba);BE(B+25,Count);}
  if(Op==0x03||Op==0x12)B[19]=(UINT8)Count;
  Done(A);
}
static void Raw(CONST UINT8 *Cdb,UINT8 Length,UINT32 Host,BOOLEAN In){
  CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkOut&&A.Length==31);UINT8 *B=A.Buffer;memset(B,0,31);
  LE(B,0x43425355);LE(B+4,0x93510000U+Transactions++);LE(B+8,Host);B[12]=In?0x80:0;B[14]=Length;memcpy(B+15,Cdb,Length);Done(A);
}
static void Reset(void){assert(CrUsbMscBusReset(&C)==CR_SUCCESS);assert(CrUsbMscSetMaxPacket(&C,512)==CR_SUCCESS);}
static void Finish(UINT8 Status,UINT32 Residue,UINT32 Bytes,UINT32 Stalls,UINT32 Zlps){
  UINT32 Got=0,Halt=0,Zlp=0;unsigned Budget=1000;
  while(--Budget){
    CR_USB_MSC_ACTION A=Next();assert(A.Type!=CrUsbMscNone);
    if(A.Type==CrUsbMscStallIn||A.Type==CrUsbMscStallOut){++Halt;Done(A);assert(CrUsbMscClearHalt(&C,A.Type==CrUsbMscStallIn));continue;}
    if(A.Buffer==C.Csw){assert(A.Type==CrUsbMscBulkIn&&A.Length==13&&L32(A.Buffer)==0x53425355);
      assert(L32((UINT8*)A.Buffer+4)==0x93510000U+Transactions-1);assert(C.Csw[12]==Status);assert(L32(C.Csw+8)==Residue);Done(A);break;}
    Got+=A.Length;if(!A.Length)++Zlp;
    if(A.Type==CrUsbMscBulkOut)memset(A.Buffer,0x96,A.Length);Done(A);
  }
  assert(Budget&&Got==Bytes&&Halt==Stalls&&Zlp==Zlps);++Cases;
}
static void Matrix(void){
  /* Device-intent N, I(512), O(512). All 13 normative cases are exercised
     through CBW bytes, not by directly seeding private core state. */
  struct {UINT8 Op;UINT32 Host;BOOLEAN In;UINT8 Status;UINT32 Residue,Bytes,Stalls,Zlps;} V[]={
    {0,0,FALSE,0,0,0,0,0},{0x28,0,FALSE,2,0,0,0,0},{0x2a,0,TRUE,2,0,0,0,0},
    {0,1024,TRUE,0,1024,0,1,0},{0x28,1024,TRUE,0,512,512,1,1},{0x28,512,TRUE,0,0,512,0,0},
    {0x28,256,TRUE,2,0,256,0,0},{0x2a,512,TRUE,2,512,0,1,0},
    {0,1024,FALSE,0,1024,1024,0,0},{0x28,512,FALSE,2,512,512,0,0},
    {0x2a,1024,FALSE,0,512,1024,0,0},{0x2a,512,FALSE,0,0,512,0,0},{0x2a,256,FALSE,2,256,256,0,0}
  };
  for(unsigned I=0;I<sizeof(V)/sizeof(V[0]);++I){Reset();CBW(V[I].Op,V[I].Op?10:6,V[I].Host,V[I].In,1,10000);Finish(V[I].Status,V[I].Residue,V[I].Bytes,V[I].Stalls,V[I].Zlps);}
  for(UINT16 P=64;P<=1024;P=(P==64?512:1024)){Reset();assert(CrUsbMscSetMaxPacket(&C,P)==CR_SUCCESS);
    CBW(0x28,10,2048,TRUE,1,0);Finish(0,1536,512,1,P==1024?0:1);if(P==1024)break;}
}
static void RecoverInvalid(void){
  CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscStallIn);Done(A);assert(!CrUsbMscClearHalt(&C,TRUE));
  A=Next();assert(A.Type==CrUsbMscStallOut);Done(A);assert(!CrUsbMscClearHalt(&C,FALSE));
  A=Next();assert(A.Type==CrUsbMscNone);assert(CrUsbMscReset(&C)==CR_SUCCESS);
  A=Next();assert(A.Type==CrUsbMscNone);assert(CrUsbMscClearHalt(&C,TRUE));A=Next();assert(A.Type==CrUsbMscNone);
  assert(CrUsbMscClearHalt(&C,FALSE));CBW(0,6,0,FALSE,0,0);Finish(0,0,0,0,0);
}
static void Invalid(void){
  for(UINT32 N=0;N<31;++N){Reset();CR_USB_MSC_ACTION A=Next();memset(A.Buffer,0,31);LE(A.Buffer,0x43425355);((UINT8*)A.Buffer)[14]=6;
    assert(CrUsbMscComplete(&C,TRUE,N)==CR_SUCCESS);RecoverInvalid();}
  for(UINT32 Field=0;Field<5;++Field){Reset();CR_USB_MSC_ACTION A=Next();memset(A.Buffer,0,31);LE(A.Buffer,0x43425355);((UINT8*)A.Buffer)[14]=6;
    UINT8 *B=A.Buffer;if(Field==0)B[0]^=1;if(Field==1)B[12]=1;if(Field==2)B[13]=0x10;if(Field==3)B[14]=0;if(Field==4)B[14]=17;Done(A);RecoverInvalid();}
  Reset();CR_USB_MSC_ACTION A=Next();assert(CrUsbMscNext(&C,&A)==CR_BUSY);assert(CrUsbMscComplete(&C,TRUE,32)==CR_BAD_BUFFER_SIZE);RecoverInvalid();
  Reset();A=Next();assert(CrUsbMscComplete(&C,FALSE,0)==CR_DEVICE_ERROR);RecoverInvalid();
  Reset();CBW(0x28,10,512,TRUE,1,0);A=Next();assert(CrUsbMscComplete(&C,TRUE,511)==CR_BAD_BUFFER_SIZE);RecoverInvalid();
  Reset();CBW(0x2a,10,512,FALSE,1,0);A=Next();assert(CrUsbMscComplete(&C,TRUE,511)==CR_BAD_BUFFER_SIZE);RecoverInvalid();
  Reset();CBW(0,6,0,FALSE,0,0);A=Next();assert(CrUsbMscComplete(&C,TRUE,12)==CR_BAD_BUFFER_SIZE);RecoverInvalid();
}
static void ReadReply(UINT8 *Out,UINT32 Length){CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkIn&&A.Length==Length);memcpy(Out,A.Buffer,Length);Done(A);Finish(0,0,0,0,0);}
static void ScsiCommands(void){
  UINT8 R[96];Reset();CBW(0x12,6,36,TRUE,36,0);ReadReply(R,36);assert(!memcmp(R+8,"CRANE   RAM DISK TEST   0001",28));
  CBW(0x25,10,8,TRUE,0,0);ReadReply(R,8);assert(B32(R)==32767&&B32(R+4)==512);
  CBW(0xff,6,0,FALSE,0,0);Finish(1,0,0,0,0);
  CBW(3,6,18,TRUE,18,0);ReadReply(R,18);assert(R[2]==5&&R[12]==0x20);
  CBW(3,6,18,TRUE,18,0);ReadReply(R,18);assert(!R[2]&&!R[12]);
  CBW(0x28,10,1024,TRUE,2,32767);Finish(1,1024,0,1,0);
  CBW(3,6,18,TRUE,18,0);ReadReply(R,18);assert(R[2]==5&&R[12]==0x21);
  CBW(0x28,10,512,TRUE,1,32767);Finish(0,0,512,0,0);
  CBW(0x28,10,0,FALSE,0,0xffffffff);Finish(0,0,0,0,0);
  CBW(0x12,6,0xffffffff,TRUE,36,0);Finish(0,0xffffffff-36,36,1,0);
  CBW(0xaa,12,512,FALSE,0xffffffff,0);Finish(1,512,512,0,0);
  CBW(0x88,16,512,TRUE,1,0xffffffff);Finish(1,512,0,1,0);
  CBW(0x35,10,0,FALSE,0,0);Finish(0,0,0,0,0);
  CBW(0x2f,10,0,FALSE,2,32767);Finish(1,0,0,0,0);
  CBW(0x2f,10,0,FALSE,1,0);Finish(0,0,0,0,0);
  CBW(0x2f,10,0,FALSE,0,0xffffffff);Finish(0,0,0,0,0);
  CBW(0x2a,10,0,FALSE,0,0xffffffff);Finish(0,0,0,0,0);
}
static void AdditionalScsi(void){
  UINT8 R[96],B[16]={0};Reset();
  B[0]=0x12;B[1]=1;B[4]=6;Raw(B,6,6,TRUE);ReadReply(R,6);assert(R[3]==2&&R[4]==0&&R[5]==0x80);
  B[2]=0x80;B[4]=20;Raw(B,6,20,TRUE);ReadReply(R,20);assert(R[3]==16&&!memcmp(R+4,"0000000000000001",16));
  memset(B,0,16);B[0]=0x1a;B[1]=8;B[2]=0x3f;B[4]=64;Raw(B,6,24,TRUE);ReadReply(R,24);assert(R[0]==23&&R[4]==8&&R[5]==18&&!R[6]);
  memset(B,0,16);B[0]=0x5a;B[1]=8;B[2]=8;B[8]=28;Raw(B,10,28,TRUE);ReadReply(R,28);assert(R[1]==26&&R[8]==8&&R[9]==18);
  memset(B,0,16);B[0]=0x23;B[8]=12;Raw(B,10,12,TRUE);ReadReply(R,12);assert(R[3]==8&&B32(R+4)==32768&&R[8]==2&&R[10]==2);
  memset(B,0,16);B[0]=0x9e;B[1]=0x10;B[13]=32;Raw(B,16,32,TRUE);ReadReply(R,32);assert(!B32(R)&&B32(R+4)==32767&&B32(R+8)==512);
  memset(B,0,16);B[0]=0xa0;B[9]=16;Raw(B,12,16,TRUE);ReadReply(R,16);assert(B32(R)==8&&!B32(R+8));
  memset(B,0,16);B[0]=0x1e;B[4]=1;Raw(B,6,0,FALSE);Finish(0,0,0,0,0);
  B[0]=0x1b;B[4]=2;Raw(B,6,0,FALSE);Finish(1,0,0,0,0);
  CBW(3,6,18,TRUE,18,0);ReadReply(R,18);assert(R[2]==5&&R[12]==0x53&&R[13]==2);
  B[0]=0x1e;B[4]=0;Raw(B,6,0,FALSE);Finish(0,0,0,0,0);B[0]=0x1b;B[4]=2;Raw(B,6,0,FALSE);Finish(0,0,0,0,0);
  CBW(0,6,0,FALSE,0,0);Finish(1,0,0,0,0);CBW(3,6,18,TRUE,18,0);ReadReply(R,18);assert(R[2]==2&&R[12]==0x3a);
  B[4]=3;Raw(B,6,0,FALSE);Finish(0,0,0,0,0);CBW(0,6,0,FALSE,0,0);Finish(0,0,0,0,0);
}
static void Streaming(void){
  Reset();CBW(0xaa,12,300*512,FALSE,300,10000);UINT32 Offset=0,Parts=0;
  while(Offset<300*512){CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkOut&&A.Length&&A.Length<=65536);
    for(UINT32 I=0;I<A.Length;++I)((UINT8*)A.Buffer)[I]=(UINT8)((Offset+I)*37+9);Offset+=A.Length;++Parts;Done(A);}
  assert(Parts==3);Finish(0,0,0,0,0);CBW(0xa8,12,300*512,TRUE,300,10000);Offset=0;
  while(Offset<300*512){CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkIn);
    for(UINT32 I=0;I<A.Length;++I)assert(((UINT8*)A.Buffer)[I]==(UINT8)((Offset+I)*37+9));Offset+=A.Length;Done(A);}
  Finish(0,0,0,0,0);assert(CrUsbMscReset(&C)==CR_SUCCESS);assert(CrUsbMscClearHalt(&C,FALSE));assert(CrUsbMscClearHalt(&C,TRUE));
  assert(Disk[10000*512]==9);Reset();assert(Disk[10000*512+1]==46);
  CBW(0x2a,10,256,FALSE,1,10000);Finish(2,256,256,0,0);assert(Disk[10000*512]==9);Reset();
}
static void CdbCorpus(void){
  /* All command opcodes and legal CDB lengths with bounded host expectations.
     Adversarial integer LBA/count bytes cannot create out-of-range actions. */
  for(UINT32 Op=0;Op<256;++Op)for(UINT32 Length=1;Length<=16;++Length){
    Reset();CR_USB_MSC_ACTION A=Next();UINT8 *B=A.Buffer;memset(B,0,31);LE(B,0x43425355);LE(B+8,65537);B[12]=(Op&1)?0x80:0;B[14]=(UINT8)Length;B[15]=(UINT8)Op;
    for(UINT32 I=1;I<16;++I)B[15+I]=(UINT8)(Op*19+I*31);if(Length>1)B[14+Length]=0;Done(A);
    for(UINT32 N=0;N<10;++N){A=Next();if(A.Type==CrUsbMscNone)break;
      if(A.Type==CrUsbMscStallIn||A.Type==CrUsbMscStallOut){Done(A);if(!CrUsbMscClearHalt(&C,A.Type==CrUsbMscStallIn))break;}
      else{if(A.Buffer==C.Csw){Done(A);break;}if(A.Type==CrUsbMscBulkOut)memset(A.Buffer,0,A.Length);Done(A);}}
    ++Cases;
  }
}
int main(int argc,char **argv){
  Disk=malloc(CR_USB_MSC_DISK_BYTES);assert(Disk);assert(CrUsbMscInit(NULL,Disk,CR_USB_MSC_DISK_BYTES)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInit(&C,Disk,CR_USB_MSC_DISK_BYTES-1)==CR_INVALID_PARAMETER);assert(CrUsbMscInit(&C,&C,CR_USB_MSC_DISK_BYTES)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInit(&C,Disk,CR_USB_MSC_DISK_BYTES)==CR_SUCCESS);assert(CrUsbMscInit(&C,Disk,CR_USB_MSC_DISK_BYTES)==CR_INVALID_PARAMETER);
  assert(Disk[510]==0x55&&Disk[511]==0xaa&&Disk[450]==6&&L32(Disk+454)==2048&&L32(Disk+458)==30720);
  if(argc==2){FILE *F=fopen(argv[1],"wb");assert(F&&fwrite(Disk,1,CR_USB_MSC_DISK_BYTES,F)==CR_USB_MSC_DISK_BYTES);assert(!fclose(F));}
  assert(CrUsbMscSetMaxPacket(&C,65)==CR_INVALID_PARAMETER);assert(CrUsbMscComplete(&C,TRUE,0)==CR_INVALID_PARAMETER);
  assert(CrUsbMscNext(&C,(CR_USB_MSC_ACTION*)Disk)==CR_INVALID_PARAMETER);
  Matrix();Invalid();ScsiCommands();AdditionalScsi();Streaming();CdbCorpus();free(Disk);
  printf("PASS: %u cases, %u host transactions, all 13 BOT cases and 4096 CDB corpus entries\n",Cases,Transactions);return 0;
}
