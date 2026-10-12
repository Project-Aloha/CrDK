/* Native owned XML -> actual typed CPHY/DSC caller profile. GPL-2.0-only */
#define main PriorPanelParserRegression
#include "MdpPanelTest.c"
#undef main
#include "../Driver/MDSSCrDxe/MdpExtendedPanel.h"
#ifdef CR_MDSS_PROFILE_BEFORE
#define MdpBuildExtendedPanelForSoc(Soc,...) MdpBuildExtendedPanel(__VA_ARGS__)
#endif
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
int main(int argc,char **argv) {
 assert(argc==3);CONST UINT8 Lanes[]={1,2,4,8};
 CONST CR_MDSS_PANEL_RESET R[]={{FALSE,{0},10},{TRUE,{0},10000}};
 MDP_PANEL_CONFIG P;MDP_EXTENDED_PANEL Out,Before;
 for(UINT32 File=1;File<3;++File) {
  size_t N;CHAR8 *Xml=ReadFile(argv[File],&N);memset(&P,0,sizeof(P));
  assert(MdpParsePanelXml(Xml,N,&P)==EFI_SUCCESS);free(Xml);
  CR_MDSS_DSC_PARAMS Compression={.Revision=1,.MinorVersion=P.DscMinor,.ScrVersion=P.DscScr,
   .BitsPerComponent=8,.BppQ4=128,.PictureWidth=P.Timing.Width,.PictureHeight=P.Timing.Height,
   .SliceWidth=P.DscSliceWidth,.SliceHeight=P.DscSliceHeight,.BlockPrediction=TRUE};
  memset(&Out,0xa5,sizeof(Out));Before=Out;
  EFI_STATUS Status=MdpBuildExtendedPanel(&P,File==1?NULL:&Compression,Lanes,TRUE,R,2,FALSE,&Out);
  if(Status!=EFI_SUCCESS)fprintf(stderr,"asset%u status=%llx flags unsupported=%u/%u/%u iface=%u color=%u/%u DSC=%u controller=%u count=%u\n",File,(unsigned long long)Status,P.HasUnsupportedCommandMode,P.HasUnsupportedGeometry,P.HasUnsupportedPhy,P.InterfaceType,P.InterfaceColorFormat,P.Timing.DsiColorFormat,P.Dsc,P.Timing.DsiController,P.ControllerCount);
  assert(Status==EFI_SUCCESS);
  assert(Out.Profile.On==Out.On&&Out.Profile.Off==Out.Off);
  assert(Out.Profile.Mode.PhyType==(File==1?CrMdssDphy:CrMdssCphy));
  assert(Out.Profile.Mode.Dsc==(File==2)&&Out.Profile.Mode.Controllers==1);
  assert(CrMdssPanelProfileValidate(&Out.Profile)==CR_SUCCESS);
  CR_MDSS_DSI_PLAN Plan;assert(CrMdssDsiBuildPlan(&Out.Profile.Mode,&Plan)==CR_SUCCESS);
  assert(Plan.Mode.Raster.Width==P.Timing.Width&&Plan.Mode.Raster.Height==P.Timing.Height);
  if(File==2) {assert(Out.Profile.Mode.Compression.Params.PictureWidth==1080);
    assert(Out.Profile.Mode.Compression.SliceCount==2&&Out.Profile.Mode.SlicesPerPacket==2);
    UINT32 Delays=0;
    for(UINT32 I=0;I<Out.Profile.OnCount;++I)if(Out.On[I].DataType==0xff){++Delays;assert(!Out.On[I].ControllerMask&&!Out.On[I].Payload);}
    assert(Delays);Before=Out;
    UINTN SavedLength=P.InitSequence.Commands[0].Length;
    P.InitSequence.Commands[0].Length=0x100000000ULL+SavedLength;
    assert(MdpBuildExtendedPanel(&P,&Compression,Lanes,TRUE,R,2,FALSE,&Out)==EFI_INVALID_PARAMETER&&!memcmp(&Out,&Before,sizeof(Out)));
    P.InitSequence.Commands[0].Length=SavedLength;Compression.SliceWidth++;
    assert(MdpBuildExtendedPanel(&P,&Compression,Lanes,TRUE,R,2,FALSE,&Out)==EFI_UNSUPPORTED&&!memcmp(&Out,&Before,sizeof(Out)));
    Compression.SliceWidth--;P.DscFieldsPresent[4]=FALSE;
    assert(MdpBuildExtendedPanel(&P,&Compression,Lanes,TRUE,R,2,FALSE,&Out)==EFI_COMPROMISED_DATA&&!memcmp(&Out,&Before,sizeof(Out)));
  }else {
    assert(MdpValidateVideoPanel(&P)==EFI_SUCCESS);
    assert(MdpBuildExtendedPanelForSoc(CR_MDSS_SOC_SM8350,&P,NULL,Lanes,TRUE,R,2,FALSE,&Out)==EFI_SUCCESS);
    assert(Out.Profile.Mode.Raster.Soc==CR_MDSS_SOC_SM8350);
    assert(CrMdssDsiBuildPlan(&Out.Profile.Mode,&Plan)==CR_SUCCESS);
    assert(CrMdssGetHardwareProfile(Plan.Mode.Raster.Soc)->PhyRevision==0x420);
    Before=Out;
    assert(MdpBuildExtendedPanelForSoc(0xffff,&P,NULL,Lanes,TRUE,R,2,FALSE,&Out)==EFI_UNSUPPORTED);
    assert(!memcmp(&Out,&Before,sizeof(Out)));
    assert(MdpBuildExtendedPanelForSoc(CR_MDSS_SOC_SM8250,&P,NULL,Lanes,TRUE,R,2,FALSE,&Out)==EFI_UNSUPPORTED);
    assert(!memcmp(&Out,&Before,sizeof(Out)));
    P.Cphy=TRUE;P.Timing.DsiLanes=4;
    assert(MdpBuildExtendedPanel(&P,NULL,Lanes,TRUE,R,2,FALSE,&Out)==EFI_INVALID_PARAMETER&&!memcmp(&Out,&Before,sizeof(Out)));
  }
  MdpFreePanelConfig(&P);
 }
 puts("Native real XML extended caller: DPHY and CPHY/DSC fields, owned commands/delays, explicit compression and unchanged output on failure PASS");
 return 0;
}
