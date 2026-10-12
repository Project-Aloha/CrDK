/* Portable DPU/DSI hardware catalogs. SPDX-License-Identifier: GPL-2.0-only
 * Public Linux dpu_{6_0_sm8250,7_0_sm8350,8_1_sm8450}.h and dsi_phy_7nm.c;
 * Android SM8350 dsi_phy_hw_v4_0.c independently confirms PHY4.2 RESCODE.
 * Capabilities describe these implemented backends, not every silicon feature.
 * No board GPIO, rail, interconnect, ownership grant or hardware IO lives here.
 */
#include <Library/MdssCrLib.h>
#define WINDOWS \
  .DpuBase=0xae01000,.DsiBase={0xae94000,0xae96000},.PhyBase={0xae94400,0xae96400}, \
  .DpuBytes=0x8f000,.DsiBytes=0x400,.PhyBytes=0x800, \
  .Dma={0x24000,0x26000},.Lm={0x44000,0x45000},.DmaClock={0x2ac,0x2b4}
#define VIDEO_LAYOUT .Ctl=0x15000,.Pingpong={0x69000,0x6a000},.Intf={0x35000,0x36000}
STATIC CONST CR_MDSS_HW_PROFILE mProfiles[]={
  {.Soc=CR_MDSS_SOC_SM8250,.DpuMajor=6,.DpuMinor=0,.PhyRevision=0x410,
   .Capabilities=CR_MDSS_HW_COMMAND,.MaxControllers=1,.MaxMixerWidth=2560,
   WINDOWS,.Ctl=0x1000,.Pingpong={0x70000,0x70800},.Intf={0x6a800,0x6b000},.IntfBytes=0x2c0,
   .DphyRescodeTop={0x3d,0},.DphyRescodeBottom={0x39,0x3c}},
  {.Soc=CR_MDSS_SOC_SM8350,.DpuMajor=7,.DpuMinor=0,.PhyRevision=0x420,
   .Capabilities=CR_MDSS_HW_VIDEO|CR_MDSS_HW_CPHY|CR_MDSS_HW_BONDED,
   .MaxControllers=2,.MaxMixerWidth=2560,WINDOWS,VIDEO_LAYOUT,.IntfBytes=0x2c4,
   .DphyRescodeTop={0x3c,0},.DphyRescodeBottom={0x38,0x39},
   .CphyRescodeTop={0x3d,1},.CphyRescodeBottom={0x38,0x3b}},
  {.Soc=CR_MDSS_SOC_SM8450,.DpuMajor=8,.DpuMinor=1,.PhyRevision=0x430,
   .Capabilities=CR_MDSS_HW_VIDEO|CR_MDSS_HW_CPHY|CR_MDSS_HW_BONDED|CR_MDSS_HW_DSC|CR_MDSS_HW_REFGEN_VOTE,
   .MaxControllers=2,.MaxMixerWidth=2560,WINDOWS,VIDEO_LAYOUT,.IntfBytes=0x300,
   .DphyRescodeTop={0x3d,1},.DphyRescodeBottom={0x38,0x39},
   .CphyRescodeTop={0x3d,1},.CphyRescodeBottom={0x38,0x3b}},
};
#undef WINDOWS
#undef VIDEO_LAYOUT
CONST CR_MDSS_HW_PROFILE *CrMdssGetHardwareProfile(UINT32 Soc)
{
  for(UINT32 I=0;I<sizeof(mProfiles)/sizeof(mProfiles[0]);++I)
    if(mProfiles[I].Soc==Soc)return &mProfiles[I];
  return NULL;
}
