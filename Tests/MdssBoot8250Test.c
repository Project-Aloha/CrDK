/* Whole actual bootstrap owner. MMIO seed and IRQ layout are compiled from
 * the separate original-source oracle, not from production register tables.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Library/MdssBoot8250CrLib/Internal.h"
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#else
static KIRQL CurrentIrql;
KIRQL KeGetCurrentIrql(VOID){return CurrentIrql;}
#endif
extern void BootReferenceSeed(UINT32,void (*)(UINT32,UINT32,UINT32),CR_MDSS_BOOT_DMA_OBSERVATION *);
extern const UINT32 BootReferenceIrqOffsets[CR_MDSS_BOOT8250_IRQ_BANKS][3];

static const UINT32 Bytes[CrMdssBootWindows]={0x1000,0x83208,0x2008,0x400,0x7c0,0x400,0x7c0,0x214};
typedef struct {
  UINT32 R[0x88000/4];
  CR_MDSS_BOOT_DMA_OBSERVATION Dma;
  UINT32 Calls,Fail,Writes,Source,SourceXin,AckDelay,ResetDelay,DrainPending;
  UINT32 DropWindow,DropOffset,DropNth,DropCount,Generation;
  UINT32 ElapsedUs,FrameUntil,AutoRefreshReleases,SyncWrites;
  CR_STATUS Failure;
  BOOLEAN Posted,Access,Lost,Drained,PanelOff,ExternalOff;
  BOOLEAN StuckAck,UpperAck,StuckReset,StuckFlush,BusyHost;
  BOOLEAN AutoRefreshActive;
} MODEL;
static MODEL M;
static UINT32 *Reg(UINT32 W,UINT32 O){
  assert(W<CrMdssBootWindows&&!(O&3)&&O<Bytes[W]);
  UINT32 Base=0;for(UINT32 I=0;I<W;++I)Base+=Bytes[I]/4;
  assert(Base+O/4<sizeof(M.R)/sizeof(M.R[0]));return &M.R[Base+O/4];
}
static void SeedReg(UINT32 W,UINT32 O,UINT32 V){*Reg(W,O)=V;}
static CR_STATUS Step(void){return ++M.Calls==M.Fail?M.Failure:CR_SUCCESS;}
static CR_STATUS Access(VOID *C,BOOLEAN On,BOOLEAN *Owned,UINT64 *Generation){
  assert(C==&M);CR_STATUS S=Step();if(S!=CR_SUCCESS&&!M.Posted)return S;
  if(!On)assert(M.Writes==0&&!M.Drained);
  M.Access=On;*Owned=On;*Generation=On?M.Generation:0;return S;
}
static CR_STATUS Ready(VOID *C,UINT64 G,CR_MDSS_BOOT_WINDOW W){
  assert(C==&M&&(UINT32)W<CrMdssBootWindows);
  CR_STATUS S=Step();return !M.Access||M.Lost||G!=M.Generation?CR_NOT_READY:S;
}
static CR_STATUS Read(VOID *C,CR_MDSS_BOOT_WINDOW W,UINT32 O,UINT32 *V){
  assert(C==&M&&M.Access&&!M.Lost);CR_STATUS S=Step();
  if(S!=CR_SUCCESS&&!M.Posted)return S;
  if(W==CrMdssBootVbif&&O==0x204&&M.AckDelay&&!M.StuckAck){
    UINT32 Req=*Reg(W,0x200),SourceForce=M.Source?0x100:1;
    BOOLEAN Force=((*Reg(CrMdssBootDpu,0x2ac)&SourceForce)!=0)||
      ((*Reg(CrMdssBootDpu,0x2bc)&(1U<<20))!=0);
    if(Force&&!--M.AckDelay)*Reg(W,O)=M.UpperAck?Req<<16:Req;
  }
  if(W==CrMdssBootDpu&&O==0x1030&&M.ResetDelay&&!M.StuckReset){
    if(!--M.ResetDelay){*Reg(W,O)=0;if(!M.StuckFlush)*Reg(W,0x1018)=0;}
  }
  if(W==CrMdssBootHost0&&O==8&&M.BusyHost)*V=6;
  else if(W==CrMdssBootDpu&&O==0x6aab0&&M.FrameUntil)
    *V=M.ElapsedUs<M.FrameUntil?512:0;
  else if(W==CrMdssBootDpu&&O==0x34c){
    assert(*Reg(W,0x418)==0x405&&*Reg(W,0x348)==0x7001);
    *V=M.AutoRefreshActive?0x80:0;
  }
  else *V=*Reg(W,O);
  return S;
}
static CR_STATUS Write(VOID *C,CR_MDSS_BOOT_WINDOW W,UINT32 O,UINT32 V){
  assert(C==&M&&M.Access&&!M.Lost&&M.Drained);
  CR_STATUS S=Step();if(S!=CR_SUCCESS&&!M.Posted)return S;
  ++M.Writes;
  assert(W!=CrMdssBootDpu||(O!=0x4004&&O!=0x24004));
  /* SSPP bit31 is pixel extension override, never an engine enable. */
  assert(W!=CrMdssBootDpu||(O!=0x4038&&O!=0x24038));
  if(W==M.DropWindow&&O==M.DropOffset&&++M.DropCount==M.DropNth)return S;
  if(W==CrMdssBootDpu){
    if((O==0x6aa84&&!V)||(O==0x1030&&(V&1)))
      assert(!M.AutoRefreshActive&&M.ElapsedUs>=M.FrameUntil);
    if(O==0x6aa90){
      assert(M.ElapsedUs>=25000);
      ++M.SyncWrites;
      if(M.AutoRefreshReleases&&!--M.AutoRefreshReleases)M.AutoRefreshActive=FALSE;
    }
    for(UINT32 I=0;I<CR_MDSS_BOOT8250_IRQ_BANKS;++I)
      if(O==BootReferenceIrqOffsets[I][2]){*Reg(W,BootReferenceIrqOffsets[I][1])&=~V;return S;}
    if(O==0x1030&&(V&1))M.ResetDelay=3;
  }
  if(W==CrMdssBootVbif&&O==0x200)M.AckDelay=3;
  if((W==CrMdssBootHost0||W==CrMdssBootHost1)&&O==0x110){
    *Reg(W,O)=(*Reg(W,O)&0x55555555U&~V)|(V&0xaaaaaaaaU);return S;
  }
  *Reg(W,O)=V;return S;
}
static CR_STATUS Fence(VOID *C){assert(C==&M&&M.Access);return Step();}
static CR_STATUS Delay(VOID *C,UINT32 Us){
  assert(C==&M&&(Us==1||Us==2000||Us==25000));CR_STATUS S=Step();
  if(S==CR_SUCCESS||M.Posted)M.ElapsedUs+=Us;return S;
}
static CR_STATUS ObserveDma(VOID *C,UINT64 Device,UINT64 N,CR_MDSS_BOOT_DMA_OBSERVATION *D){
  assert(C==&M&&Device==M.Dma.DeviceAddress&&N==M.Dma.Bytes);
  CR_STATUS S=Step();if(S!=CR_SUCCESS&&!M.Posted)return S;*D=M.Dma;return S;
}
static CR_STATUS Drain(VOID *C){
  assert(C==&M);CR_STATUS S=Step();if(S!=CR_SUCCESS&&!M.Posted)return S;
  if(M.DrainPending){--M.DrainPending;return CR_NOT_READY;}
  M.Drained=TRUE;return S;
}
static void DmaFenced(void){
  UINT32 Mask=(1U<<M.SourceXin)|(1U<<7);
  assert((*Reg(CrMdssBootVbif,0x200)&Mask)==Mask);
  assert((*Reg(CrMdssBootVbif,0x204)&Mask)==Mask);
  assert(!*Reg(CrMdssBootDpu,0x10f4)&&!*Reg(CrMdssBootDpu,0x10e8));
  assert(!(*Reg(CrMdssBootDpu,0x6aab4)&(1U<<31)));
}
static CR_STATUS PanelOff(VOID *C){
  assert(C==&M);DmaFenced();CR_STATUS S=Step();
  if(S==CR_SUCCESS||M.Posted)M.PanelOff=TRUE;return S;
}
static CR_STATUS ExternalOff(VOID *C){
  assert(C==&M&&M.PanelOff);DmaFenced();
  assert(!(*Reg(CrMdssBootHost0,4)&7)&&!(*Reg(CrMdssBootHost0,8)&6));
  assert(!(*Reg(CrMdssBootDpu,0x700a0)&1));
  CR_STATUS S=Step();if(S==CR_SUCCESS||M.Posted)M.ExternalOff=TRUE;return S;
}
static CR_STATUS PanelReadyOff(VOID *C){assert(C==&M);CR_STATUS S=Step();return M.PanelOff?S:CR_NOT_READY;}
static CR_STATUS ExternalReadyOff(VOID *C){assert(C==&M);CR_STATUS S=Step();return M.ExternalOff?S:CR_NOT_READY;}
static void Setup(CR_MDSS_BOOT8250 *P,UINT32 Source){
  memset(&M,0,sizeof(M));memset(P,0,sizeof(*P));
  M.Generation=17;M.Source=Source;M.SourceXin=Source?1:0;
  M.Failure=CR_DEVICE_ERROR;M.DropOffset=0xffffffffU;
  BootReferenceSeed(Source,SeedReg,&M.Dma);
  CR_MDSS_BOOT_IO Io={.Context=&M,.Access=Access,.Ready=Ready,.Read32=Read,.Write32=Write,
    .Fence=Fence,.DelayUs=Delay,.ObserveDma=ObserveDma,.DrainWriters=Drain,
    .PanelResetOff=PanelOff,.PanelResetReadyOff=PanelReadyOff,
    .ExternalClocksOff=ExternalOff,.ExternalClocksReadyOff=ExternalReadyOff};
  const UINT64 Physical[]={0xae00000,0xae01000,0xaeb0000,0xae94000,0xae94400,0xae96000,0xae96400,0xaeac000};
  for(UINT32 I=0;I<CrMdssBootWindows;++I){Io.Physical[I]=Physical[I];Io.Bytes[I]=Bytes[I];}
  assert(CrMdssBoot8250Init(P,&Io)==CR_SUCCESS);
}
static void Observe(CR_MDSS_BOOT8250 *P){
  CR_STATUS S=CrMdssBoot8250Observe(P);
  if(S!=CR_SUCCESS){fprintf(stderr,"Observe returned %llx source%u\n",(unsigned long long)S,M.Source);abort();}
  assert(P->Admitted&&P->AccessOwned&&!P->Touched&&M.Writes==0);
}
static void Finish(CR_MDSS_BOOT8250 *P){
  M.Fail=0;M.Lost=M.StuckAck=M.UpperAck=M.StuckReset=M.StuckFlush=M.BusyHost=FALSE;
  M.DropOffset=0xffffffffU;M.DrainPending=0;
  M.AutoRefreshActive=FALSE;M.FrameUntil=0;
  assert(CrMdssBoot8250Retire(P)==CR_SUCCESS&&P->Retired&&P->Stage==9);
  DmaFenced();assert(M.Access&&M.ExternalOff&&M.PanelOff);
  UINT32 Writes=M.Writes;assert(CrMdssBoot8250CheckFence(P)==CR_SUCCESS&&M.Writes==Writes);
  assert(CrMdssBoot8250Abort(P)==CR_BUSY);
}
static void Queued(void){
  M.FrameUntil=6000;M.AutoRefreshActive=TRUE;M.AutoRefreshReleases=3;
  *Reg(CrMdssBootDpu,0x418)=0x12;*Reg(CrMdssBootDpu,0x348)=0x23;
}
static void AllFaults(UINT32 Source,BOOLEAN Pending){
  CR_MDSS_BOOT8250 P;Setup(&P,Source);Observe(&P);if(Pending)Queued();M.Calls=0;
  assert(CrMdssBoot8250Retire(&P)==CR_SUCCESS);
  UINT32 Count=M.Calls;
  for(UINT32 Warning=0;Warning<2;++Warning)for(UINT32 Posted=0;Posted<2;++Posted)
    for(UINT32 F=1;F<=Count;++F){
      Setup(&P,Source);Observe(&P);M.Calls=0;M.Fail=F;M.Posted=Posted;
      if(Pending)Queued();
      M.Failure=Warning?1:CR_DEVICE_ERROR;
      CR_STATUS S=CrMdssBoot8250Retire(&P);
      assert(M.Calls>=F);
      assert(S!=CR_SUCCESS&&P.AccessOwned&&P.Touched&&!P.Retired);
      Finish(&P);
    }
  Setup(&P,Source);Observe(&P);Finish(&P);M.Calls=0;
  assert(CrMdssBoot8250CheckFence(&P)==CR_SUCCESS);UINT32 Checks=M.Calls;
  for(UINT32 F=1;F<=Checks;++F){
    Setup(&P,Source);Observe(&P);Finish(&P);M.Calls=0;M.Fail=F;
    assert(CrMdssBoot8250CheckFence(&P)!=CR_SUCCESS&&!P.Retired);
    Finish(&P);
  }
  printf("source%u queued%u: whole retained retire %u/check %u callback positions PASS\n",Source,Pending,Count,Checks);
}
static void AccessFaults(void){
  CR_MDSS_BOOT8250 P;Setup(&P,0);Observe(&P);UINT32 Count=M.Calls;
  for(UINT32 Warning=0;Warning<2;++Warning)for(UINT32 Posted=0;Posted<2;++Posted)
    for(UINT32 F=1;F<=Count;++F){
      Setup(&P,0);M.Fail=F;M.Posted=Posted;M.Failure=Warning?1:CR_DEVICE_ERROR;
      assert(CrMdssBoot8250Observe(&P)!=CR_SUCCESS&&!P.Admitted&&!P.Touched&&M.Writes==0);
      M.Fail=0;Observe(&P);assert(CrMdssBoot8250Abort(&P)==CR_SUCCESS&&!M.Access);
    }
  for(UINT32 Warning=0;Warning<2;++Warning)for(UINT32 Posted=0;Posted<2;++Posted){
    Setup(&P,0);Observe(&P);M.Fail=M.Calls+1;M.Posted=Posted;M.Failure=Warning?1:CR_DEVICE_ERROR;
    assert(CrMdssBoot8250Abort(&P)!=CR_SUCCESS&&P.Initialized&&!P.Touched);
    M.Fail=0;assert(CrMdssBoot8250Abort(&P)==CR_SUCCESS&&!M.Access);
  }
  Setup(&P,0);CR_MDSS_BOOT_IO Io=P.Io;memset(&P,0,sizeof(P));P.Io=Io;
  assert(CrMdssBoot8250Init(&P,&P.Io)==CR_SUCCESS);Observe(&P);
  assert(CrMdssBoot8250Abort(&P)==CR_SUCCESS);
  printf("actual access/observation %u callback positions x4 and lease-release retries PASS\n",Count);
}
int main(void){
  CR_MDSS_BOOT8250 P;
  AccessFaults();
  for(UINT32 Source=0;Source<=4;Source+=4){
    AllFaults(Source,FALSE);AllFaults(Source,TRUE);
    Setup(&P,Source);Observe(&P);M.DrainPending=1;
    assert(CrMdssBoot8250Retire(&P)==CR_NOT_READY&&P.AccessOwned&&M.Writes==0);Finish(&P);
    Setup(&P,Source);Observe(&P);M.UpperAck=TRUE;
    assert(CrMdssBoot8250Retire(&P)==CR_TIMEOUT&&P.AccessOwned);Finish(&P);
    Setup(&P,Source);Observe(&P);M.StuckReset=TRUE;
    assert(CrMdssBoot8250Retire(&P)==CR_TIMEOUT&&P.AccessOwned);Finish(&P);
    Setup(&P,Source);Observe(&P);M.Lost=TRUE;
    assert(CrMdssBoot8250Retire(&P)!=CR_SUCCESS&&M.Writes==0&&P.AccessOwned);Finish(&P);
    Setup(&P,Source);Observe(&P);++M.Generation;
    assert(CrMdssBoot8250Retire(&P)!=CR_SUCCESS&&M.Writes==0);--M.Generation;Finish(&P);
    Setup(&P,Source);Observe(&P);M.FrameUntil=6000;M.AutoRefreshActive=TRUE;
    M.AutoRefreshReleases=3;*Reg(CrMdssBootDpu,0x418)=0x12;*Reg(CrMdssBootDpu,0x348)=0x23;
    assert(CrMdssBoot8250Retire(&P)==CR_SUCCESS&&M.SyncWrites==4);
    assert(*Reg(CrMdssBootDpu,0x418)==0x12&&*Reg(CrMdssBootDpu,0x348)==0x23);Finish(&P);
    Setup(&P,Source);Observe(&P);M.FrameUntil=100000;
    assert(CrMdssBoot8250Retire(&P)==CR_TIMEOUT&&!M.PanelOff);Finish(&P);
    Setup(&P,Source);Observe(&P);M.AutoRefreshActive=TRUE;
    assert(CrMdssBoot8250Retire(&P)==CR_TIMEOUT&&P.DebugTouched&&!M.PanelOff);Finish(&P);
    const UINT32 Drop[]={0x418,0x348,0x6aa84,0x10f4,0x10e8,0x700a0,0x81800};
    for(UINT32 I=0;I<sizeof(Drop)/sizeof(Drop[0]);++I){
      Setup(&P,Source);
      Observe(&P);M.DropWindow=CrMdssBootDpu;M.DropOffset=Drop[I];M.DropNth=1;
      assert(CrMdssBoot8250Retire(&P)!=CR_SUCCESS&&P.AccessOwned&&!P.Retired);Finish(&P);
    }
  }
  Setup(&P,0);Observe(&P);assert(CrMdssBoot8250Abort(&P)==CR_SUCCESS&&!M.Access);
#ifdef _KERNEL_MODE
  Setup(&P,0);CurrentIrql=DISPATCH_LEVEL;
  assert(CrMdssBoot8250Observe(&P)==CR_INVALID_PARAMETER&&M.Calls==0);CurrentIrql=PASSIVE_LEVEL;
  Observe(&P);CR_MDSS_BOOT8250 Saved=P;UINT32 Calls=M.Calls;CurrentIrql=DISPATCH_LEVEL;
  assert(CrMdssBoot8250Retire(&P)==CR_INVALID_PARAMETER&&!memcmp(&P,&Saved,sizeof(P))&&M.Calls==Calls);
  CurrentIrql=PASSIVE_LEVEL;Finish(&P);Saved=P;Calls=M.Calls;CurrentIrql=DISPATCH_LEVEL;
  assert(CrMdssBoot8250CheckFence(&P)==CR_INVALID_PARAMETER&&!memcmp(&P,&Saved,sizeof(P))&&M.Calls==Calls);
  CurrentIrql=PASSIVE_LEVEL;
#endif
  puts("actual boot owner: failures retain access; IRQ drain, low-field ACK, reset and incarnation checks PASS");
  return 0;
}
