/* Real panel lifecycle + DSI commands/PHY/DPU/DSC. GPL-2.0-only */
#define main PriorExtendedRegression
#include "MdssDsiExtendedTest.c"
#undef main
#include <Library/MdssDsiPanelCrLib.h>
typedef struct {HW *H;UINT32 Calls,Fail,Acquires,Releases,ResetCalls,Waits;BOOLEAN Held,ResetLevel,Commit;CR_STATUS Failure;} PANEL_HW;
static CR_STATUS PanelStep(PANEL_HW *P){++P->Calls;return P->Calls==P->Fail?P->Failure:CR_SUCCESS;}
static CR_STATUS PanelReady(VOID *C){PANEL_HW *P=C;return PanelStep(P);}
static CR_STATUS PanelAcquire(VOID *C,BOOLEAN *Owned){PANEL_HW *P=C;CR_STATUS S=PanelStep(P);++P->Acquires;
 if(S==CR_SUCCESS||P->Commit){P->Held=TRUE;*Owned=TRUE;}return S;}
static CR_STATUS PanelRelease(VOID *C,BOOLEAN *Owned){PANEL_HW *P=C;CR_STATUS S=PanelStep(P);++P->Releases;
 assert(!(P->H->R[CrMdssExtDsi0][1]&2)&&!(P->H->R[CrMdssExtDsi1][1]&2));
 if(S==CR_SUCCESS){P->Held=FALSE;*Owned=FALSE;}return S;}
static CR_STATUS PanelReset(VOID *C,BOOLEAN High){PANEL_HW *P=C;assert(P->Held);++P->ResetCalls;CR_STATUS S=PanelStep(P);
 if(S==CR_SUCCESS)P->ResetLevel=High;return S;}
static CR_STATUS PanelDelay(VOID *C,UINT32 Us){PANEL_HW *P=C;assert(Us&&Us<=20000);++P->Waits;return PanelStep(P);}
static CR_STATUS PanelPublish(VOID *C,VOID *Cpu,UINTN Bytes){PANEL_HW *P=C;assert(P->Held&&Cpu&&Bytes==7680*1080);return PanelStep(P);}
static CONST UINT8 ExitSleep[]={0x11},DisplayOn[]={0x29},DisplayOff[]={0x28},EnterSleep[]={0x10};
static CR_MDSS_PANEL_COMMAND On[]={
 {0xff,0,TRUE,0,10,NULL,0},
 {0x05,3,TRUE,0,120,ExitSleep,1},{0x05,3,TRUE,0,20,DisplayOn,1}};
static CR_MDSS_PANEL_COMMAND Off[]={
 {0x05,3,TRUE,0,20,DisplayOff,1},{0x05,3,TRUE,0,120,EnterSleep,1}};
static CONST CR_MDSS_PANEL_RESET Resets[]={{TRUE,{0},20000},{FALSE,{0},20000},{TRUE,{0},120000}};
static UINT32 Configure(HW *H,PANEL_HW *P,CR_MDSS_DSI_PANEL *Panel,CR_MDSS_EXT_DISPLAY *D,
                        BOOLEAN Cphy,BOOLEAN Dsc) {
 Reset(H);memset(P,0,sizeof(*P));P->H=H;P->Commit=TRUE;memset(Panel,0,sizeof(*Panel));
 CR_MDSS_PANEL_PROFILE F={.Revision=1,.Mode=Mode(Cphy,2,Cphy?2:4),.On=On,.Off=Off,
  .OnCount=ARRAY_SIZE(On),.OffCount=ARRAY_SIZE(Off),.Reset=Resets,.ResetCount=ARRAY_SIZE(Resets)};
 F.Mode.Dsc=Dsc;
 if(Dsc){F.Mode.SlicesPerPacket=1;CR_MDSS_DSC_PARAMS Q={.Revision=1,.MinorVersion=2,.BitsPerComponent=8,.BppQ4=128,
  .PictureWidth=1920,.PictureHeight=1080,.SliceWidth=480,.SliceHeight=8,.BlockPrediction=TRUE};
  assert(CrMdssDscBuild(&Q,&F.Mode.Compression)==CR_SUCCESS);}
 CR_MDSS_PANEL_IO PI={P,PanelReady,PanelAcquire,PanelRelease,PanelReset,PanelDelay,PanelPublish};
 assert(CrMdssDsiPanelInit(Panel,&F,&PI)==CR_SUCCESS&&!P->Calls);
 CR_MDSS_EXT_IO I=Io(H);CR_MDSS_EXT_PANEL X={Panel,CrMdssDsiPanelPrepare,CrMdssDsiPanelUnprepare,CrMdssDsiPanelPublish};
 CR_MDSS_DSI_COMMAND_MEMORY B={H,H->Command,sizeof(H->Command),0x20000000,CommandReady,Publish};
 assert(CrMdssExtendedInit(D,&I,&X,&F.Mode,H,0x10800000,7680*1080,Dsc?&B:NULL)==CR_SUCCESS);
 return P->Calls;
}
#ifndef CR_MDSS_PANEL_FIXTURE_ONLY
int main(void) {
 HW H;PANEL_HW P;CR_MDSS_DSI_PANEL Panel;CR_MDSS_EXT_DISPLAY D;
 UINT32 Phases=0;
 for(UINT32 C=0;C<2;++C)for(UINT32 DSC=0;DSC<2;++DSC) {
  Configure(&H,&P,&Panel,&D,C,DSC);assert(CrMdssExtendedStart(&D)==CR_SUCCESS&&D.Active&&P.Held&&Panel.Prepared);
  assert(H.PacketTriggers==(DSC?8U:4U));UINT32 Start=P.Calls;P.Calls=0;
  assert(CrMdssExtendedStop(&D)==CR_SUCCESS&&!P.Held&&!Panel.Prepared);UINT32 Stop=P.Calls;
  assert(CrMdssDsiPanelUninit(&Panel)==CR_SUCCESS);assert(CrMdssExtendedUninit(&D)==CR_SUCCESS);
  for(UINT32 Stage=0;Stage<2;++Stage)for(UINT32 Positive=0;Positive<2;++Positive)
   for(UINT32 Fail=1;Fail<=(Stage?Stop:Start);++Fail) {
    Configure(&H,&P,&Panel,&D,C,DSC);
    if(Stage)assert(CrMdssExtendedStart(&D)==CR_SUCCESS);
    P.Calls=0;P.Fail=Fail;P.Failure=Positive?1:CR_DEVICE_ERROR;
    CR_STATUS S=Stage?CrMdssExtendedStop(&D):CrMdssExtendedStart(&D);
    /* Failed off-packet delays can still be physically fenced by reset+power;
       only that complete physical fence is allowed to return success. */
    if(S==CR_SUCCESS){assert(Stage&&!P.Held&&!Panel.ResetTouched);}
    else {assert(D.CleanupPending);assert(CrMdssExtendedStart(&D)==CR_NOT_READY);}
    P.Fail=0;assert(CrMdssExtendedUninit(&D)==CR_SUCCESS&&!P.Held);
    assert(CrMdssDsiPanelUninit(&Panel)==CR_SUCCESS);++Phases;
   }
 }
 /* A poisoned on-command must reset physically without replaying any off
    command; both host clocks/resources remain until the actual DMA fence. */
 Configure(&H,&P,&Panel,&D,TRUE,TRUE);H.NeverComplete=TRUE;
 assert(CrMdssExtendedStart(&D)==CR_TIMEOUT&&Panel.PowerOwned&&D.PanelOwned);
 UINT32 Packets=H.PacketTriggers;assert(CrMdssDsiPanelUninit(&Panel)==CR_BUSY);
 H.NeverComplete=FALSE;assert(CrMdssExtendedUninit(&D)==CR_SUCCESS&&H.PacketTriggers==Packets&&!P.Held);
 assert(CrMdssDsiPanelUninit(&Panel)==CR_SUCCESS);
 /* Changed command topology/mode is rejected before GPIO/power/packet IO. */
 Configure(&H,&P,&Panel,&D,FALSE,FALSE);UINT32 Calls=H.Calls;BOOLEAN Owned;
 D.Dsi.Plan.Mode.Raster.DisableEot=TRUE;
 assert(CrMdssDsiPanelPrepare(&Panel,&D.Dsi,&Owned)==CR_UNSUPPORTED&&!Owned&&!P.Calls&&H.Calls==Calls);
 assert(CrMdssExtendedUninit(&D)==CR_SUCCESS);assert(CrMdssDsiPanelUninit(&Panel)==CR_SUCCESS);
 Configure(&H,&P,&Panel,&D,FALSE,FALSE);On[1].ControllerMask=4;
 assert(CrMdssPanelProfileValidate(&Panel.Profile)==CR_INVALID_PARAMETER);On[1].ControllerMask=3;
 On[0].Payload=ExitSleep;
 assert(CrMdssPanelProfileValidate(&Panel.Profile)==CR_INVALID_PARAMETER);On[0].Payload=NULL;
 CR_MDSS_PANEL_PROFILE Bad=Panel.Profile;Bad.OnCount=257;
 assert(CrMdssPanelProfileValidate(&Bad)==CR_INVALID_PARAMETER);
 Bad=Panel.Profile;Bad.Revision=2;assert(CrMdssPanelProfileValidate(&Bad)==CR_INVALID_PARAMETER);
 assert(CrMdssExtendedUninit(&D)==CR_SUCCESS);assert(CrMdssDsiPanelUninit(&Panel)==CR_SUCCESS);
 printf("Actual panel/dual DSI/CPHY/DSC caller: %u callback fault phases, physical off recovery, immutable mode and topology PASS\n",Phases);
 return 0;
}

#endif
