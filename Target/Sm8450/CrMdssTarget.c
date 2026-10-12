/* Public Linux/Android display resource identities. No hardware operations.
 * Board output routing and panel power are provided separately.
 * SPDX-License-Identifier: MIT */
#include <Library/CrTargetMdssLib.h>
STATIC CONST CR_MDSS_TARGET mMdss = {
  .Revision=CR_MDSS_TARGET_REVISION,.Size=sizeof(CR_MDSS_TARGET),.Soc=0x8450,
  .TopBase=0xae00000,.TopBytes=0x1000,.VbifBase=0xaeb0000,.VbifBytes=0x3000,
  .SmmuBase=0x15000000,.Interrupt=115,
  .Streams={0x2800,0x2802,0x2c00,0x2c02},.RequiresStoppedDisplay=FALSE,
  .Rails={{"ldob6",1200,7},{"ldob5",880,7}},
  /* Endpoint values are Crane's flattened target graph IDs. */
  .Paths={{"qcom,sm8450-mdp",0x4002,0x4003},{"qcom,sm8450-mdp",0x4000,0x4001}},
  .Clocks={{"dispcc","disp_cc_mdss_mdp_clk"},{"gcc","gcc_disp_hf_axi_clk"},
    {"gcc","gcc_disp_sf_axi_clk"},{"dispcc","disp_cc_mdss_mdp_lut_clk"},
    {"dispcc","disp_cc_mdss_vsync_clk"},{"dispcc","disp_cc_mdss_esc0_clk"},
    {"dispcc","disp_cc_mdss_esc1_clk"}},
};
CONST CR_MDSS_TARGET *CrTargetGetMdssContext(VOID) { return &mMdss; }
