/* Linux SM8450 catalog, dpu_hw_{sspp,lm,ctl,vbif} and dpu_crtc alpha setup.
   SPDX-License-Identifier: GPL-2.0-only */
#include <Library/MdssDpuCursorCrLib.h>
#include <oskal/cr_string.h>
#define DMA 0x28000U
#define PRIMARY 0x24000U
#define CTL 0x15000U
#define LM 0x44000U
#define INTF 0x35000U
#define XIN (1U<<9)
/* Target Android sde_hw_get_xin_halt_status reads CTRL1[16 + xin].
   Mainline uses the low field; it is not this target's retirement witness. */
#define HALT_ACK (XIN<<16)
#define FETCH (1U<<2)
#define FLUSH ((1U<<24)|(1U<<17)|(1U<<6))
#define BASE_ROUTE ((1U<<24)|(1U<<18))
static CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
static CR_STATUS Ready(CR_MDSS_DPU_CURSOR *D){return Exact(D->Io.Ready(D->Io.Context));}
static CR_STATUS Read(CR_MDSS_DPU_CURSOR *D,CR_MDSS_CURSOR_WINDOW W,UINT32 O,UINT32 *V){
 UINT32 T;CR_STATUS S=Ready(D);if(S!=CR_SUCCESS)return S;
 if(W>=CrMdssCursorWindowCount||(O&3)||D->Io.Bytes[W]<4||O>D->Io.Bytes[W]-4)return CR_BAD_BUFFER_SIZE;
 S=Exact(D->Io.Read32(D->Io.Context,W,O,&T));if(S==CR_SUCCESS)*V=T;return S;
}
static CR_STATUS Write(CR_MDSS_DPU_CURSOR *D,CR_MDSS_CURSOR_WINDOW W,UINT32 O,UINT32 V){
 CR_STATUS S=Ready(D);if(S!=CR_SUCCESS)return S;
 if(W>=CrMdssCursorWindowCount||(O&3)||D->Io.Bytes[W]<4||O>D->Io.Bytes[W]-4)return CR_BAD_BUFFER_SIZE;
 return Exact(D->Io.Write32(D->Io.Context,W,O,V));
}
static CR_STATUS Fence(CR_MDSS_DPU_CURSOR *D){CR_STATUS S=Ready(D);return S==CR_SUCCESS?Exact(D->Io.Fence(D->Io.Context)):S;}
static CR_STATUS Delay(CR_MDSS_DPU_CURSOR *D){CR_STATUS S=Ready(D);return S==CR_SUCCESS?Exact(D->Io.DelayUs(D->Io.Context,50)):S;}
static CR_STATUS Match(CR_MDSS_DPU_CURSOR *D,CR_MDSS_CURSOR_WINDOW W,UINT32 O,UINT32 Want){
 UINT32 V;CR_STATUS S=Read(D,W,O,&V);return S!=CR_SUCCESS?S:V==Want?CR_SUCCESS:CR_DEVICE_ERROR;
}
static CR_STATUS Rmw(CR_MDSS_DPU_CURSOR *D,CR_MDSS_CURSOR_WINDOW W,UINT32 O,UINT32 Mask,UINT32 Bits){
 UINT32 V;CR_STATUS S=Read(D,W,O,&V);if(S!=CR_SUCCESS)return S;V=(V&~Mask)|(Bits&Mask);
 S=Write(D,W,O,V);if(S!=CR_SUCCESS)return S;S=Fence(D);return S!=CR_SUCCESS?S:Match(D,W,O,V);
}
CR_STATUS CrMdssDpuCursorInit(CR_MDSS_DPU_CURSOR *D,CONST CR_MDSS_DPU_CURSOR_IO *Io,
 CONST CR_MDSS_MODE *M,UINT64 PrimaryAddress,CONST UINT64 Address[2],UINTN Bytes){
 if(!D||!Io||!M||!Address||!Io->Context||!Io->Ready||!Io->Read32||!Io->Write32||!Io->Fence||!Io->DelayUs)return CR_INVALID_PARAMETER;
 if(D->Initialized)return CR_BUSY;
 if(Io->Physical[0]!=0xae01000ULL||Io->Physical[1]!=0xaeb0000ULL||M->Soc!=CR_MDSS_SOC_SM8450||M->Controller)return CR_UNSUPPORTED;
 if(Io->Bytes[0]<0x8f000||Io->Bytes[1]<0x3000||Io->Bytes[0]>CR_MAX_UINT64-Io->Physical[0]||Io->Bytes[1]>CR_MAX_UINT64-Io->Physical[1]||Bytes!=CR_MDSS_DPU_CURSOR_BYTES)return CR_BAD_BUFFER_SIZE;
 CR_STATUS S=CrMdssValidateMode(M,(UINTN)M->Stride*M->Height);if(S!=CR_SUCCESS)return S;
 UINT64 PrimaryBytes=(UINT64)M->Stride*M->Height;
 if(M->Width>2560||!PrimaryAddress||PrimaryAddress>CR_MAX_UINT32||PrimaryBytes-1>CR_MAX_UINT32-PrimaryAddress)return CR_BAD_BUFFER_SIZE;
 for(UINT32 I=0;I<2;++I){if(!Address[I]||(Address[I]&4095)||Address[I]>CR_MAX_UINT32||Bytes-1>CR_MAX_UINT32-Address[I]||
   (Address[I]<PrimaryAddress+PrimaryBytes&&PrimaryAddress<Address[I]+Bytes))return CR_BAD_BUFFER_SIZE;}
 if(Address[0]<Address[1]+Bytes&&Address[1]<Address[0]+Bytes)return CR_INVALID_PARAMETER;
 cr_memset(D,0,sizeof(*D));D->Io=*Io;D->Mode=*M;D->PrimaryAddress=PrimaryAddress;
 D->DeviceAddress[0]=Address[0];D->DeviceAddress[1]=Address[1];D->Initialized=TRUE;return CR_SUCCESS;
}
/* No foreign CTL or mixer may route DMA2. This owner reserves the entire DPU
   pipeline; the exact baseline is DMA0 in LM0 BASE and single DSI0. */
static CR_STATUS Routes(CR_MDSS_DPU_CURSOR *D){
 UINT32 V;CR_STATUS S;
 for(UINT32 C=0;C<6;++C){UINT32 Base=CTL+C*0x1000;
  S=Read(D,CrMdssCursorDpu,Base+0xfc,&V);if(S!=CR_SUCCESS)return S;
  if(V&FETCH){if(C||!D->Touched)return CR_BUSY;}
  for(UINT32 L=0;L<6;++L){UINT32 Off=L==5?0x24:L*4;
   S=Read(D,CrMdssCursorDpu,Base+0x70+L*4,&V);if(S!=CR_SUCCESS)return S;
   if(V&0xf000fU){if(C||L||(V&0xf000fU)!=2||!D->Touched)return CR_BUSY;}
   /* DMA2 is encoded in EXT2; other source fields remain untouched. */
   if(!C&&!L){S=Match(D,CrMdssCursorDpu,Base+Off,BASE_ROUTE);if(S!=CR_SUCCESS)return S;}
  }
 }
 return CR_SUCCESS;
}
static CR_STATUS Primary(CR_MDSS_DPU_CURSOR *D,BOOLEAN *On){
 CR_STATUS S=Routes(D);if(S!=CR_SUCCESS)return S;UINT32 V,E;
 S=Match(D,CrMdssCursorDpu,CTL+0x40,0);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssCursorDpu,CTL+0xa0,0);if(S!=CR_SUCCESS)return S;
 S=Read(D,CrMdssCursorDpu,CTL+0x70,&V);if(S!=CR_SUCCESS)return S;if(V!=0&&V!=2)return CR_BUSY;
 S=Read(D,CrMdssCursorDpu,INTF,&E);if(S!=CR_SUCCESS)return S;
 S=Read(D,CrMdssCursorDpu,INTF+0x26c,&V);if(S!=CR_SUCCESS)return S;
 if(!(E&1)&&!(V&1)){S=Read(D,CrMdssCursorDpu,CTL+0x30,&V);if(S!=CR_SUCCESS)return S;if(V&1)return CR_BUSY;*On=FALSE;return CR_SUCCESS;}
 if(!(E&1)||!(V&1))return CR_NOT_READY;
 S=Match(D,CrMdssCursorDpu,CTL+0xf4,2);if(S!=CR_SUCCESS)return S;
 S=Read(D,CrMdssCursorDpu,CTL+0xfc,&V);if(S!=CR_SUCCESS)return S;
 if(V!=1&&V!=(1U|FETCH))return CR_BUSY;
 if(V!=(D->Visible?1U|FETCH:1U)&&!D->CleanupPending)return CR_NOT_READY;
 S=Match(D,CrMdssCursorDpu,PRIMARY,D->Mode.Width|(D->Mode.Height<<16));if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssCursorDpu,PRIMARY+0x14,(UINT32)D->PrimaryAddress);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssCursorDpu,PRIMARY+0x30,0x236ff);if(S!=CR_SUCCESS)return S;
 S=Match(D,CrMdssCursorDpu,LM+4,D->Mode.Width|(D->Mode.Height<<16));if(S!=CR_SUCCESS)return S;
 *On=TRUE;return CR_SUCCESS;
}
static CR_STATUS HaltLevel(CR_MDSS_DPU_CURSOR *D,BOOLEAN On){
 for(UINT32 Us=0;Us<=16384;Us+=50){UINT32 V;CR_STATUS S=Read(D,CrMdssCursorVbif,0x204,&V);if(S!=CR_SUCCESS)return S;
  if(!!(V&HALT_ACK)==!!On)return Ready(D);if(Us+50>16384)break;S=Delay(D);if(S!=CR_SUCCESS)return S;}
 return CR_TIMEOUT;
}
static CR_STATUS Force(CR_MDSS_DPU_CURSOR *D){
 UINT32 V;CR_STATUS S;
 /* Linux forces the source clock while requesting/waiting for VBIF halt.
    Journal its baseline before the first possibly committed control write,
    including the initial Stop of an inherited, clock-gated DMA2 pipe. */
 if(!D->ForceTouched){S=Read(D,CrMdssCursorDpu,0x2bc,&V);if(S!=CR_SUCCESS)return S;
  D->ForceOriginal=V&(1U<<8);D->ForceTouched=TRUE;}
 return Rmw(D,CrMdssCursorDpu,0x2bc,1U<<8,1U<<8);
}
static CR_STATUS Halt(CR_MDSS_DPU_CURSOR *D){
 CR_STATUS S=Force(D);if(S!=CR_SUCCESS)return S;
 /* A possibly committed prior unhalt invalidates the earlier ACK. Observe
    it low before requesting a new high level; do not accept a stale drain. */
 if(!D->Halted){S=Rmw(D,CrMdssCursorVbif,0x200,XIN,0);if(S!=CR_SUCCESS)return S;
  S=HaltLevel(D,FALSE);if(S!=CR_SUCCESS)return S;}
 S=Rmw(D,CrMdssCursorVbif,0x200,XIN,XIN);if(S!=CR_SUCCESS)return S;
 S=HaltLevel(D,TRUE);if(S==CR_SUCCESS)D->Halted=TRUE;return S;
}
static CR_STATUS WaitFlush(CR_MDSS_DPU_CURSOR *D,UINT32 First){
 CR_STATUS S;
 for(UINT32 Us=0;Us<=250000;Us+=50){UINT32 V,F,Frame;
  S=Read(D,CrMdssCursorDpu,INTF+0x26c,&V);if(S!=CR_SUCCESS)return S;if(!(V&1))return CR_NOT_READY;
  S=Read(D,CrMdssCursorDpu,CTL+0x18,&F);if(S!=CR_SUCCESS)return S;
  S=Read(D,CrMdssCursorDpu,INTF+0xac,&Frame);if(S!=CR_SUCCESS)return S;
  if(!F&&Frame!=First)return Ready(D);if(Us==250000)break;S=Delay(D);if(S!=CR_SUCCESS)return S;}
 return CR_TIMEOUT;
}
static CR_STATUS Flush(CR_MDSS_DPU_CURSOR *D,UINT32 First){
 CR_STATUS S=Write(D,CrMdssCursorDpu,CTL+0x18,FLUSH);if(S!=CR_SUCCESS)return S;
 S=Fence(D);if(S!=CR_SUCCESS)return S;
 /* Continuing scanout is not evidence that this transaction was latched.
    Require the owned pending bits before accepting their later retirement.
    A transaction already consumed at a frame boundary may safely decline;
    its retained journal still requires Stop before any buffer retirement. */
 S=Match(D,CrMdssCursorDpu,CTL+0x18,FLUSH);
 return S!=CR_SUCCESS?S:WaitFlush(D,First);
}
CR_STATUS CrMdssDpuCursorStop(CR_MDSS_DPU_CURSOR *D){
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 BOOLEAN On;CR_STATUS S=Primary(D,&On);if(S!=CR_SUCCESS)return S;
 UINT32 V,First=0;S=Read(D,CrMdssCursorDpu,CTL+0x18,&V);if(S!=CR_SUCCESS)return S;
 if(On&&V){
  /* Complete this journal's earlier posted flush before issuing another.
     Never consume a foreign pipe's pending transaction. */
  if(!D->CleanupPending||(V&~FLUSH))return CR_BUSY;
  S=Read(D,CrMdssCursorDpu,INTF+0xac,&First);if(S!=CR_SUCCESS)return S;
  S=WaitFlush(D,First);if(S!=CR_SUCCESS)return S;
 }
 if(On){S=Read(D,CrMdssCursorDpu,INTF+0xac,&First);if(S!=CR_SUCCESS)return S;}
 D->Touched=TRUE;D->CleanupPending=TRUE;
 S=Rmw(D,CrMdssCursorDpu,CTL+0x70,0xf,0);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssCursorDpu,CTL+0xfc,FETCH,0);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssCursorDpu,LM,1U<<1,0);if(S!=CR_SUCCESS)return S;
 if(On){S=Flush(D,First);if(S!=CR_SUCCESS)return S;}
 S=Halt(D);if(S!=CR_SUCCESS)return S;
 if(D->ForceTouched){S=Rmw(D,CrMdssCursorDpu,0x2bc,1U<<8,D->ForceOriginal);if(S!=CR_SUCCESS)return S;D->ForceTouched=FALSE;}
 S=Ready(D);if(S!=CR_SUCCESS)return S;D->Visible=FALSE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
typedef struct {UINT32 SourceX,SourceY,Width,Height,X,Y;} CLIP;
static CR_STATUS Clip(CR_MDSS_DPU_CURSOR *D,CONST CR_MDSS_DPU_CURSOR_IMAGE *I,CLIP *C){
 if(I->DeviceAddress!=D->DeviceAddress[0]&&I->DeviceAddress!=D->DeviceAddress[1])return CR_BUSY;
 if(!I->Width||!I->Height||I->Width>CR_MDSS_DPU_CURSOR_MAX||I->Height>CR_MDSS_DPU_CURSOR_MAX||
    (I->Transform!=0&&I->Transform!=CR_MDSS_DPU_REFLECT_MASK))return CR_INVALID_PARAMETER;
 INT64 Left=I->X,Top=I->Y,Right=Left+I->Width,Bottom=Top+I->Height;
 if(!I->Visible||Right<=0||Bottom<=0||Left>=D->Mode.Width||Top>=D->Mode.Height){cr_memset(C,0,sizeof(*C));return CR_SUCCESS;}
 INT64 L=Left<0?0:Left,T=Top<0?0:Top,R=Right>D->Mode.Width?D->Mode.Width:Right,B=Bottom>D->Mode.Height?D->Mode.Height:Bottom;
 C->Width=(UINT32)(R-L);C->Height=(UINT32)(B-T);C->X=(UINT32)L;C->Y=(UINT32)T;
 C->SourceX=(UINT32)(I->Transform?Right-R:L-Left);C->SourceY=(UINT32)(I->Transform?Bottom-B:T-Top);return CR_SUCCESS;
}
static CR_STATUS Source(CR_MDSS_DPU_CURSOR *D,CONST CR_MDSS_DPU_CURSOR_IMAGE *I,CONST CLIP *C,BOOLEAN Program){
 const UINT32 Pair[][2]={{0,C->Width|(C->Height<<16)},{8,C->SourceX|(C->SourceY<<16)},
  {0xc,C->Width|(C->Height<<16)},{0x10,C->X|(C->Y<<16)},{0x14,(UINT32)I->DeviceAddress},{0x18,0},{0x1c,0},{0x20,0},
  {0x24,CR_MDSS_DPU_CURSOR_STRIDE},{0x28,0},{0x30,0x237ff},{0x34,0x03020001},
  {0x38,(1U<<31)|(I->Transform?((1U<<13)|(1U<<14)):0)},
  {0x60,0x3ffff},{0x64,0xfe00},{0x6c,1},{0x74,0x22335777},{0x78,0x00112222},
  {0xb4,0},{0x100,0},{0x104,0},{0x108,C->Width|(C->Height<<16)},
  {0x110,0},{0x114,0},{0x118,0},{0x120,0},{0x124,0},{0x128,C->Width|(C->Height<<16)},
  {0x134,9},{0x170,0}};
 for(UINT32 P=0;P<sizeof(Pair)/sizeof(Pair[0]);++P){
  CR_STATUS S=Program?Write(D,CrMdssCursorDpu,DMA+Pair[P][0],Pair[P][1]):Match(D,CrMdssCursorDpu,DMA+Pair[P][0],Pair[P][1]);
  if(S!=CR_SUCCESS)return S;
 }
 return CR_SUCCESS;
}
static CR_STATUS ImageReadback(CR_MDSS_DPU_CURSOR *D,CONST CR_MDSS_DPU_CURSOR_IMAGE *I,CONST CLIP *C){
 CR_STATUS S=Source(D,I,C,FALSE);if(S!=CR_SUCCESS)return S;
#define M(o,v) do{S=Match(D,CrMdssCursorDpu,(o),(v));if(S!=CR_SUCCESS)return S;}while(0)
 M(CTL+0x70,2);M(CTL+0xfc,1U|FETCH);M(LM+0x20,I->Premultiplied?0x600:0x602);M(LM+0x24,0xff0000);
#undef M
 UINT32 V;S=Read(D,CrMdssCursorDpu,DMA+0x70,&V);if(S!=CR_SUCCESS)return S;if(V&15)return CR_DEVICE_ERROR;
 return Ready(D);
}
CR_STATUS CrMdssDpuCursorSet(CR_MDSS_DPU_CURSOR *D,CONST CR_MDSS_DPU_CURSOR_IMAGE *I){
 if(!D||!D->Initialized||!I)return CR_INVALID_PARAMETER;if(D->CleanupPending)return CR_NOT_READY;
 CLIP C;CR_STATUS S=Clip(D,I,&C);if(S!=CR_SUCCESS)return S;
 BOOLEAN On;S=Primary(D,&On);if(S!=CR_SUCCESS)return S;if(!On)return CR_NOT_READY;
 S=CrMdssDpuCursorStop(D);if(S!=CR_SUCCESS)return S;
 if(!C.Width){D->Image=*I;D->Image.Visible=FALSE;return Ready(D);}
 UINT32 V,First;S=Read(D,CrMdssCursorDpu,INTF+0xac,&First);if(S!=CR_SUCCESS)return S;
 S=Read(D,CrMdssCursorDpu,0x2bc,&V);if(S!=CR_SUCCESS)return S;
 D->ForceOriginal=V&(1U<<8);D->ForceTouched=TRUE;D->CleanupPending=TRUE;
 S=Rmw(D,CrMdssCursorDpu,0x2bc,1U<<8,1U<<8);if(S!=CR_SUCCESS)return S;
 /* Source-qualified eight real-time levels, XIN9 only; foreign fields kept. */
 static CONST UINT32 Priority[8]={3,3,4,4,5,5,6,6};
 S=Rmw(D,CrMdssCursorVbif,0x164,0x70,0x30);if(S!=CR_SUCCESS)return S;
 for(UINT32 L=0;L<8;++L){S=Rmw(D,CrMdssCursorVbif,0x554+L*8,0x70,Priority[L]<<4);if(S!=CR_SUCCESS)return S;
  S=Rmw(D,CrMdssCursorVbif,0x594+L*8,0x70,Priority[L]<<4);if(S!=CR_SUCCESS)return S;}
 S=Source(D,I,&C,TRUE);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssCursorDpu,DMA+0x70,15,0);if(S!=CR_SUCCESS)return S;
 S=Write(D,CrMdssCursorDpu,LM+0x24,0xff0000);if(S!=CR_SUCCESS)return S;
 S=Write(D,CrMdssCursorDpu,LM+0x20,I->Premultiplied?0x600:0x602);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssCursorDpu,LM,1U<<1,1U<<1);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssCursorDpu,CTL+0x70,0xf,2);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssCursorDpu,CTL+0xfc,FETCH,FETCH);if(S!=CR_SUCCESS)return S;
 S=Fence(D);if(S!=CR_SUCCESS)return S;S=ImageReadback(D,I,&C);if(S!=CR_SUCCESS)return S;
 D->Halted=FALSE; /* before the first possibly committed unhalt request */
 S=Rmw(D,CrMdssCursorVbif,0x200,XIN,0);if(S!=CR_SUCCESS)return S;
 S=HaltLevel(D,FALSE);if(S!=CR_SUCCESS)return S;
 S=Rmw(D,CrMdssCursorDpu,0x2bc,1U<<8,D->ForceOriginal);if(S!=CR_SUCCESS)return S;D->ForceTouched=FALSE;
 S=Flush(D,First);if(S!=CR_SUCCESS)return S;S=ImageReadback(D,I,&C);if(S!=CR_SUCCESS)return S;
 D->Image=*I;D->Visible=TRUE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDpuCursorRetire(CR_MDSS_DPU_CURSOR *D){
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;if(D->CleanupPending||D->Visible||!D->Halted||D->ForceTouched)return CR_BUSY;
 CR_STATUS S=Routes(D);if(S!=CR_SUCCESS)return S;
 UINT32 V;S=Read(D,CrMdssCursorDpu,CTL+0x70,&V);if(S!=CR_SUCCESS)return S;if(V&0xf000fU)return CR_BUSY;
 S=Read(D,CrMdssCursorDpu,CTL+0xfc,&V);if(S!=CR_SUCCESS)return S;if(V&FETCH)return CR_BUSY;
 S=Read(D,CrMdssCursorVbif,0x204,&V);if(S!=CR_SUCCESS)return S;if(!(V&HALT_ACK))return CR_BUSY;
 S=Ready(D);if(S==CR_SUCCESS)cr_memset(D,0,sizeof(*D));return S;
}
