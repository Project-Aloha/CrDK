/* Two independent media behind the actual sequential BOT core. SPDX-License-Identifier: MIT */
#include <Library/UsbMscCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MEDIA_BYTES (3U*65536U)
typedef struct {
  UINT32 Size,Reads,Writes,Flushes,Checks,FailRead,FailWrite,FailFlush;
  UINT64 Base,Last;
  CR_STATUS Failure,CheckFailure;
  BOOLEAN ReadOnly;
  UINT8 Data[MEDIA_BYTES];
} MEDIUM;
static CR_USB_MSC C;
static MEDIUM M[2];
static UINT32 Tag,Cases,ExpectedLun;
static UINT8 Output[MEDIA_BYTES];
static UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static UINT32 Be32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
static UINT64 Be64(CONST UINT8 *P){return ((UINT64)Be32(P)<<32)|Be32(P+4);}
static void PutLe(UINT8 *P,UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
static void PutBe(UINT8 *P,UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>((3-I)*8));}
static void PutBe64(UINT8 *P,UINT64 V){PutBe(P,(UINT32)(V>>32));PutBe(P+4,(UINT32)V);}
static UINT8 Pattern(UINT32 Lun,UINT32 Offset){return (UINT8)(0x39+Lun*37+Offset*19);}
static MEDIUM *Selected(VOID *Context){assert(ExpectedLun<2&&Context==&M[ExpectedLun]);return Context;}
static UINT32 Offset(MEDIUM *D,UINT64 Lba,UINT32 Blocks,CONST VOID *Buffer){
  assert(Blocks&&Blocks<=65536/D->Size&&Buffer==C.Discard);
  assert(Lba>=D->Base&&Lba<=D->Last&&(UINT64)(Blocks-1)<=D->Last-Lba);
  assert(Lba-D->Base<MEDIA_BYTES/D->Size);
  UINT32 O=(UINT32)(Lba-D->Base)*D->Size;assert(Blocks*D->Size<=MEDIA_BYTES-O);return O;
}
static CR_STATUS Read(VOID *Context,UINT64 Lba,UINT32 Blocks,VOID *Buffer){
  MEDIUM *D=Selected(Context);UINT32 O=Offset(D,Lba,Blocks,Buffer);
  if(++D->Reads==D->FailRead)return D->Failure;
  memcpy(Buffer,D->Data+O,Blocks*D->Size);return CR_SUCCESS;
}
static CR_STATUS Write(VOID *Context,UINT64 Lba,UINT32 Blocks,CONST VOID *Buffer){
  MEDIUM *D=Selected(Context);assert(!D->ReadOnly);UINT32 O=Offset(D,Lba,Blocks,Buffer);
  if(++D->Writes==D->FailWrite)return D->Failure;
  memcpy(D->Data+O,Buffer,Blocks*D->Size);return CR_SUCCESS;
}
static CR_STATUS Flush(VOID *Context){MEDIUM *D=Selected(Context);return ++D->Flushes==D->FailFlush?D->Failure:CR_SUCCESS;}
static CR_STATUS Check(VOID *Context){MEDIUM *D=Selected(Context);++D->Checks;return D->CheckFailure;}
static void Backends(CR_USB_MSC_BLOCK_BACKEND *B){
  for(UINT32 I=0;I<2;++I){B[I]=(CR_USB_MSC_BLOCK_BACKEND){&M[I],M[I].Size,M[I].Last,M[I].ReadOnly,
    Read,M[I].ReadOnly?NULL:Write,M[I].ReadOnly?NULL:Flush,Check};}
}
static void Init(UINT8 Count,BOOLEAN SecondReadOnly){
  memset(&C,0,sizeof C);memset(M,0,sizeof M);
  for(UINT32 I=0;I<2;++I){
    M[I].Size=I?4096:512;M[I].Base=I?0x100000001ULL:8;M[I].Last=M[I].Base+511;
    M[I].ReadOnly=I&&SecondReadOnly;M[I].Failure=CR_DEVICE_ERROR;
    for(UINT32 J=0;J<MEDIA_BYTES;++J)M[I].Data[J]=Pattern(I,J);
  }
  CR_USB_MSC_BLOCK_BACKEND B[2];Backends(B);
  assert(CrUsbMscInitBlocks(&C,B,Count)==CR_SUCCESS);memset(B,0,sizeof B);
  UINT8 Max=0xff;assert(CrUsbMscGetMaxLun(&C,&Max)==CR_SUCCESS&&Max==Count-1);
  assert(CrUsbMscSetMaxPacket(&C,512)==CR_SUCCESS);
  for(UINT32 I=0;I<2;++I)assert(!M[I].Reads&&!M[I].Writes&&!M[I].Flushes&&!M[I].Checks);
}
static CR_USB_MSC_ACTION Next(void){CR_USB_MSC_ACTION A;assert(CrUsbMscNext(&C,&A)==CR_SUCCESS);assert(A.Length<=65536);return A;}
static void Done(CR_USB_MSC_ACTION A){assert(CrUsbMscComplete(&C,TRUE,A.Length)==CR_SUCCESS);}
static void Raw(UINT8 Lun,CONST UINT8 *B,UINT8 Length,UINT32 Host,BOOLEAN In){
  ExpectedLun=Lun;CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkOut&&A.Length==31);
  UINT8 *W=A.Buffer;memset(W,0,31);PutLe(W,0x43425355);PutLe(W+4,++Tag);PutLe(W+8,Host);
  W[12]=In?0x80:0;W[13]=Lun;W[14]=Length;memcpy(W+15,B,Length);Done(A);
}
static void Command(UINT8 Lun,UINT8 Op,UINT64 Lba,UINT32 Blocks,UINT32 Host,BOOLEAN In){
  UINT8 B[16]={0};B[0]=Op;PutBe64(B+2,Lba);PutBe(B+10,Blocks);Raw(Lun,B,16,Host,In);
}
static void Finish(UINT8 Status,UINT32 Residue,UINT32 InBytes,UINT32 OutBytes,UINT8 Fill){
  UINT32 In=0,Out=0,Budget=100;memset(Output,0xee,sizeof Output);
  while(--Budget){CR_USB_MSC_ACTION A=Next();assert(A.Type!=CrUsbMscNone);
    if(A.Type==CrUsbMscStallIn||A.Type==CrUsbMscStallOut){Done(A);assert(CrUsbMscClearHalt(&C,A.Type==CrUsbMscStallIn));continue;}
    if(A.Buffer==C.Csw){UINT8 *W=A.Buffer;assert(A.Type==CrUsbMscBulkIn&&A.Length==13&&Le32(W)==0x53425355&&Le32(W+4)==Tag);
      assert(W[12]==Status&&Le32(W+8)==Residue);Done(A);break;}
    if(A.Type==CrUsbMscBulkIn){assert(In+A.Length<=sizeof Output);memcpy(Output+In,A.Buffer,A.Length);In+=A.Length;}
    else{assert(A.Type==CrUsbMscBulkOut);memset(A.Buffer,Fill,A.Length);Out+=A.Length;}
    Done(A);
  }
  assert(Budget&&In==InBytes&&Out==OutBytes);++Cases;
}
static void Sense(UINT8 Lun,UINT8 Key,UINT8 Asc,UINT8 Ascq){
  UINT8 B[6]={3,0,0,0,18,0};Raw(Lun,B,6,18,TRUE);Finish(0,0,18,0,0);
  assert(Output[2]==Key&&Output[12]==Asc&&Output[13]==Ascq);
}
static void Tur(UINT8 Lun,UINT8 Status){UINT8 B[6]={0};Raw(Lun,B,6,0,FALSE);Finish(Status,0,0,0,0);}
static void UnitCommand(UINT8 Lun,UINT8 Op,UINT8 Flags,UINT8 Status){UINT8 B[6]={0};B[0]=Op;B[4]=Flags;Raw(Lun,B,6,0,FALSE);Finish(Status,0,0,0,0);}
static void BusReset(void){assert(CrUsbMscBusReset(&C)==CR_SUCCESS);}
static void ClassReset(void){assert(CrUsbMscReset(&C)==CR_SUCCESS);assert(CrUsbMscClearHalt(&C,TRUE));assert(CrUsbMscClearHalt(&C,FALSE));}
static UINT32 IoCalls(void){return M[0].Reads+M[0].Writes+M[0].Flushes+M[0].Checks+M[1].Reads+M[1].Writes+M[1].Flushes+M[1].Checks;}
static void Admission(void){
  Init(2,FALSE);CR_USB_MSC_BLOCK_BACKEND B[2];Backends(B);UINT8 Max=0xaa;
  assert(CrUsbMscGetMaxLun(NULL,&Max)==CR_INVALID_PARAMETER&&Max==0xaa);
  assert(CrUsbMscGetMaxLun(&C,NULL)==CR_INVALID_PARAMETER);
  assert(CrUsbMscGetMaxLun(&C,(UINT8*)&C)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInitBlocks(&C,B,2)==CR_INVALID_PARAMETER);
  memset(&C,0,sizeof C);assert(CrUsbMscGetMaxLun(&C,&Max)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInitBlocks(&C,B,0)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInitBlocks(&C,B,3)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInitBlocks(&C,B,255)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInitBlocks(&C,NULL,2)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInitBlocks(&C,(VOID*)&C,2)==CR_INVALID_PARAMETER);
  assert(CrUsbMscInitBlocks(&C,(VOID*)((UINT8*)&C+sizeof C-1),2)==CR_INVALID_PARAMETER);
  B[1].Read=NULL;assert(CrUsbMscInitBlocks(&C,B,2)==CR_INVALID_PARAMETER);
  for(UINTN I=0;I<sizeof C;++I)assert(!((UINT8*)&C)[I]);
  B[1].Read=Read;B[1].Flush=NULL;assert(CrUsbMscInitBlocks(&C,B,2)==CR_INVALID_PARAMETER);
  for(UINTN I=0;I<sizeof C;++I)assert(!((UINT8*)&C)[I]);
  B[1].Flush=Flush;assert(CrUsbMscInitBlocks(&C,B,2)==CR_SUCCESS);assert(!IoCalls());
}
static void Enumeration(void){
  for(UINT8 Count=1;Count<=2;++Count){Init(Count,FALSE);
    for(UINT8 Lun=0;Lun<Count;++Lun){
      UINT8 B[16]={0x12,0,0,0,36,0};Raw(Lun,B,6,36,TRUE);Finish(0,0,36,0,0);assert(Output[0]==0);
      B[1]=1;B[2]=0x80;B[4]=20;Raw(Lun,B,6,20,TRUE);Finish(0,0,20,0,0);
      assert(Output[3]==16&&!memcmp(Output+4,"000000000000000",15)&&Output[19]=='1'+Lun);
      memset(B,0,sizeof B);B[0]=0x9e;B[1]=0x10;B[13]=32;Raw(Lun,B,16,32,TRUE);Finish(0,0,32,0,0);
      assert(Be64(Output)==M[Lun].Last&&Be32(Output+8)==M[Lun].Size);
      for(UINT8 Select=0;Select<3;++Select){UINT32 List=Select==1?0:Count*8;
        for(UINT32 Allocation=0;Allocation<=32;++Allocation){
          memset(B,0,sizeof B);B[0]=0xa0;B[2]=Select;PutBe(B+6,Allocation);
          Raw(Lun,B,12,Allocation,TRUE);UINT32 Actual=Allocation<8+List?Allocation:8+List;
          Finish(0,Allocation-Actual,Actual,0,0);
          if(Actual>=4)assert(Be32(Output)==List);
          for(UINT32 I=8;I<Actual;++I)assert(Output[I]==(I==17?1:0));
        }
      }
    }
  }
}
static void Unsupported(void){
  for(UINT8 Count=1;Count<=2;++Count){Init(Count,FALSE);
    for(UINT8 Lun=Count;Lun<16;++Lun){
      for(UINT8 Op=0;Op<3;++Op){
        if(Op==0)Tur(Lun,1);
        else{Command(Lun,Op==1?0x88:0x8a,0,1,512,Op==1);Finish(1,512,0,Op==1?0:512,0);}
        Sense(Lun,5,0x25,0);assert(!IoCalls());
      }
      UINT8 B[6]={0x12,0,0,0,36,0};Raw(Lun,B,6,36,TRUE);Finish(0,0,36,0,0);
      assert(Output[0]==0x7f&&Output[4]==31);Sense(Lun,5,0x25,0);
      B[1]=1;B[2]=0x80;Raw(Lun,B,6,36,TRUE);Finish(1,36,0,0,0);assert(!IoCalls());
      Sense(0,0,0,0);if(Count==2)Sense(1,0,0,0);
    }
  }
  Init(2,FALSE);
  for(UINT32 Byte=0x10;Byte<256;++Byte){UINT8 B[6]={0};Raw((UINT8)Byte,B,6,0,FALSE);
    CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscStallIn);Done(A);assert(!CrUsbMscClearHalt(&C,TRUE));
    A=Next();assert(A.Type==CrUsbMscStallOut);Done(A);assert(!CrUsbMscClearHalt(&C,FALSE));
    A=Next();assert(A.Type==CrUsbMscNone);ClassReset();assert(!IoCalls());++Cases;
  }
  Tur(1,0);assert(M[1].Checks==1&&!M[0].Checks);
}
static void StreamingAndProtection(void){
  Init(2,FALSE);
  for(UINT8 Lun=0;Lun<2;++Lun){UINT32 Bytes=Lun?35*4096:300*512,Blocks=Bytes/M[Lun].Size;
    Command(Lun,0x88,M[Lun].Base,Blocks,Bytes,TRUE);Finish(0,0,Bytes,0,0);
    for(UINT32 I=0;I<Bytes;++I)assert(Output[I]==Pattern(Lun,I));
    Command(Lun,0x8a,M[Lun].Base,Blocks,Bytes,FALSE);Finish(0,0,0,Bytes,(UINT8)(0xb0+Lun));
    assert(M[Lun].Writes==3&&M[Lun].Flushes==3);
  }
  for(UINT8 Lun=0;Lun<2;++Lun){Command(Lun,0x88,M[Lun].Base,1,M[Lun].Size,TRUE);Finish(0,0,M[Lun].Size,0,0);
    for(UINT32 I=0;I<M[Lun].Size;++I)assert(Output[I]==0xb0+Lun);
    UINT32 Writes=M[Lun].Writes;Command(Lun,0x8a,M[Lun].Base,1,M[Lun].Size/2,FALSE);Finish(2,M[Lun].Size/2,0,M[Lun].Size/2,0xff);
    assert(M[Lun].Writes==Writes);BusReset();
  }
  Init(2,TRUE);UINT8 B[6]={0x1a,8,8,0,24,0};
  for(UINT8 Lun=0;Lun<2;++Lun){Raw(Lun,B,6,24,TRUE);Finish(0,0,24,0,0);assert(Output[2]==(Lun?0x80:0));}
  Command(1,0x8a,M[1].Base,1,4096,FALSE);Finish(1,4096,0,4096,0x33);Sense(1,7,0x27,0);
  assert(!M[1].Writes&&!M[1].Flushes);Command(0,0x8a,M[0].Base,1,512,FALSE);Finish(0,0,0,512,0x22);
  assert(M[0].Writes==1&&M[0].Flushes==1);Sense(0,0,0,0);
}
static void SenseAndReset(void){
  Init(2,FALSE);Command(0,0x88,M[0].Last+1,1,512,TRUE);Finish(1,512,0,0,0);
  UINT8 B[6]={0xff};Raw(1,B,6,0,FALSE);Finish(1,0,0,0,0);
  Sense(0,5,0x21,0);Sense(0,0,0,0);Sense(1,5,0x20,0);Sense(1,0,0,0);
  UnitCommand(0,0x1e,1,0);UnitCommand(0,0x1b,2,1);UnitCommand(1,0x1b,2,0);
  Tur(1,1);Tur(0,0);ClassReset();Sense(0,5,0x53,2);Sense(1,2,0x3a,0);
  Tur(1,1);UnitCommand(0,0x1b,2,0);Tur(0,1);BusReset();
  Sense(0,0,0,0);Sense(1,0,0,0);Tur(0,0);Tur(1,0);
  for(UINT8 Lun=0;Lun<2;++Lun){UnitCommand(Lun,0x1e,1,0);}
  BusReset();for(UINT8 Lun=0;Lun<2;++Lun)UnitCommand(Lun,0x1b,2,0);
}
static void MediaFaults(void){
  for(UINT8 Removed=0;Removed<2;++Removed)for(UINT8 Kind=0;Kind<2;++Kind){
    Init(2,FALSE);UINT8 Other=(UINT8)(1-Removed);M[Removed].CheckFailure=Kind?CR_ABORT:CR_NOT_FOUND;
    Tur(Removed,1);Tur(Other,0);Sense(Removed,Kind?6:2,Kind?0x28:0x3a,0);Sense(Other,0,0,0);
    M[Removed].CheckFailure=CR_SUCCESS;
    for(UINT8 Reset=0;Reset<2;++Reset){if(Reset)ClassReset();else BusReset();
      Tur(Removed,1);assert(M[Removed].Checks==1);Tur(Other,0);
      Command(Removed,0x8a,M[Removed].Base,1,M[Removed].Size,FALSE);Finish(1,M[Removed].Size,0,M[Removed].Size,0);
      assert(!M[Removed].Reads&&!M[Removed].Writes&&!M[Removed].Flushes);
      Sense(Removed,Kind?6:2,Kind?0x28:0x3a,0);
      Command(Other,0x88,M[Other].Base,1,M[Other].Size,TRUE);Finish(0,0,M[Other].Size,0,0);
    }
  }
  for(UINT8 Lun=0;Lun<2;++Lun)for(UINT8 Fault=0;Fault<3;++Fault){
    Init(2,FALSE);UINT32 Bytes=3*65536,Blocks=Bytes/M[Lun].Size;
    if(Fault==0)M[Lun].FailRead=2;else if(Fault==1)M[Lun].FailWrite=2;else M[Lun].FailFlush=2;
    Command(Lun,Fault?0x8a:0x88,M[Lun].Base,Blocks,Bytes,!Fault);
    Finish(1,2*65536,Fault?0:65536,Fault?Bytes:0,0x45);
    UINT8 Other=(UINT8)(1-Lun);Command(Other,0x88,M[Other].Base,1,M[Other].Size,TRUE);Finish(0,0,M[Other].Size,0,0);
    for(UINT32 I=0;I<M[Other].Size;++I)assert(Output[I]==Pattern(Other,I));
    Sense(Lun,3,Fault?0x0c:0x11,Fault?2:0);Sense(Other,0,0,0);
  }
}
static void OutstandingReset(void){
  for(UINT8 Lun=0;Lun<2;++Lun)for(UINT8 Bus=0;Bus<2;++Bus){
    Init(2,FALSE);Command(Lun,0x8a,M[Lun].Base,1,M[Lun].Size,FALSE);
    CR_USB_MSC_ACTION A=Next();assert(A.Type==CrUsbMscBulkOut);memset(A.Buffer,0x99,A.Length);
    /* Adapter drained the issued USB transfer without acknowledging payload. */
    if(Bus)BusReset();else ClassReset();assert(CrUsbMscComplete(&C,TRUE,A.Length)==CR_INVALID_PARAMETER);
    assert(!M[Lun].Writes&&!M[Lun].Flushes);UINT8 Other=(UINT8)(1-Lun);
    Command(Other,0x88,M[Other].Base,1,M[Other].Size,TRUE);Finish(0,0,M[Other].Size,0,0);
    for(UINT32 I=0;I<M[Other].Size;++I)assert(Output[I]==Pattern(Other,I));
  }
}
int main(void){Admission();Enumeration();Unsupported();StreamingAndProtection();SenseAndReset();MediaFaults();OutstandingReset();
  printf("PASS: %u multi-LUN completions/recoveries; independent geometry, data, sense, eject, reset, removal and backend failures\n",Cases);return 0;}
