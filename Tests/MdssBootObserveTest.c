/* Pure inherited-route observation versus the unchanged Android CTL decoder.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#endif
#include "../Library/MdssBoot8250CrLib/Internal.h"
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
extern VOID BootReferenceSeed(UINT32,VOID (*)(UINT32,UINT32,UINT32),CR_MDSS_BOOT_DMA_OBSERVATION *);
extern UINT32 BootReferenceStage(UINT32,UINT32,UINT32,UINT32[4]);
extern UINT32 BootReferenceDecode(CONST UINT32[4],UINT32 *,UINT32 *);
extern UINT32 BootReferenceIntf(UINT32);
extern CONST UINT32 BootReferenceIrqOffsets[12][3];
#define BIT(N) (1U<<(N))
STATIC struct {
  UINT32 Reg[CrMdssBootWindows][0x84000/4];
  CR_MDSS_BOOT_DMA_OBSERVATION Dma;
  UINT32 Calls,Reads,Dmas,Fail;
  CR_STATUS Failure;
} F;
STATIC UINT32 Checks;
STATIC VOID SetReg(UINT32 W,UINT32 O,UINT32 V)
{assert(W<CrMdssBootWindows&&!(O&3)&&O<sizeof(F.Reg[0]));F.Reg[W][O/4]=V;}
STATIC CR_STATUS Tick(VOID)
{return ++F.Calls==F.Fail?F.Failure:CR_SUCCESS;}
CR_STATUS BootReady(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W)
{
  assert(P->Io.Context==&F&&W<CrMdssBootWindows);CR_STATUS S=Tick();
  return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;
}
CR_STATUS BootRead(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 O,UINT32 *V)
{
  CR_STATUS S=BootReady(P,W);if(S!=CR_SUCCESS)return S;
  assert(!(O&3)&&O<sizeof(F.Reg[0]));
  /* IRQ clear and undefined SSPP+4 are never observational reads. */
  if(W==CrMdssBootDpu){
    for(UINT32 I=0;I<12;++I)assert(O!=BootReferenceIrqOffsets[I][2]);
    CONST UINT32 Base[]={0x4000,0x6000,0x8000,0xa000,0x24000,0x26000,0x28000,0x2a000};
    for(UINT32 I=0;I<8;++I)assert(O!=Base[I]+4);
  }
  ++F.Reads;S=Tick();if(S!=CR_SUCCESS)return CR_ERROR(S)?S:CR_NOT_READY;
  *V=F.Reg[W][O/4];return CR_SUCCESS;
}
STATIC CR_STATUS ObserveDma(VOID *C,UINT64 Device,UINT64 Bytes,CR_MDSS_BOOT_DMA_OBSERVATION *D)
{
  assert(C==&F);++F.Dmas;CR_STATUS S=Tick();if(S!=CR_SUCCESS)return S;
  assert(Device&&Bytes);*D=F.Dma;return CR_SUCCESS;
}
STATIC CR_MDSS_BOOT8250 Seed(UINT32 Index)
{
  memset(&F,0,sizeof(F));BootReferenceSeed(Index,SetReg,&F.Dma);
  F.Failure=CR_DEVICE_ERROR;CR_MDSS_BOOT8250 P={0};
  P.Io.Context=&F;P.Io.ObserveDma=ObserveDma;return P;
}
STATIC CR_MDSS_BOOT_OBSERVATION Good(UINT32 Index)
{
  CR_MDSS_BOOT8250 P=Seed(Index);CR_MDSS_BOOT_OBSERVATION O;
  assert(BootObserve(&P,&O)==CR_SUCCESS);assert(F.Dmas==1);
  assert(BootAdmit(&O)==CR_SUCCESS);assert(O.SourceIndex==Index);
  assert(O.SourceOffset==(Index?0x24000U:0x4000U));assert(O.Xin==(Index?1U:0U));
  assert(O.ForceOffset==0x2ac&&O.ForceMask==BIT(Index?8:0));
  assert(O.FrameDevice==0x9c000000&&O.FrameBytes==1080*4*2340);
  assert(O.Width==1080&&O.Height==2340&&O.Stride==4320);++Checks;return O;
}
STATIC VOID RejectReg(UINT32 W,UINT32 Off,UINT32 Value)
{
  CR_MDSS_BOOT8250 P=Seed(0);SetReg(W,Off,Value);CR_MDSS_BOOT_OBSERVATION O;
  assert(BootObserve(&P,&O)==CR_SUCCESS&&O.Revision==1);
  assert(BootAdmit(&O)!=CR_SUCCESS);assert(!O.FrameBytes&&!O.ForceMask);assert(F.Dmas==0);++Checks;
}
STATIC VOID StageTests(VOID)
{
  CONST UINT32 Stages[]={1,7,8,11,12,15};
  for(UINT32 Pipe=1;Pipe<=14;++Pipe)for(UINT32 Rect=0;Rect<2;++Rect)
    for(UINT32 J=0;J<sizeof(Stages)/sizeof(Stages[0]);++J){
      UINT32 W[4],Pipes[32],Virtual[32];
      if(!BootReferenceStage(Pipe,Rect,Stages[J],W))continue;
      UINT32 Count=BootReferenceDecode(W,Pipes,Virtual);
      CR_MDSS_BOOT_OBSERVATION O=Good(Pipe==9?4:0);
      memcpy(O.Ctl[0].Layer[0],W,sizeof(W));
      BOOLEAN Expected=Count==1&&!Virtual[0]&&(Pipes[0]==1||Pipes[0]==9)&&Stages[J]<=11;
      assert((BootAdmit(&O)==CR_SUCCESS)==Expected);++Checks;
    }
  assert(BootReferenceIntf(2)==2);assert(BootReferenceIntf(6)==6);
  CR_MDSS_BOOT_OBSERVATION O=Good(0);O.Ctl[0].Layer[0][0]|=BIT(18);
  assert(BootAdmit(&O)==CR_UNSUPPORTED);++Checks;
}
STATIC VOID Exclusions(VOID)
{
  for(UINT32 C=0;C<6;++C){
    UINT32 B=0x1000+C*0x200;
    CONST UINT32 Extras[]={0xec,0xf0,0xf8,0xe4,0x30,0xd4,0x134,0xc0,0xcc};
    for(UINT32 I=0;I<sizeof(Extras)/sizeof(Extras[0]);++I)RejectReg(CrMdssBootDpu,B+Extras[I],1);
    if(C){RejectReg(CrMdssBootDpu,B+0x14,BIT(17));RejectReg(CrMdssBootDpu,B+0xf4,2);RejectReg(CrMdssBootDpu,B+0xe8,1);}
    for(UINT32 L=0;L<6;++L)if(C||L){
      CONST UINT32 Off[]={L==5?0x24:L*4,0x40+L*4,0x70+L*4,0xa0+L*4};
      for(UINT32 I=0;I<4;++I)RejectReg(CrMdssBootDpu,B+Off[I],1);
    }
  }
  RejectReg(CrMdssBootTop,0,0x70000000);RejectReg(CrMdssBootTop,0x10,0x20);
  RejectReg(CrMdssBootDpu,0x1018,1); /* pending shadow flush is not a fetch proof */
  CONST UINT32 Split[]={0x2f4,0x2f8,0x3f0,0x450,0x330,0x338,0x370,0x374,0x334,0x33c};
  for(UINT32 I=0;I<sizeof(Split)/sizeof(Split[0]);++I)RejectReg(CrMdssBootDpu,Split[I],1);
  for(UINT32 I=0;I<6;++I){
    UINT32 B=0x70000+I*0x800;
    RejectReg(CrMdssBootDpu,B+0xa0,I?1:0);RejectReg(CrMdssBootDpu,B,1);RejectReg(CrMdssBootDpu,B+0x30,1);
  }
  for(UINT32 I=0;I<4;++I){
    UINT32 B=0x6a000+I*0x800;RejectReg(CrMdssBootDpu,B,1);
    RejectReg(CrMdssBootDpu,B+0x26c,1);RejectReg(CrMdssBootDpu,B+0x80,1);
    if(I!=1){RejectReg(CrMdssBootDpu,B+0x284,1);RejectReg(CrMdssBootDpu,B+0x2b4,1);RejectReg(CrMdssBootDpu,B+0x84,1);}
  }
  RejectReg(CrMdssBootDpu,0x6aa5c,0x60000);RejectReg(CrMdssBootDpu,0x6aa84,0);
  RejectReg(CrMdssBootDpu,0x80000,1);RejectReg(CrMdssBootDpu,0x80008,1);
  RejectReg(CrMdssBootDpu,0x8000c,1);RejectReg(CrMdssBootDpu,0x81800,1);
  RejectReg(CrMdssBootHost1,4,1);RejectReg(CrMdssBootHost1,8,4);RejectReg(CrMdssBootHost0,8,2);
  RejectReg(CrMdssBootHost0,0x330,1);RejectReg(CrMdssBootHost1,0x330,1);
  RejectReg(CrMdssBootHost1,0x110,2);
  CONST UINT32 Phy[]={0x3c,0x24,0x14,0x528,0x1c};
  for(UINT32 I=0;I<5;++I)RejectReg(CrMdssBootPhy1,Phy[I],I==2?0x20:1);
  RejectReg(CrMdssBootRegDma,4,1);
  for(UINT32 C=0;C<6;++C)for(UINT32 I=0;I<4;++I)RejectReg(CrMdssBootRegDma,0x14+C*0x10+I*4,1);
  for(UINT32 I=0;I<5;++I)RejectReg(CrMdssBootRegDma,0x160+I*4,1);
  for(UINT32 I=0;I<12;++I)for(UINT32 R=0;R<2;++R){
    UINT32 Forbidden=1;while(Forbidden&BootIrqAllowed[I])Forbidden<<=1;
    RejectReg(CrMdssBootDpu,BootReferenceIrqOffsets[I][R],Forbidden);
  }
}
STATIC VOID FetchTests(VOID)
{
  CONST UINT32 Pair[][2]={
    {0,0},{8,1},{0xc,1},{0x10,1},{0x14,0},{0x14,0xfffff000},{0x14,0x9c000001},
    {0x18,1},{0x1c,1},{0x20,1},{0x24,1},{0x24,4316},{0x24,4321},{0x24,0x10000},
    {0x28,1},{0x30,0},{0x30,0x80236ff},{0x30,0x236ef},{0x34,0},
    {0x38,0},{0x38,0x80000001},{0x70,1},{0x170,1},{0xb4,1},
    {0x100,1},{0x104,1},{0x108,1},{0x110,1},{0x114,1},{0x118,1},
    {0x120,1},{0x124,1},{0x128,1}};
  for(UINT32 I=0;I<sizeof(Pair)/sizeof(Pair[0]);++I)RejectReg(CrMdssBootDpu,0x4000+Pair[I][0],Pair[I][1]);
  CR_MDSS_BOOT_OBSERVATION O=Good(0);O.Sspp[0].Format|=BIT(8);assert(BootAdmit(&O)==CR_SUCCESS);++Checks;
  O=Good(0);O.Sspp[0].Stride[0]=4352;O.Dma.Bytes=4352*2340;assert(BootAdmit(&O)==CR_SUCCESS);++Checks;
}
STATIC VOID DmaTests(VOID)
{
  CR_MDSS_BOOT_OBSERVATION Base=Good(0),O;
#define BAD(EXPR) do{O=Base;EXPR;assert(BootAdmit(&O)!=CR_SUCCESS);++Checks;}while(0)
  BAD(O.Dma.ProducerGeneration=0);BAD(O.Dma.Count=0);BAD(O.Dma.Count=5);BAD(O.Dma.Reserved=1);
  BAD(O.Dma.DeviceAddress+=4);BAD(O.Dma.Bytes-=4);BAD(O.Dma.PhysicalAddress=0x9bffffff);
  BAD(O.Dma.PhysicalAddress=0x9e300000);BAD(O.Dma.PhysicalAddress=0x9e2ffffc);
  BAD(O.Dma.Streams[0].Smr&=~BIT(31));BAD(O.Dma.Streams[0].Smr|=BIT(15));
  BAD(O.Dma.Streams[0].Smr|=BIT(19));BAD(O.Dma.Streams[0].Smr^=0x10);
  BAD(O.Dma.Streams[0].Index=128);BAD(O.Dma.Streams[0].Reserved=1);
  BAD(O.Dma.Streams[0].S2cr=2U<<16);BAD(O.Dma.Streams[0].Bank=2);
  BAD(O.Dma.Streams[0].Sctlr=0);BAD(O.Dma.Streams[0].Smr&=~(0x402U<<16));
  BAD(O.Dma.Count=2;O.Dma.Streams[1]=O.Dma.Streams[0]);
  BAD(O.Dma.Count=2;O.Dma.Streams[1]=O.Dma.Streams[0];O.Dma.Streams[1].Index=4);
#undef BAD
  O=Base;O.Dma.Count=4;CONST UINT32 Sid[]={0x820,0x822,0xc20,0xc22};
  for(UINT32 I=0;I<4;++I){O.Dma.Streams[I]=Base.Dma.Streams[0];O.Dma.Streams[I].Index=(UINT16)(10+I);O.Dma.Streams[I].Smr=BIT(31)|Sid[I];}
  assert(BootAdmit(&O)==CR_SUCCESS);++Checks;
  O=Base;O.Dma.Streams[0].S2cr=BIT(16);O.Dma.Streams[0].Sctlr=0;assert(BootAdmit(&O)==CR_SUCCESS);++Checks;
}
STATIC VOID SameTests(VOID)
{
  CR_MDSS_BOOT_OBSERVATION A=Good(0),B=A;
  B.MdssPending=0x11;B.IrqStatus[0]=BootIrqAllowed[0];B.IrqStatus[2]=7;
  B.HostStatus[0]=4;B.HostInterrupt[0]=BIT(0)|BIT(8);B.Intf[1].LineCount=900;
  assert(BootSameRoute(&A,&B)==CR_SUCCESS);++Checks;
  B=A;B.Dma.ProducerGeneration++;assert(BootSameRoute(&A,&B)==CR_NOT_READY);++Checks;
  B=A;B.Dma.Streams[0].Ttbr0=0x12340000;assert(BootSameRoute(&A,&B)==CR_NOT_READY);++Checks;
  B=A;B.IrqEnable[0]^=BIT(8);assert(BootSameRoute(&A,&B)==CR_NOT_READY);++Checks;
  B=A;B.Ctl[0].Flush=1;assert(BootSameRoute(&A,&B)!=CR_SUCCESS);
  assert(BootAdmit(&B)!=CR_SUCCESS&&!B.FrameBytes&&!B.ForceMask);++Checks;
  B=Good(4);assert(BootSameRoute(&A,&B)==CR_NOT_READY);++Checks;
}
/* Frozen previous copy-normalize comparator is the differential oracle for
 * the stack-only rewrite. Android route decoding is independently tested. */
STATIC CR_STATUS CopyCompare(CONST CR_MDSS_BOOT_OBSERVATION *Old,CONST CR_MDSS_BOOT_OBSERVATION *New)
{
  CR_MDSS_BOOT_OBSERVATION A=*Old,B=*New;
  CR_STATUS S=BootAdmit(&A);if(S!=CR_SUCCESS)return S;
  S=BootAdmit(&B);if(S!=CR_SUCCESS)return S;
  A.MdssPending=B.MdssPending=0;
  memset(A.IrqStatus,0,sizeof(A.IrqStatus));memset(B.IrqStatus,0,sizeof(B.IrqStatus));
  for(UINT32 I=0;I<4;++I)A.Intf[I].LineCount=B.Intf[I].LineCount=0;
  for(UINT32 I=0;I<2;++I){A.HostStatus[I]=B.HostStatus[I]=0;
    A.HostInterrupt[I]&=0xaaaaaaaaU;B.HostInterrupt[I]&=0xaaaaaaaaU;}
  return memcmp(&A,&B,sizeof(A))?CR_NOT_READY:CR_SUCCESS;
}
STATIC VOID CompareBytes(VOID)
{
  for(UINT32 Source=0;Source<=4;Source+=4){
    CR_MDSS_BOOT_OBSERVATION A=Good(Source),B;
    for(UINTN Byte=0;Byte<sizeof(A);++Byte)for(UINT32 Bit=0;Bit<8;++Bit){
      B=A;((UINT8 *)&B)[Byte]^=(UINT8)(1U<<Bit);
      assert(BootSameRoute(&A,&B)==CopyCompare(&A,&B));++Checks;
    }
  }
}
STATIC UINT32 Faults(VOID)
{
  CR_MDSS_BOOT_OBSERVATION O=Good(0);UINT32 Calls=F.Calls;
  for(UINT32 Kind=0;Kind<2;++Kind)for(UINT32 I=1;I<=Calls;++I){
    CR_MDSS_BOOT8250 P=Seed(0);F.Fail=I;F.Failure=Kind?1:CR_DEVICE_ERROR;
    memset(&O,0xa5,sizeof(O));assert(BootObserve(&P,&O)!=CR_SUCCESS);
    assert(O.Revision==0&&BootAdmit(&O)==CR_NOT_READY);
    F.Fail=0;assert(BootObserve(&P,&O)==CR_SUCCESS&&BootAdmit(&O)==CR_SUCCESS);++Checks;
  }
  return Calls;
}
int main(void)
{
  assert(!memcmp(BootIrqOffsets,BootReferenceIrqOffsets,sizeof(BootReferenceIrqOffsets)));
  Good(0);Good(4);StageTests();Exclusions();FetchTests();DmaTests();SameTests();CompareBytes();UINT32 Calls=Faults();
  assert(BootAdmit(NULL)==CR_INVALID_PARAMETER&&BootObserve(NULL,NULL)==CR_INVALID_PARAMETER);
  printf("PASS %u checks; %u observer callback fault positions x error/warning; VIG0/DMA0 Android oracle\n",Checks,Calls);return 0;
}
