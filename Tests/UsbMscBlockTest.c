/* Actual BOT core, sparse independent block medium and failure injection.
 * SPDX-License-Identifier: MIT */
#include <Library/UsbMscCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG_LBA 0x100000020ULL
#define BLOCKS_MAX 512U
static CR_USB_MSC C;
static struct {
  UINT32 Size;UINT64 Last;BOOLEAN ReadOnly;
  UINT32 Reads,Writes,Flushes,Checks,FailRead,FailWrite,FailFlush,FailCheck;
  CR_STATUS Failure;
  struct {UINT64 Lba;UINT8 *Data;} Slots[BLOCKS_MAX];
  UINT32 SlotCount;
  CHAR8 Order[4096];UINT32 OrderCount;
} M;
static UINT32 Tag,Cases;
static UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static UINT32 Be32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
static UINT64 Be64(CONST UINT8 *P){return ((UINT64)Be32(P)<<32)|Be32(P+4);}
static void PutLe(UINT8 *P,UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>(I*8));}
static void PutBe(UINT8 *P,UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>((3-I)*8));}
static void PutBe64(UINT8 *P,UINT64 V){PutBe(P,(UINT32)(V>>32));PutBe(P+4,(UINT32)V);}
static void Io(CONST VOID *Context,UINT64 Lba,UINT32 Blocks,CONST VOID *Buffer){
  assert(Context==&M&&Blocks&&Blocks<=65536/M.Size&&Buffer==C.Discard);
  assert(Lba<=M.Last&&(UINT64)(Blocks-1)<=M.Last-Lba);
}
static UINT8 *Find(UINT64 Lba){
  for(UINT32 I=0;I<M.SlotCount;++I)if(M.Slots[I].Lba==Lba)return M.Slots[I].Data;
  return NULL;
}
static CR_STATUS Read(VOID *Context,UINT64 Lba,UINT32 Blocks,VOID *Buffer){
  Io(Context,Lba,Blocks,Buffer);++M.Reads;M.Order[M.OrderCount++]='R';assert(M.OrderCount<sizeof M.Order);
  if(M.Reads==M.FailRead)return M.Failure;
  UINT8 *B=Buffer;
  for(UINT32 I=0;I<Blocks;++I){UINT8 *D=Find(Lba+I);
    if(D)memcpy(B+I*M.Size,D,M.Size);
    else for(UINT32 J=0;J<M.Size;++J)B[I*M.Size+J]=(UINT8)((Lba+I)^J^0x5a);
  }
  return CR_SUCCESS;
}
static CR_STATUS Write(VOID *Context,UINT64 Lba,UINT32 Blocks,CONST VOID *Buffer){
  Io(Context,Lba,Blocks,Buffer);assert(!M.ReadOnly);++M.Writes;M.Order[M.OrderCount++]='W';assert(M.OrderCount<sizeof M.Order);
  if(M.Writes==M.FailWrite)return M.Failure;
  CONST UINT8 *B=Buffer;
  for(UINT32 I=0;I<Blocks;++I){UINT8 *D=Find(Lba+I);
    if(!D){assert(M.SlotCount<BLOCKS_MAX);D=malloc(M.Size);assert(D);M.Slots[M.SlotCount].Data=D;M.Slots[M.SlotCount++].Lba=Lba+I;}
    memcpy(D,B+I*M.Size,M.Size);
  }
  return CR_SUCCESS;
}
static CR_STATUS Flush(VOID *Context){
  assert(Context==&M);++M.Flushes;M.Order[M.OrderCount++]='F';assert(M.OrderCount<sizeof M.Order);
  return M.Flushes==M.FailFlush?M.Failure:CR_SUCCESS;
}
static CR_STATUS Check(VOID *Context){
  assert(Context==&M);++M.Checks;
  return M.Checks==M.FailCheck?M.Failure:CR_SUCCESS;
}
static void FreeMedium(void){for(UINT32 I=0;I<M.SlotCount;++I)free(M.Slots[I].Data);}
static void Init(UINT32 Size,UINT64 Last,BOOLEAN ReadOnly){
  FreeMedium();memset(&M,0,sizeof M);memset(&C,0,sizeof C);M.Size=Size;M.Last=Last;M.ReadOnly=ReadOnly;M.Failure=CR_DEVICE_ERROR;
  CR_USB_MSC_BLOCK_BACKEND B={&M,Size,Last,ReadOnly,Read,ReadOnly?NULL:Write,ReadOnly?NULL:Flush,Check};
  assert(CrUsbMscInitBlock(&C,&B)==CR_SUCCESS);memset(&B,0,sizeof B); /* descriptor copied */
  assert(CrUsbMscSetMaxPacket(&C,512)==CR_SUCCESS);assert(!M.Reads&&!M.Writes&&!M.Flushes&&!M.Checks);
}
static CR_USB_MSC_ACTION Next(void){CR_USB_MSC_ACTION A;assert(CrUsbMscNext(&C,&A)==CR_SUCCESS);assert(A.Length<=65536);return A;}
static void Done(CR_USB_MSC_ACTION A){assert(CrUsbMscComplete(&C,TRUE,A.Length)==CR_SUCCESS);}
static void Raw(CONST UINT8 *B,UINT8 Length,UINT32 Host,BOOLEAN In){
  CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkOut&&A.Length==31);UINT8 *W=A.Buffer;memset(W,0,31);
  PutLe(W,0x43425355);PutLe(W+4,++Tag);PutLe(W+8,Host);W[12]=In?0x80:0;W[14]=Length;memcpy(W+15,B,Length);Done(A);
}
static void Command(UINT8 Op,UINT64 Lba,UINT32 Blocks,UINT32 Host,BOOLEAN In){
  UINT8 B[16]={0};B[0]=Op;
  if(Op==0x88||Op==0x8a||Op==0x91){PutBe64(B+2,Lba);PutBe(B+10,Blocks);Raw(B,16,Host,In);}
  else if(Op==0xa8||Op==0xaa){assert(Lba<=0xffffffffU);PutBe(B+2,(UINT32)Lba);PutBe(B+6,Blocks);Raw(B,12,Host,In);}
  else{assert(Lba<=0xffffffffU&&Blocks<=65535);PutBe(B+2,(UINT32)Lba);B[7]=(UINT8)(Blocks>>8);B[8]=(UINT8)Blocks;Raw(B,10,Host,In);}
}
static void Finish(UINT8 Status,UINT32 Residue,UINT32 InBytes,UINT32 OutBytes){
  UINT32 In=0,Out=0,Budget=8192;
  while(--Budget){CR_USB_MSC_ACTION A=Next();assert(A.Type!=CrUsbMscNone);
    if(A.Type==CrUsbMscStallIn||A.Type==CrUsbMscStallOut){Done(A);assert(CrUsbMscClearHalt(&C,A.Type==CrUsbMscStallIn));continue;}
    if(A.Buffer==C.Csw){UINT8 *W=A.Buffer;assert(A.Type==CrUsbMscBulkIn&&A.Length==13&&Le32(W)==0x53425355&&Le32(W+4)==Tag);
      assert(W[12]==Status&&Le32(W+8)==Residue);Done(A);break;}
    if(A.Type==CrUsbMscBulkIn)In+=A.Length;
    else {memset(A.Buffer,0x96,A.Length);Out+=A.Length;}
    Done(A);
  }
  assert(Budget&&In==InBytes&&Out==OutBytes);++Cases;
}
static void Reply(UINT8 *Out,UINT32 Bytes){CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkIn&&A.Length==Bytes);memcpy(Out,A.Buffer,Bytes);Done(A);Finish(0,0,0,0);}
static void Sense(UINT8 Key,UINT8 Asc,UINT8 Ascq){UINT8 B[6]={3,0,0,0,18,0},R[18];Raw(B,6,18,TRUE);Reply(R,18);assert(R[2]==Key&&R[12]==Asc&&R[13]==Ascq);}
static void Reset(void){assert(CrUsbMscBusReset(&C)==CR_SUCCESS);}
static void Admission(void){
  memset(&C,0,sizeof C);CR_USB_MSC_BLOCK_BACKEND B={&M,512,0,FALSE,Read,Write,Flush,NULL};
  assert(CrUsbMscInitBlock(NULL,&B)==CR_INVALID_PARAMETER);assert(CrUsbMscInitBlock(&C,NULL)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInitBlock(&C,(VOID*)&C)==CR_INVALID_PARAMETER);
  UINT32 Sizes[]={0,1,511,513,1000,65537,0x80000000U};
  for(UINTN I=0;I<sizeof Sizes/sizeof Sizes[0];++I){B.BlockSize=Sizes[I];assert(CrUsbMscInitBlock(&C,&B)==CR_INVALID_PARAMETER);}
  B.BlockSize=512;B.Read=NULL;assert(CrUsbMscInitBlock(&C,&B)==CR_INVALID_PARAMETER);B.Read=Read;
  B.Write=NULL;assert(CrUsbMscInitBlock(&C,&B)==CR_INVALID_PARAMETER);B.Write=Write;
  B.Flush=NULL;assert(CrUsbMscInitBlock(&C,&B)==CR_INVALID_PARAMETER);B.Flush=Flush;
  B.ReadOnly=2;assert(CrUsbMscInitBlock(&C,&B)==CR_INVALID_PARAMETER);B.ReadOnly=FALSE;
  assert(CrUsbMscInitBlock(&C,&B)==CR_SUCCESS);assert(CrUsbMscInitBlock(&C,&B)==CR_INVALID_PARAMETER);
}
static void Geometry(void){
  UINT8 B[16]={0},R[96];Init(4096,0x200000fffULL,FALSE);
  B[0]=0x25;Raw(B,10,8,TRUE);Reply(R,8);assert(Be32(R)==0xffffffffU&&Be32(R+4)==4096);
  B[0]=0x9e;B[1]=0x10;B[13]=32;Raw(B,16,32,TRUE);Reply(R,32);assert(Be64(R)==M.Last&&Be32(R+8)==4096);
  memset(B,0,sizeof B);B[0]=0x23;B[8]=12;Raw(B,10,12,TRUE);Reply(R,12);assert(Be32(R+4)==0xffffffffU&&R[8]==2&&R[9]==0&&R[10]==16&&R[11]==0);
  assert(!M.Reads&&!M.Writes&&!M.Flushes);
  Command(0x88,M.Last,1,4096,TRUE);CR_USB_MSC_ACTION A=Next();assert(A.Length==4096);
  for(UINT32 I=0;I<A.Length;++I)assert(((UINT8*)A.Buffer)[I]==(UINT8)(M.Last^I^0x5a));Done(A);Finish(0,0,0,0);
  Command(0x88,M.Last,2,8192,TRUE);Finish(1,8192,0,0);Sense(5,0x21,0);assert(M.Reads==1);
  Command(0x88,0,0x100000U,512,TRUE);Finish(1,512,0,0);Sense(5,0x24,0);assert(M.Reads==1);
  Init(65536,~(UINT64)0,FALSE);memset(B,0,sizeof B);B[0]=0x9e;B[1]=0x10;B[13]=32;Raw(B,16,32,TRUE);Reply(R,32);assert(Be64(R)==~(UINT64)0&&Be32(R+8)==65536);
  Command(0x88,~(UINT64)0,1,65536,TRUE);Finish(0,0,65536,0);assert(M.Reads==1);
  Command(0x88,~(UINT64)0,2,131072,TRUE);Finish(1,131072,0,0);Sense(5,0x21,0);assert(M.Reads==1);
  Command(0x8a,~(UINT64)0-1,2,131072,FALSE);Finish(0,0,0,131072);assert(M.Writes==2&&M.Flushes==2);
  Command(0x88,~(UINT64)0,1,65536,TRUE);A=Next();
  for(UINT32 I=0;I<A.Length;++I)assert(((UINT8*)A.Buffer)[I]==0x96);Done(A);Finish(0,0,0,0);
  Init(512,0,FALSE);Command(0x28,0,1,512,TRUE);Finish(0,0,512,0);assert(M.Reads==1);
}
static void Streaming(void){
  Init(4096,BIG_LBA+1000,FALSE);Command(0x8a,BIG_LBA,35,35*4096,FALSE);
  UINT32 Offset=0;
  while(Offset<35*4096){CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkOut&&A.Buffer==C.Discard);
    for(UINT32 I=0;I<A.Length;++I)((UINT8*)A.Buffer)[I]=(UINT8)((Offset+I)*17+3);
    assert(M.Flushes==M.Writes);Done(A);assert(M.Flushes==M.Writes);Offset+=A.Length;
  }
  assert(M.Writes==3&&M.Flushes==3&&!memcmp(M.Order,"WFWFWF",6));Finish(0,0,0,0);
  Command(0x88,BIG_LBA,35,35*4096,TRUE);Offset=0;
  while(Offset<35*4096){CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkIn);
    for(UINT32 I=0;I<A.Length;++I)assert(((UINT8*)A.Buffer)[I]==(UINT8)((Offset+I)*17+3));Offset+=A.Length;Done(A);}
  Finish(0,0,0,0);assert(M.Reads==3);Reset();
  Command(0x91,0,0,0,FALSE);Finish(0,0,0,0);assert(M.Flushes==4);
  Command(0x35,0,0,0,FALSE);Finish(0,0,0,0);assert(M.Flushes==5);
}
static void NoIoMalformed(void){
  Init(4096,BIG_LBA+1000,FALSE);
  Command(0x8a,BIG_LBA,1,2048,FALSE);Finish(2,2048,0,2048);Reset();
  Command(0x8a,BIG_LBA,1,4096,TRUE);Finish(2,4096,0,0);Reset();
  Command(0x88,BIG_LBA,1,4096,FALSE);Finish(2,4096,0,4096);Reset();
  Command(0x8a,BIG_LBA,1,0,FALSE);Finish(2,0,0,0);Reset();
  UINT8 B[16]={0x8a};PutBe64(B+2,BIG_LBA);PutBe(B+10,1);B[1]=1;Raw(B,16,4096,FALSE);Finish(1,4096,0,4096);
  B[1]=0;B[14]=1;Raw(B,16,4096,FALSE);Finish(1,4096,0,4096);
  B[14]=0;B[15]=1;Raw(B,16,4096,FALSE);Finish(1,4096,0,4096);
  for(UINT32 N=0;N<31;++N){Reset();CR_USB_MSC_ACTION A=Next();memset(A.Buffer,0,31);PutLe(A.Buffer,0x43425355);((UINT8*)A.Buffer)[14]=16;
    assert(CrUsbMscComplete(&C,TRUE,N)==CR_SUCCESS);Reset();}
  Command(0x8a,BIG_LBA,1,4096,FALSE);CR_USB_MSC_ACTION A=Next();memset(A.Buffer,0x96,A.Length);
  assert(CrUsbMscComplete(&C,TRUE,A.Length-1)==CR_BAD_BUFFER_SIZE);Reset();
  assert(!M.Reads&&!M.Writes&&!M.Flushes&&!M.SlotCount);
  /* Case7 may read one complete backend block but exposes only host bytes. */
  Command(0x88,BIG_LBA,1,256,TRUE);Finish(2,0,256,0);assert(M.Reads==1&&!M.Writes);Reset();
}
static void WriteProtect(void){
  Init(4096,BIG_LBA+100,TRUE);UINT8 B[16]={0x1a,8,8,0,24,0},R[32];
  Raw(B,6,24,TRUE);Reply(R,24);assert(R[2]==0x80&&R[6]==0);
  memset(B,0,sizeof B);B[0]=0x5a;B[1]=8;B[2]=8;B[8]=28;Raw(B,10,28,TRUE);Reply(R,28);assert(R[3]==0x80&&R[10]==0);
  Command(0x8a,BIG_LBA,1,4096,FALSE);Finish(1,4096,0,4096);Sense(7,0x27,0);
  assert(!M.Writes&&!M.Flushes);Command(0x88,BIG_LBA,1,4096,TRUE);Finish(0,0,4096,0);
  Command(0x35,0,0,0,FALSE);Finish(0,0,0,0);assert(M.Reads==1&&!M.Writes&&!M.Flushes);
}
static void BotMatrix(void){
  struct {UINT8 Op;UINT32 Host;BOOLEAN In;UINT8 Status;UINT32 Residue,InBytes,OutBytes,Reads,Writes;} V[]={
    {0,0,FALSE,0,0,0,0,0,0},{0x88,0,FALSE,2,0,0,0,0,0},{0x8a,0,TRUE,2,0,0,0,0,0},
    {0,1024,TRUE,0,1024,0,0,0,0},{0x88,1024,TRUE,0,512,512,0,1,0},{0x88,512,TRUE,0,0,512,0,1,0},
    {0x88,256,TRUE,2,0,256,0,1,0},{0x8a,512,TRUE,2,512,0,0,0,0},
    {0,1024,FALSE,0,1024,0,1024,0,0},{0x88,512,FALSE,2,512,0,512,0,0},
    {0x8a,1024,FALSE,0,512,0,1024,0,1},{0x8a,512,FALSE,0,0,0,512,0,1},{0x8a,256,FALSE,2,256,0,256,0,0}
  };
  for(UINTN I=0;I<sizeof V/sizeof V[0];++I){
    Init(512,BIG_LBA+100,FALSE);
    if(V[I].Op)Command(V[I].Op,BIG_LBA,1,V[I].Host,V[I].In);
    else{UINT8 B[6]={0};Raw(B,6,V[I].Host,V[I].In);}
    Finish(V[I].Status,V[I].Residue,V[I].InBytes,V[I].OutBytes);
    assert(M.Reads==V[I].Reads&&M.Writes==V[I].Writes&&M.Flushes==M.Writes);
  }
}
static void Failures(void){
  Init(512,BIG_LBA+1000,FALSE);M.FailRead=2;
  Command(0x88,BIG_LBA,384,384*512,TRUE);Finish(1,256*512,128*512,0);Sense(3,0x11,0);assert(M.Reads==2);
  Init(512,BIG_LBA+1000,FALSE);M.FailWrite=2;
  Command(0x8a,BIG_LBA,384,384*512,FALSE);Finish(1,256*512,0,384*512);Sense(3,0x0c,2);assert(M.Writes==2&&M.Flushes==1&&M.SlotCount==128);
  Init(512,BIG_LBA+1000,FALSE);M.FailFlush=2;
  Command(0x8a,BIG_LBA,384,384*512,FALSE);Finish(1,256*512,0,384*512);Sense(3,0x0c,2);assert(M.Writes==2&&M.Flushes==2&&M.SlotCount==256);
  Init(512,BIG_LBA+1000,FALSE);M.FailFlush=1;
  Command(0x35,0,0,0,FALSE);Finish(1,0,0,0);Sense(3,0x0c,2);assert(!M.Writes&&M.Flushes==1);
  Init(512,BIG_LBA+1000,FALSE);M.Failure=CR_BUSY;M.FailWrite=1;
  Command(0x8a,BIG_LBA,1,512,FALSE);Finish(1,512,0,512);Sense(7,0x27,0);assert(M.Writes==1&&!M.Flushes);
  Init(512,BIG_LBA+1000,FALSE);M.Failure=(CR_STATUS)1;M.FailRead=1;
  Command(0x88,BIG_LBA,1,512,TRUE);Finish(1,512,0,0);Sense(3,0x11,0);assert(M.Reads==1);
  Init(512,BIG_LBA+1000,FALSE);M.FailRead=1;
  Command(0x88,BIG_LBA,1,256,TRUE);Finish(2,256,0,0);assert(M.Reads==1);
  CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscStallIn); /* phase recovery survives I/O error */
  Done(A);assert(!CrUsbMscClearHalt(&C,TRUE));Reset();
  for(UINT32 Which=0;Which<3;++Which){Init(4096,BIG_LBA+1000,FALSE);M.FailRead=1;M.Failure=Which==0?CR_NOT_FOUND:Which==1?CR_ABORT:CR_NOT_READY;
    Command(0x88,BIG_LBA,1,4096,TRUE);Finish(1,4096,0,0);Sense(Which==1?6:2,Which==0?0x3a:Which==1?0x28:4,Which==2?1:0);
    Reset();M.FailRead=0;
    if(Which<2){Command(0x88,BIG_LBA,1,4096,TRUE);Finish(1,4096,0,0);assert(M.Reads==1);
      assert(CrUsbMscReset(&C)==CR_SUCCESS);assert(CrUsbMscClearHalt(&C,TRUE)&&CrUsbMscClearHalt(&C,FALSE));
      Command(0x8a,BIG_LBA,1,4096,FALSE);Finish(1,4096,0,4096);assert(!M.Writes&&!M.Flushes);
    }else{Command(0x88,BIG_LBA,1,4096,TRUE);Finish(0,0,4096,0);assert(M.Reads==2);}
  }
}
static void MediaChecks(void){
  UINT8 B[16]={0},R[32];Init(4096,BIG_LBA+100,TRUE);
  Raw(B,6,0,FALSE);Finish(0,0,0,0);assert(M.Checks==1&&!M.Reads);
  B[1]=1;Raw(B,6,0,FALSE);Finish(1,0,0,0);assert(M.Checks==1); /* reserved TUR field */
  memset(B,0,sizeof B);B[0]=0x25;Raw(B,10,8,FALSE);Finish(2,8,0,8);Reset();assert(M.Checks==1);
  B[6]=1;Raw(B,10,8,TRUE);Finish(1,8,0,0);assert(M.Checks==1);
  B[6]=0;Raw(B,10,8,TRUE);Reply(R,8);assert(M.Checks==2);
  B[0]=0x9e;B[1]=0x10;B[13]=32;Raw(B,16,32,TRUE);Reply(R,32);assert(M.Checks==3);
  M.FailCheck=4;M.Failure=CR_ABORT;memset(B,0,sizeof B);Raw(B,6,0,FALSE);Finish(1,0,0,0);Sense(6,0x28,0);
  Reset();M.FailCheck=0;B[0]=0x25;Raw(B,10,8,TRUE);Finish(1,8,0,0);assert(M.Checks==4&&!M.Reads);
  Init(512,100,FALSE);M.FailCheck=1;M.Failure=CR_NOT_FOUND;B[0]=0x25;Raw(B,10,8,TRUE);Finish(1,8,0,0);Sense(2,0x3a,0);assert(M.Checks==1);
}
static void PhysicalVerify(void){
  Init(512,100,FALSE);Command(0x2f,0,1,0,FALSE);Finish(1,0,0,0);Sense(5,0x20,0);
  assert(!M.Checks&&!M.Reads&&!M.Writes&&!M.Flushes);
}
static void NoDataOperations(void){
  /* Dn with Hi/Ho is BOT case4/9: execute the valid command, retain the
     entire host residue, then halt IN or discard OUT. It is not case8/10. */
  for(UINT32 Op=0;Op<3;++Op)for(UINT32 In=0;In<2;++In)for(UINT32 Fail=0;Fail<2;++Fail){
    Init(512,100,FALSE);M.Failure=Op?CR_DEVICE_ERROR:CR_NOT_FOUND;
    if(Fail){if(Op)M.FailFlush=1;else M.FailCheck=1;}
    if(Op)Command(Op==1?0x35:0x91,0,0,256,(BOOLEAN)In);
    else{UINT8 B[6]={0};Raw(B,6,256,(BOOLEAN)In);}
    Finish((UINT8)Fail,256,0,In?0:256);
    assert(M.Checks==(Op?0:1)&&M.Flushes==(Op?1:0)&&!M.Reads&&!M.Writes);
    if(Fail)Sense(Op?3:2,Op?0x0c:0x3a,Op?2:0);
  }
  for(UINT32 Op=0;Op<3;++Op){
    Init(512,100,FALSE);UINT8 B[16]={0};B[0]=Op==0?0:Op==1?0x35:0x91;B[1]=1;
    Raw(B,Op==0?6:Op==1?10:16,256,FALSE);Finish(1,256,0,256);
    assert(!M.Checks&&!M.Flushes&&!M.Reads&&!M.Writes);
  }
}
static void SynchronizeRange(void){
  for(UINT32 Sixteen=0;Sixteen<2;++Sixteen)for(UINT32 Excess=0;Excess<2;++Excess){
    UINT64 Last=Sixteen?BIG_LBA+100:100;UINT32 Host=Excess?256:0;
    Init(512,Last,FALSE);
    Command(Sixteen?0x91:0x35,Last+1,0,Host,FALSE);Finish(1,Host,0,Host);Sense(5,0x21,0);
    assert(!M.Flushes&&!M.Reads&&!M.Writes);
    Command(Sixteen?0x91:0x35,Last,0,0,FALSE);Finish(0,0,0,0);assert(M.Flushes==1);
  }
  /* Zero-length READ/WRITE keeps its existing no-operation meaning. */
  Init(512,100,FALSE);
  Command(0x28,0xffffffffU,0,0,FALSE);Finish(0,0,0,0);
  Command(0x2a,0xffffffffU,0,0,FALSE);Finish(0,0,0,0);
  Command(0x88,~(UINT64)0,0,0,FALSE);Finish(0,0,0,0);
  Command(0x8a,~(UINT64)0,0,0,FALSE);Finish(0,0,0,0);
  assert(!M.Checks&&!M.Flushes&&!M.Reads&&!M.Writes);
}
int main(int Argc,char **Argv){
  if(Argc==2){
    if(!strcmp(Argv[1],"verify"))PhysicalVerify();
    else if(!strcmp(Argv[1],"no-data"))NoDataOperations();
    else if(!strcmp(Argv[1],"sync-range"))SynchronizeRange();
    else assert(!"unknown regression selector");
    FreeMedium();return 0;
  }
  Admission();Geometry();Streaming();NoIoMalformed();WriteProtect();BotMatrix();Failures();MediaChecks();
  PhysicalVerify();NoDataOperations();SynchronizeRange();FreeMedium();
  printf("PASS: %u block-backed SCSI completions; sparse >2TiB, 4KiB/64KiB, UINT64 LBA, readonly, durable writes, media latch and BOT faults\n",Cases);return 0;}
