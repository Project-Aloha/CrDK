/* Actual observer against the independently source-qualified Android seed.
 * Missing/changing route, pixel packing and disabled refresh must not become
 * a writable preserving GOP admission. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/MdssBoot8250CrLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#else
KIRQL KeGetCurrentIrql(VOID){return PASSIVE_LEVEL;}
#endif
/* These declarations also allow a test-first link against the old API. */
extern CR_STATUS CrMdssBoot8250ReadInventory(VOID *,
  CR_STATUS (*)(VOID *,CR_MDSS_BOOT_WINDOW,UINT32,UINT32 *),CR_MDSS_BOOT_OBSERVATION *);
extern CR_STATUS CrMdssBoot8250ProbePreservedRoute(CR_MDSS_BOOT_OBSERVATION *);
extern VOID BootReferenceSeed(UINT32,VOID (*)(UINT32,UINT32,UINT32),CR_MDSS_BOOT_DMA_OBSERVATION *);
STATIC CONST UINT32 Sizes[]={0x1000,0x83208,0x2008,0x400,0x7c0,0x400,0x7c0,0x214};
STATIC UINT32 Registers[0x88000/4],Reads,FailRead,ChangeRead,DynamicRead;
STATIC CR_STATUS Failure;
STATIC UINT32 *Reg(UINT32 W,UINT32 O){
  assert(W<CrMdssBootWindows&&!(O&3)&&O<Sizes[W]);
  UINT32 Base=0;for(UINT32 I=0;I<W;++I)Base+=Sizes[I]/4;
  return &Registers[Base+O/4];
}
STATIC VOID SeedReg(UINT32 W,UINT32 O,UINT32 V){*Reg(W,O)=V;}
STATIC CR_STATUS Read(VOID *C,CR_MDSS_BOOT_WINDOW W,UINT32 O,UINT32 *V){
  assert(C==Registers);++Reads;
  if(Reads==FailRead)return Failure;
  if(Reads==ChangeRead)*Reg(CrMdssBootDpu,0x24014)+=0x1000;
  if(Reads==DynamicRead){
    *Reg(CrMdssBootDpu,0x14)=0x100;
    *Reg(CrMdssBootDpu,0x6aab0)=123;
  }
  *V=*Reg(W,O);return CR_SUCCESS;
}
STATIC VOID Seed(UINT32 Source){
  CR_MDSS_BOOT_DMA_OBSERVATION Unused;
  memset(Registers,0,sizeof(Registers));Reads=FailRead=ChangeRead=DynamicRead=0;
  Failure=CR_DEVICE_ERROR;BootReferenceSeed(Source,SeedReg,&Unused);
  *Reg(CrMdssBootDpu,0x6aab4)=0x80000001U;
}
STATIC CR_MDSS_BOOT_OBSERVATION Capture(UINT32 Source){
  CR_MDSS_BOOT_OBSERVATION O;Seed(Source);
  assert(CrMdssBoot8250ReadInventory(Registers,Read,&O)==CR_SUCCESS);
  assert(CrMdssBoot8250ProbePreservedRoute(&O)==CR_SUCCESS);
  assert(O.Width==1080&&O.Height==2340&&O.Stride==4320);
  assert(O.FrameDevice!=0&&O.FrameBytes==10108800ULL);
  assert(!O.Dma.ProducerGeneration&&!O.Dma.Count&&!O.Dma.PhysicalAddress);
  assert(O.FrameUnpack==O.Sspp[Source].Unpack);
  assert(O.PixelRedMask==0x00ff0000U&&O.PixelGreenMask==0x0000ff00U&&
         O.PixelBlueMask==0x000000ffU&&O.PixelReservedMask==0xff000000U);
  return O;
}
STATIC VOID Reject(CR_MDSS_BOOT_OBSERVATION *O,CR_STATUS Want){
  O->Width=O->Height=O->Stride=1;O->FrameDevice=O->FrameBytes=1;
  O->FrameUnpack=1;
  assert(CrMdssBoot8250ProbePreservedRoute(O)==Want);
  assert(!O->Width&&!O->Height&&!O->Stride&&!O->FrameDevice&&!O->FrameBytes&&!O->FrameUnpack);
}
int main(VOID){
  CR_MDSS_BOOT_OBSERVATION O;
  for(UINT32 Source=0;Source<=4;Source+=4){
    O=Capture(Source);O.Intf[1].AutoRefresh=0;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Intf[1].AutoRefresh=0x80000000U;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Intf[1].AutoRefresh=0x80010001U;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Ctl[0].Intf=0;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Sspp[Source].Format|=1U<<30;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Sspp[Source].Format|=1U<<8;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Ctl[0].Layer[0][0]=Source?2U<<18:2U;
    Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Sspp[Source].Unpack=0x03020000U;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Sspp[Source].Unpack=0x03020004U;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Ctl[0].Flush=1;Reject(&O,CR_NOT_READY);
    O=Capture(Source);O.HostControl[1]=1;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.RegDmaMode=1;Reject(&O,CR_UNSUPPORTED);
    O=Capture(Source);O.Revision=0;Reject(&O,CR_NOT_READY);
    O=Capture(Source);O.Sspp[Source].Unpack=0x03020100U;
    assert(CrMdssBoot8250ProbePreservedRoute(&O)==CR_SUCCESS&&O.FrameUnpack==0x03020100U);
    assert(O.PixelRedMask==0x00ff0000U&&O.PixelGreenMask==0xffU&&O.PixelBlueMask==0xff00U);
  }
  O=Capture(4);UINT32 Count=Reads;assert(Count>100);
  for(UINT32 I=1;I<=Count;++I){
    Seed(4);FailRead=I;memset(&O,0xa5,sizeof(O));
    assert(CrMdssBoot8250ReadInventory(Registers,Read,&O)==CR_DEVICE_ERROR);
    CR_MDSS_BOOT_OBSERVATION Zero={0};assert(!memcmp(&O,&Zero,sizeof(O)));
  }
  Seed(4);FailRead=1;Failure=1;
  assert(CrMdssBoot8250ReadInventory(Registers,Read,&O)==CR_NOT_READY);
  Seed(4);ChangeRead=Count/2+1;
  assert(CrMdssBoot8250ReadInventory(Registers,Read,&O)==CR_NOT_READY);
  assert(!O.Revision&&!O.Width&&!O.FrameDevice);
  Seed(4);DynamicRead=Count/2+1;
  assert(CrMdssBoot8250ReadInventory(Registers,Read,&O)==CR_SUCCESS);
  assert(O.IrqStatus[0]==0x100&&O.Intf[1].LineCount==123);
  assert(CrMdssBoot8250ProbePreservedRoute(&O)==CR_SUCCESS);
  assert(CrMdssBoot8250ReadInventory(Registers,NULL,&O)==CR_INVALID_PARAMETER);
  assert(CrMdssBoot8250ReadInventory(Registers,Read,NULL)==CR_INVALID_PARAMETER);
  assert(CrMdssBoot8250ProbePreservedRoute(NULL)==CR_INVALID_PARAMETER);
  puts("PASS: pure preserved DSI0 inventory rejects inactive refresh, bad packing, route changes and failed reads; no DMA proof is fabricated");
  return 0;
}
