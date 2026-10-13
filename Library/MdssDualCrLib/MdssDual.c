/* Linux DPU8.1 dpu_hw_{sspp,ctl,lm,intf,top,pingpong}/dpu_encoder_phys_vid
 * and Android sde_hw_dsc_1_2 routing. SPDX-License-Identifier: GPL-2.0-only */
#include <Library/MdssDualCrLib.h>
#include "../MdssDsiCrLib/MdssDsiInternal.h"
#include <oskal/cr_string.h>
#define CTL 0x15000U
#define UP(v,d) (((v)+(d)-1)/(d))
static UINT32 Intf(UINT32 C){return 0x35000U+C*0x1000U;}
static CR_STATUS Dr(CR_MDSS_DPU_EXT *D,UINT32 O,UINT32 *V){return XRead(D->Dsi,CrMdssExtDpu,O,V);}
static CR_STATUS Dw(CR_MDSS_DPU_EXT *D,UINT32 O,UINT32 V){return XWrite(D->Dsi,CrMdssExtDpu,O,V);}
static CR_STATUS Df(CR_MDSS_DPU_EXT *D){return XFence(D->Dsi,CrMdssExtDpu);}
static CR_STATUS Dc(CR_MDSS_DPU_EXT *D,UINT32 O,UINT32 V,UINT32 M){return XChecked(D->Dsi,CrMdssExtDpu,O,V,M);}
static CR_STATUS Du(CR_MDSS_DPU_EXT *D,UINT32 O,UINT32 M,UINT32 V){return XUpdate(D->Dsi,CrMdssExtDpu,O,M,V);}
static CR_STATUS DsReady(VOID *C){CR_MDSS_DPU_EXT *D=C;return XReady(D->Dsi,CrMdssExtDpu);}
static CR_STATUS DsRead(VOID *C,UINT32 O,UINT32 *V){return Dr(C,O,V);}
static CR_STATUS DsWrite(VOID *C,UINT32 O,UINT32 V){return Dw(C,O,V);}
static CR_STATUS DsFence(VOID *C){return Df(C);}
CR_STATUS CrMdssDpuExtendedInit(CR_MDSS_DPU_EXT *D,CR_MDSS_DSI *S,UINT64 Iova,UINTN Bytes)
{
 UINT32 I;CR_STATUS Status;CR_MDSS_DSC_ROUTE R;CR_MDSS_DSC_IO Io;
 UINT64 Size;
 if(!D||!S||!S->Initialized||!Iova||(Iova&3))return CR_INVALID_PARAMETER;
 Size=(UINT64)S->Plan.Mode.Raster.Stride*S->Plan.Mode.Raster.Height;
 if(Size>Bytes||Iova>CR_MAX_UINT32||Size-1>CR_MAX_UINT32-Iova)return CR_BAD_BUFFER_SIZE;
 if(S->Io.Physical[CrMdssExtDpu]!=0xae01000ULL)
  return CR_UNSUPPORTED;
 if(S->Io.Bytes[CrMdssExtDpu]<0x82000||
    S->Io.Bytes[CrMdssExtDpu]>CR_MAX_UINT64-S->Io.Physical[CrMdssExtDpu])
  return CR_BAD_BUFFER_SIZE;
 if(S->Plan.Link[0].Width>2560)return CR_UNSUPPORTED; /* SM8450 LM max width */
 if(S->Plan.Mode.Dsc&&S->Plan.Mode.Compression.SliceCount/S->Plan.Mode.Controllers>2)
  return CR_UNSUPPORTED;
 cr_memset(D,0,sizeof(*D));D->Dsi=S;D->DeviceAddress=Iova;D->BufferBytes=Bytes;
 Io=(CR_MDSS_DSC_IO){D,S->Io.Bytes[CrMdssExtDpu],DsReady,DsRead,DsWrite,DsFence};
 if(S->Plan.Mode.Dsc)for(I=0;I<S->Plan.Mode.Controllers;++I){
  UINT32 Soft=S->Plan.Mode.Compression.SliceCount/S->Plan.Mode.Controllers;
  UINT32 Delay=S->Plan.Mode.Compression.Params.BitsPerComponent==8?84:92;
  UINT32 Total=Delay*3+S->Plan.Mode.Compression.InitialTransmitDelay+47;
  if(Soft>1)Total+=Delay*3;
  UINT32 Lines=UP(Total,S->Plan.Mode.Compression.Params.SliceWidth);
  R=(CR_MDSS_DSC_ROUTE){.Encoder=I,.Pingpong=I,.EncoderCount=S->Plan.Mode.Controllers,
   .ActiveSoftSlices=Soft,.InitialLines=Lines,.SplitPanel=S->Plan.Mode.Controllers>1,.Video=TRUE};
  Status=CrMdssDscInit(&D->Dsc[I],&Io,&S->Plan.Mode.Compression,&R);
  if(Status!=CR_SUCCESS){cr_memset(D,0,sizeof(*D));return Status;}
 }
 D->Initialized=TRUE;return CR_SUCCESS;
}
static CR_STATUS ProgramPipe(CR_MDSS_DPU_EXT *D,UINT32 I)
{
 CONST CR_MDSS_DSI_PLAN *P=&D->Dsi->Plan;CONST CR_MDSS_MODE *M=&P->Link[I];
 UINT32 S=0x24000+I*0x2000,L=0x44000+I*0x1000,J;CR_STATUS Status;
#define PUT(o,v) do{Status=Dw(D,(o),(v));if(Status!=CR_SUCCESS)return Status;}while(0)
 /* Each pipe reads a source rectangle from the same full-stride mapping. */
 PUT(S,P->Mode.Raster.Width|(M->Height<<16));PUT(S+8,I*M->Width);
 PUT(S+0xc,M->Width|(M->Height<<16));PUT(S+0x10,0);
 PUT(S+0x14,(UINT32)D->DeviceAddress);PUT(S+0x18,0);PUT(S+0x1c,0);PUT(S+0x20,0);
 PUT(S+0x24,P->Mode.Raster.Stride);PUT(S+0x30,0x000236ff);PUT(S+0x34,0x03020001);
 PUT(S+0x38,XB(31));PUT(S+0x170,0);
 Status=Du(D,S+0x70,15,0);if(Status!=CR_SUCCESS)return Status;
 for(J=0;J<3;++J){PUT(S+0x100+J*0x10,0);PUT(S+0x104+J*0x10,0);
  PUT(S+0x108+J*0x10,M->Width|(M->Height<<16));}
 PUT(S+0x60,0x3ffff);PUT(S+0x64,0xfe00);PUT(S+0x74,0x22335777);
 PUT(S+0x78,0x00112222);PUT(S+0x6c,1);
 Status=Du(D,I?0x2b4:0x2ac,XB(8),XB(8));if(Status!=CR_SUCCESS)return Status;
 /* Opaque BGRX uses BASE; preserve only the source-defined bit30. */
 UINT32 Op;Status=Dr(D,L,&Op);if(Status!=CR_SUCCESS)return Status;
 PUT(L,(Op&XB(30))|1|(I?XB(31):0));PUT(L+4,M->Width|(M->Height<<16));
 PUT(CTL+I*4,XB(24)|(1U<<(I?21:18)));
 PUT(CTL+0x40+I*4,0);PUT(CTL+0x70+I*4,0);PUT(CTL+0xa0+I*4,0);
#undef PUT
 return CR_SUCCESS;
}
static CR_STATUS ProgramIntf(CR_MDSS_DPU_EXT *D,UINT32 I)
{
 CONST CR_MDSS_DSI_PLAN *P=&D->Dsi->Plan;CR_MDSS_MODE M=P->Link[I];
 UINT32 Base=Intf(M.Controller),H,V,Start,Window,Fetch,Lines,Config2;CR_STATUS S;
 M.Width=P->PayloadWidth;H=M.Width+M.HSync+M.HBackPorch+M.HFrontPorch;
 V=M.Height+M.VSync+M.VBackPorch+M.VFrontPorch;Start=M.HSync+M.HBackPorch;
 Window=((Start+M.Width-1)<<16)|Start;
 Lines=M.VSync+M.VBackPorch;Lines=Lines>=24?0:24-Lines;
 if(Lines>M.VFrontPorch)Lines=M.VFrontPorch;
 Fetch=Lines?(V-Lines)*H+1:0;Config2=P->Mode.Dsc?XB(12):XB(4);
#define PUT(o,v) do{S=Dw(D,Base+(o),(v));if(S!=CR_SUCCESS)return S;}while(0)
 PUT(8,(H<<16)|M.HSync);PUT(0xc,V*H);PUT(0x14,M.VSync*H);
 PUT(0x1c,(M.VSync+M.VBackPorch)*H);PUT(0x24,(V-M.VFrontPorch)*H-1);
 PUT(0x2c,0);PUT(0x34,0);PUT(0x3c,Window);PUT(0x40,0);
 PUT(0x44,M.BorderColor);PUT(0x48,0xff);PUT(0x4c,0);
 PUT(0x50,0); /* Qualcomm DSI cannot handle active-low sync signals. */
 PUT(0x54,0);PUT(0x100,0);PUT(0x90,0x213f);PUT(0xa8,3);
 S=Du(D,Base+0x25c,15,I);if(S!=CR_SUCCESS)return S;
 PUT(0x60,Config2);PUT(0x64,Window);PUT(0x68,0);PUT(0x170,Fetch);
 PUT(4,Fetch?XB(31):0);
#undef PUT
 return CR_SUCCESS;
}
CR_STATUS CrMdssDpuExtendedPrepare(CR_MDSS_DPU_EXT *D)
{
 UINT32 I,V,Mask=0;CR_STATUS S;CONST CR_MDSS_DSI_PLAN *P;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 if(D->Prepared&&!D->CleanupPending)return XReady(D->Dsi,CrMdssExtDpu);
 if(D->Touched||D->CleanupPending||!D->Dsi->Prepared||D->Dsi->CleanupPending)return CR_NOT_READY;
 P=&D->Dsi->Plan;
 for(I=0;I<P->Mode.Controllers;++I){
  S=Dr(D,Intf(P->Link[I].Controller),&V);if(S!=CR_SUCCESS)return S;if(V&1)return CR_BUSY;
  S=Dr(D,Intf(P->Link[I].Controller)+0x26c,&V);if(S!=CR_SUCCESS)return S;if(V&1)return CR_BUSY;
  Mask|=XB(P->Link[I].Controller+1);
 }
 D->Touched=TRUE;D->CleanupPending=TRUE;
 S=Dw(D,CTL+0x14,0xf0000000);if(S!=CR_SUCCESS)return S;
 for(I=0;I<P->Mode.Controllers;++I){
  S=ProgramPipe(D,I);if(S!=CR_SUCCESS)return S;S=ProgramIntf(D,I);if(S!=CR_SUCCESS)return S;
  if(P->Mode.Dsc){S=CrMdssDscStart(&D->Dsc[I]);if(S!=CR_SUCCESS)return S;
   S=Du(D,0x69000+I*0x1000+0xc8,XB(18),XB(18));if(S!=CR_SUCCESS)return S;
  }
  S=Dc(D,0x69000+I*0x1000+0xa0,P->Mode.Dsc?1:0,1);if(S!=CR_SUCCESS)return S;
 }
#define PUT(o,v) do{S=Dw(D,(o),(v));if(S!=CR_SUCCESS)return S;}while(0)
 PUT(CTL+0xe4,0);PUT(CTL+0xe8,P->Mode.Dsc?(1U<<P->Mode.Controllers)-1:0);
 PUT(CTL+0xec,0);PUT(CTL+0xf0,0);PUT(CTL+0xf4,Mask);PUT(CTL+0xf8,0);
 PUT(CTL+0xfc,(1U<<P->Mode.Controllers)-1);PUT(CTL+0x134,P->Mode.Controllers==2?XB(1):0);
 PUT(0x28,P->Mode.Controllers==2?1:0);PUT(0x3f0,P->Mode.Controllers==2?XB(8):0);
 PUT(0x2f8,P->Mode.Controllers==2?XB(4):0);
 S=Dc(D,0x2f4,P->Mode.Controllers==2?1:0,1);if(S!=CR_SUCCESS)return S;
 PUT(CTL+0x110,Mask);PUT(CTL+0x104,P->Mode.Dsc?(1U<<P->Mode.Controllers)-1:0);
 PUT(CTL+0x18,XB(11)|XB(6)|XB(17)|XB(31)|(P->Mode.Controllers==2?(XB(12)|XB(7)):0)|
     (P->Mode.Dsc?XB(22):0));
#undef PUT
 S=Df(D);if(S!=CR_SUCCESS)return S;D->Prepared=TRUE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDpuExtendedEnable(CR_MDSS_DPU_EXT *D)
{
 UINT32 I,E,First[2]={0},Seen=0;CR_STATUS S;CONST CR_MDSS_DSI_PLAN *P;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 if(!D->Prepared||D->CleanupPending||!D->Dsi->Active||D->Dsi->CleanupPending)return CR_NOT_READY;
 if(D->Active)return XReady(D->Dsi,CrMdssExtDpu);
 P=&D->Dsi->Plan;D->CleanupPending=TRUE;
 S=Dw(D,CTL+0x1c,1);if(S!=CR_SUCCESS)return S;
 /* Arm the slave timing engine before releasing the source-selected master. */
 for(I=P->Mode.Controllers;I>0;--I){S=Dw(D,Intf(P->Link[I-1].Controller),1);if(S!=CR_SUCCESS)return S;}
 S=Df(D);if(S!=CR_SUCCESS)return S;
 for(E=0;E<=250000;E+=50){
  UINT32 Running=0,V,Frame,Flush;
  for(I=0;I<P->Mode.Controllers;++I){
   S=Dr(D,Intf(P->Link[I].Controller)+0x26c,&V);if(S!=CR_SUCCESS)return S;
   S=Dr(D,Intf(P->Link[I].Controller)+0xac,&Frame);if(S!=CR_SUCCESS)return S;
   if(!(V&1)){Seen&=~XB(I);continue;}
   if(!(Seen&XB(I))){First[I]=Frame;Seen|=XB(I);}else if(Frame!=First[I])Running|=XB(I);
  }
  S=Dr(D,CTL+0x18,&Flush);if(S!=CR_SUCCESS)return S;
  if(Running==(1U<<P->Mode.Controllers)-1&&!Flush){D->Active=TRUE;D->CleanupPending=FALSE;return CR_SUCCESS;}
  if(E==250000)break;S=XDelay(D->Dsi,CrMdssExtDpu,50);if(S!=CR_SUCCESS)return S;
 }
 return CR_TIMEOUT;
}
CR_STATUS CrMdssDpuExtendedStop(CR_MDSS_DPU_EXT *D)
{
 UINT32 I;CR_STATUS S;CONST CR_MDSS_DSI_PLAN *P;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;if(!D->Touched)return CR_SUCCESS;
 P=&D->Dsi->Plan;D->CleanupPending=TRUE;
 for(I=P->Mode.Controllers;I>0;--I){S=Dw(D,Intf(P->Link[I-1].Controller),0);if(S!=CR_SUCCESS)return S;}
 S=Df(D);if(S!=CR_SUCCESS)return S;
 for(I=0;I<P->Mode.Controllers;++I){
  S=XPoll(D->Dsi,CrMdssExtDpu,Intf(P->Link[I].Controller)+0x26c,1,0,100000);if(S!=CR_SUCCESS)return S;
 }
 S=Dw(D,CTL+0x30,1);if(S!=CR_SUCCESS)return S;S=Df(D);if(S!=CR_SUCCESS)return S;
 S=XPoll(D->Dsi,CrMdssExtDpu,CTL+0x30,1,0,2000);if(S!=CR_SUCCESS)return S;
 for(I=P->Mode.Controllers;I>0;--I){
  if(P->Mode.Dsc){S=CrMdssDscStop(&D->Dsc[I-1]);if(S!=CR_SUCCESS)return S;}
  S=Dc(D,0x69000+(I-1)*0x1000+0xa0,0,1);if(S!=CR_SUCCESS)return S;
 }
 S=Dc(D,0x2f4,0,1);if(S!=CR_SUCCESS)return S;
 D->Touched=FALSE;D->Prepared=FALSE;D->Active=FALSE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDpuExtendedUninit(CR_MDSS_DPU_EXT *D)
{
 CR_STATUS S;if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 S=CrMdssDpuExtendedStop(D);if(S!=CR_SUCCESS)return S;cr_memset(D,0,sizeof(*D));return CR_SUCCESS;
}
CR_STATUS CrMdssExtendedInit(CR_MDSS_EXT_DISPLAY *D,CONST CR_MDSS_EXT_IO *Io,
 CONST CR_MDSS_EXT_PANEL *Panel,CONST CR_MDSS_DSI_MODE *M,VOID *Cpu,UINT64 Iova,UINTN Bytes,
 CONST CR_MDSS_DSI_COMMAND_MEMORY *Commands)
{
 CR_STATUS S;
 if(!D||!Panel||!Panel->Prepare||!Panel->Unprepare||!Panel->PublishPixels||!Cpu||
    Bytes>(UINTN)-1-(UINTN)Cpu||!M||(M->Dsc&&!Commands))return CR_INVALID_PARAMETER;
 cr_memset(D,0,sizeof(*D));S=CrMdssDsiInit(&D->Dsi,Io,M);if(S!=CR_SUCCESS)return S;
 if(Commands){S=CrMdssDsiSetCommandMemory(&D->Dsi,Commands);if(S!=CR_SUCCESS)goto Failed;}
 S=CrMdssDpuExtendedInit(&D->Dpu,&D->Dsi,Iova,Bytes);if(S!=CR_SUCCESS)goto Failed;
 D->Panel=*Panel;D->Cpu=Cpu;D->Bytes=(UINTN)M->Raster.Stride*M->Raster.Height;D->Initialized=TRUE;return CR_SUCCESS;
Failed:cr_memset(D,0,sizeof(*D));return S;
}
CR_STATUS CrMdssExtendedStart(CR_MDSS_EXT_DISPLAY *D)
{
 CR_STATUS S;BOOLEAN Owned=FALSE;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 if(D->Active&&!D->CleanupPending){
  S=CrMdssDsiEnable(&D->Dsi);if(S!=CR_SUCCESS)return S;
  return CrMdssDpuExtendedEnable(&D->Dpu);
 }
 if(D->CleanupPending)return CR_NOT_READY;D->CleanupPending=TRUE;
 S=CrMdssDsiPrepare(&D->Dsi);if(S!=CR_SUCCESS)return S;
 S=XExact(D->Panel.Prepare(D->Panel.Context,&D->Dsi,&Owned));D->PanelOwned=Owned;
 if(S!=CR_SUCCESS)return S;if(!Owned)return CR_NOT_READY;
 if(D->Dsi.Plan.Mode.Dsc){S=CrMdssDsiSendPps(&D->Dsi,0);if(S!=CR_SUCCESS)return S;}
 S=XExact(D->Panel.PublishPixels(D->Panel.Context,D->Cpu,D->Bytes));if(S!=CR_SUCCESS)return S;
 S=CrMdssDpuExtendedPrepare(&D->Dpu);if(S!=CR_SUCCESS)return S;
 S=CrMdssDsiEnable(&D->Dsi);if(S!=CR_SUCCESS)return S;
 S=CrMdssDpuExtendedEnable(&D->Dpu);if(S!=CR_SUCCESS)return S;
 D->Active=TRUE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssExtendedStop(CR_MDSS_EXT_DISPLAY *D)
{
 CR_STATUS S;BOOLEAN Owned;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;D->CleanupPending=TRUE;
 S=CrMdssDpuExtendedStop(&D->Dpu);if(S!=CR_SUCCESS)return S;
 if(D->Dsi.Active&&!D->Dsi.CleanupPending){S=CrMdssDsiDisableVideo(&D->Dsi);if(S!=CR_SUCCESS)return S;}
 if(D->PanelOwned){Owned=TRUE;S=XExact(D->Panel.Unprepare(D->Panel.Context,&D->Dsi,&Owned));
  if(S!=CR_SUCCESS)return S;if(Owned)return CR_NOT_READY;D->PanelOwned=FALSE;}
 S=CrMdssDsiStop(&D->Dsi);if(S!=CR_SUCCESS)return S;
 D->Active=FALSE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssExtendedUninit(CR_MDSS_EXT_DISPLAY *D)
{
 CR_STATUS S;if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 S=CrMdssExtendedStop(D);if(S!=CR_SUCCESS)return S;cr_memset(D,0,sizeof(*D));return CR_SUCCESS;
}
