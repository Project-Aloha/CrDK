/* Real read-only observer against independently source-derived DPU7 blocks.
 * The MMIO test double is necessary because host tests cannot read a board.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/Hdk8350BootDisplayCrLib.h>
#include "hdk8350-source-oracle.h"

typedef struct { UINT64 Address; UINT32 Value; } REGISTER;
typedef struct {
  REGISTER Registers[1024];
  UINT32 Count, Calls, FailAt, ChangeAt, ChangeMask;
  UINT64 ChangeAddress;
  CR_STATUS Failure;
} MODEL;
STATIC MODEL M;
STATIC UINT32 *Reg(UINT64 Address) {
  for (UINT32 I=0; I<M.Count; ++I)
    if (M.Registers[I].Address==Address) return &M.Registers[I].Value;
  assert(M.Count<1024);
  M.Registers[M.Count].Address=Address;
  return &M.Registers[M.Count++].Value;
}
STATIC CR_STATUS Read(VOID *Context, UINT64 Address, UINT32 *Value) {
  assert(Context==&M && Value && !(Address&3));
  ++M.Calls;
  if (M.Calls==M.FailAt) return M.Failure;
  *Value=*Reg(Address);
  if (Address==RefDpu+RefIntf[1]+0xac) *Value=M.Calls;
  if (M.ChangeAt && M.Calls>=M.ChangeAt && Address==M.ChangeAddress) *Value^=M.ChangeMask;
  return CR_SUCCESS;
}
STATIC VOID Setup(UINT32 Pipe) {
  memset(&M,0,sizeof(M)); M.Failure=CR_DEVICE_ERROR; M.ChangeMask=1;
  *Reg(RefMdss)=0x70000000;
  UINT64 C=RefDpu+RefCtl[0], F=RefDpu+RefSource[Pipe], I=RefDpu+RefIntf[1];
  *Reg(C+0x14)=0xf0000000; *Reg(C+0xf4)=2;
  *Reg(C)=0x1000000|(Pipe==0?1U:(1U<<18));
  *Reg(C+0xfc)=Pipe==0?(1U<<16):1;
  *Reg(RefDpu+RefLm[0]+4)=0x04380780;
  *Reg(F)=*Reg(F+0xc)=0x04380780;
  *Reg(F+0x14)=0x40002000; *Reg(F+0x24)=7680;
  *Reg(F+0x30)=0x236ff; *Reg(F+0x34)=0x03020001;
  *Reg(F+0x38)=0x80000000;
  for (UINT32 J=0; J<3; ++J) *Reg(F+0x108+J*0x10)=0x04380780;
  *Reg(I)=1; *Reg(I+0x26c)=1;
  *Reg(I+8)=0x0898002c; *Reg(I+0xc)=2475000; *Reg(I+0x14)=11000;
  *Reg(I+0x1c)=90200; *Reg(I+0x24)=2466199;
  *Reg(I+0x3c)=*Reg(I+0x64)=0x083f00c0;
  *Reg(I+0x60)=0x10; *Reg(I+0x90)=0x213f; *Reg(I+0xa8)=3;
  *Reg(RefDsi[0])=0x20050000;
  *Reg(RefDsi[0]+4)=0x1f3; *Reg(RefDsi[0]+0x10)=0x130;
  *Reg(RefDsi[0]+0x24)=0x084000c0;
  *Reg(RefDsi[0]+0x28)=0x04610029;
  *Reg(RefDsi[0]+0x2c)=0x04640897;
  *Reg(RefDsi[0]+0xac)=0x10000000;
  *Reg(RefPhy[0]+0x24)=0x7f; *Reg(RefPhy[0]+0x3c)=1;
  *Reg(RefPhy[0]+0x6b0)=1; *Reg(RefPhy[0]+0x14)=0x31;
  *Reg(RefPhy[0]+0x1c)=1; *Reg(RefPhy[0]+0xa0)=0x1f;
  *Reg(RefPhy[0]+0x34)=0x21; *Reg(RefPhy[0]+0x38)=0x84;
  *Reg(RefPhy[0]+0xa4)=0x60; *Reg(RefPhy[0]+0x140)=1;
  *Reg(0xaf03000)=0x80000000;
  const UINT64 Gates[]={0x127004,0x12701c,0x12700c,0x127014,
    0xaf0207c,0xaf0200c,0xaf0201c,0xaf02024,0xaf02028,0xaf0202c,0xaf02004};
  for (UINT32 J=0; J<sizeof(Gates)/sizeof(Gates[0]); ++J) *Reg(Gates[J])=1;
  *Reg(0xaf02110)=0x101; *Reg(0xaf02098)=0x101; *Reg(0xaf02124)=1;
}
STATIC CR_HDK8350_DISPLAY_OBSERVATION Inventory(VOID) {
  CR_HDK8350_DISPLAY_OBSERVATION O;
  CR_HDK8350_DISPLAY_IO Io={&M,Read};
  UINT32 Before=M.Calls;
  CR_STATUS S=CrHdk8350DisplayReadInventory(&Io,&O);
  if (S!=CR_SUCCESS) fprintf(stderr,"inventory unexpectedly returned %llx\n",(unsigned long long)S);
  assert(S==CR_SUCCESS);
  assert(O.Revision==CR_HDK8350_DISPLAY_REVISION && O.Stable);
  assert(O.Raw.Phy[0].LaneMap[0]==0x21&&O.Raw.Phy[0].LaneMap[1]==0x84);
  assert(O.Reads==M.Calls-Before && O.Reads<=CR_HDK8350_DISPLAY_MAX_READS);
  return O;
}
STATIC VOID NoDerived(CONST CR_HDK8350_DISPLAY_OBSERVATION *O) {
  assert(!O->Width&&!O->Height&&!O->Stride&&!O->SourceOffset&&!O->Device&&!O->Bytes);
  assert(!O->Format&&!O->Unpack&&!O->RedMask&&!O->GreenMask&&!O->BlueMask&&!O->ReservedMask);
}
STATIC VOID Rejected(CR_HDK8350_DISPLAY_OBSERVATION *O) {
  O->Width=1; O->Device=1; O->RedMask=1;
  assert(CrHdk8350DisplayAdmitPreserved(O)!=CR_SUCCESS); NoDerived(O);
}
int main(void) {
  for (UINT32 P=0; P<2; ++P) {
    UINT32 Pipe=P?4:0; Setup(Pipe);
    CR_HDK8350_DISPLAY_OBSERVATION O=Inventory();
    assert(CrHdk8350DisplayAdmitPreserved(&O)==CR_SUCCESS);
    assert(O.Width==1920&&O.Height==1080&&O.Stride==7680&&O.Device==0x40002000);
    assert(O.Bytes==8294400&&O.SourceOffset==(P?0x24000U:0x4000U));
    assert(O.Format==0x236ff&&O.Unpack==0x03020001);
    assert(O.RedMask==0xff0000&&O.GreenMask==0xff00&&O.BlueMask==0xff&&O.ReservedMask==0xff000000);
  }
  Setup(4); CR_HDK8350_DISPLAY_OBSERVATION Good=Inventory(), O;
#define BAD(member,value) do { O=Good; O.member=(value); Rejected(&O); } while(0)
  BAD(Revision,0); BAD(Stable,FALSE); BAD(Raw.DpuVersion,0x81000000);
  BAD(Raw.Ctl[0].Top,0xf0020000); BAD(Raw.Ctl[0].Intf,6);
  BAD(Raw.Ctl[0].Fetch,3); BAD(Raw.Ctl[0].Dsc,1); BAD(Raw.Ctl[0].Wb,1);
  BAD(Raw.Ctl[0].Cwb,1); BAD(Raw.Ctl[0].Cdm,1); BAD(Raw.Ctl[0].Merge,1);
  BAD(Raw.Ctl[0].IntfMaster,1); BAD(Raw.Ctl[0].Layer[0][0],1|(1U<<18));
  BAD(Raw.Ctl[0].Layer[0][0],2U<<18); BAD(Raw.Ctl[0].Layer[0][2],1U<<8);
  BAD(Raw.Ctl[0].Layer[1][0],1); BAD(Raw.Ctl[1].Intf,2);
  BAD(Raw.Ctl[0].Flush,1); BAD(Raw.Ctl[0].Reset,1);
  BAD(Raw.SplitEnable,1); BAD(Raw.PpbControl[0],1); BAD(Raw.DceSelect,1);
  BAD(Raw.Source[4].Format,0x800236ff); BAD(Raw.Source[4].Format,0x236fe);
  BAD(Raw.Source[4].Unpack,0x03020000); BAD(Raw.Source[4].Unpack,0x04020001);
  BAD(Raw.Source[4].OpMode,0x80002000); BAD(Raw.Source[4].Secure,1);
  BAD(Raw.Source[4].MultiRect,1); BAD(Raw.Source[4].Decimation,1);
  BAD(Raw.Source[4].Exclusion,1); BAD(Raw.Source[4].SourceXy,1);
  BAD(Raw.Source[4].OutputXy,1); BAD(Raw.Source[4].SourceSize,0x04380781);
  BAD(Raw.Source[4].Stride[0],7676); BAD(Raw.Source[4].Stride[0],7681);
  BAD(Raw.Source[4].Stride[0],65536); BAD(Raw.Source[4].Stride[1],1);
  BAD(Raw.Source[4].Address[0],0); BAD(Raw.Source[4].Address[0],0xfffffffc);
  BAD(Raw.Source[4].Address[0],0x40002001); BAD(Raw.Source[4].Address[1],1);
  BAD(Raw.Source[4].PixelExtension[2],1); BAD(Raw.LmOutput[0],1);
  BAD(Raw.LmMode[0],0x80000000); BAD(Raw.Intf[1].Enable,0);
  BAD(Raw.Intf[1].Status,0); BAD(Raw.Intf[2].Enable,1);
  BAD(Raw.Intf[1].Config2,0x110); BAD(Raw.Intf[1].TearEnable,1);
  BAD(Raw.Intf[1].AutoRefresh,0x80000000); BAD(Raw.Intf[1].CommandTrigger,1);
  BAD(Raw.Intf[1].TestEnable,1); BAD(Raw.Intf[1].HDisplay,1);
  BAD(Raw.Intf[1].VPeriod,1); BAD(Raw.Intf[1].DataHDisplay,1);
  BAD(Raw.Host[0].Control,0x1f7); BAD(Raw.Host[0].Version,0x20040000);
  BAD(Raw.Host[1].Control,1); BAD(Raw.Host[0].Total,1);
  BAD(Raw.Host[1].Status,8); BAD(Raw.Host[0].Status,0x80000000);
  BAD(Raw.Host[0].Split,1); BAD(Raw.Host[0].Cphy,1);
  BAD(Raw.Host[0].LaneSwap,1); BAD(Raw.Host[0].LaneControl,0x11000000);
  BAD(Raw.Phy[0].Global,0x40); BAD(Raw.Phy[0].LaneMap[0],0x12);
  BAD(Raw.Phy[0].Pll,0); BAD(Raw.Phy[0].PllStatus,0);
  BAD(Raw.Phy[0].Lanes,0xf); BAD(Raw.Phy[1].Control,1);
  BAD(Raw.DispGdsc,0); BAD(Raw.GccDisplay[0],0); BAD(Raw.DispClock[0],0);
  BAD(Raw.DispClock[4],0x80000001);
  BAD(Raw.DsiClockConfig[0],1); BAD(Raw.DsiClockConfig[1],0x201);
  BAD(Raw.DsiClockCommand[0],1);
#undef BAD
  O=Good; O.Raw.Source[4].Stride[0]=8192;
  assert(CrHdk8350DisplayAdmitPreserved(&O)==CR_SUCCESS&&O.Bytes==8847360);
  O=Good; O.Raw.Source[4].Format|=1U<<8;
  Rejected(&O);
  O=Good;
  O.Raw.Source[4].Unpack=0x03000102;
  assert(CrHdk8350DisplayAdmitPreserved(&O)==CR_SUCCESS&&O.RedMask==0xff&&O.BlueMask==0xff00&&O.GreenMask==0xff0000);
  Setup(0); O=Inventory(); O.Raw.Source[0].Scaler=1; Rejected(&O);
  O=Inventory(); O.Raw.Source[0].Csc=1; Rejected(&O);
  Setup(4); UINT32 Calls=Inventory().Reads;
  for (UINT32 I=1; I<=Calls; ++I) {
    Setup(4); M.FailAt=I; memset(&O,0xa5,sizeof(O));
    CR_HDK8350_DISPLAY_IO Io={&M,Read};
    assert(CrHdk8350DisplayReadInventory(&Io,&O)==CR_DEVICE_ERROR);
    assert(M.Calls==I&&O.Reads==I&&!O.Revision&&!O.Stable); NoDerived(&O);
  }
  Setup(4); M.FailAt=1; M.Failure=1;
  CR_HDK8350_DISPLAY_IO Io={&M,Read};
  assert(CrHdk8350DisplayReadInventory(&Io,&O)==CR_NOT_READY);
  Setup(4); M.ChangeAt=Calls/2+1; M.ChangeAddress=RefDpu+RefSource[4]+0x14;
  assert(CrHdk8350DisplayReadInventory(&Io,&O)==CR_SUCCESS&&!O.Stable);
  Rejected(&O);
  /* A command engine becoming busy in the second sweep is not video-only.
   * Video-engine progress status may change without invalidating the route. */
  Setup(4); M.ChangeAt=Calls/2+1; M.ChangeAddress=RefDsi[0]+8;
  assert(CrHdk8350DisplayReadInventory(&Io,&O)==CR_SUCCESS&&!O.Stable);
  Rejected(&O);
  Setup(4); M.ChangeAt=Calls/2+1; M.ChangeAddress=RefDsi[1]+8; M.ChangeMask=8;
  assert(CrHdk8350DisplayReadInventory(&Io,&O)==CR_SUCCESS&&!O.Stable);
  Rejected(&O);
  Setup(4); M.ChangeAt=Calls/2+1; M.ChangeAddress=RefDsi[0]+8; M.ChangeMask=8;
  assert(CrHdk8350DisplayReadInventory(&Io,&O)==CR_SUCCESS&&O.Stable);
  assert(CrHdk8350DisplayAdmitPreserved(&O)==CR_SUCCESS);
  assert(CrHdk8350DisplayReadInventory(NULL,&O)==CR_INVALID_PARAMETER);
  assert(CrHdk8350DisplayAdmitPreserved(NULL)==CR_INVALID_PARAMETER);
  printf("PASS: preserved VIG0/DMA0, route and format rejection, every %u read failure, stable two-sweep inventory\n",Calls);
  return 0;
}
