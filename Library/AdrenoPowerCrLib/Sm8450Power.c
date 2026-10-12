/* SM8450/A730.1 mainline source facts. SPDX-License-Identifier: GPL-2.0-only
 * Linux a53261b60834ce617cc4f3c7723dda8285c59789 sm8450.dtsi GPU/GMU OPPs,
 * qcom,rpmhpd.h corner values and a6xx_gmu.c resume/initial-index selection.
 */
#include <Library/AdrenoPowerCrLib.h>

STATIC CONST CR_ADRENO_POWER_PROFILE Profile = {
  .Version=CR_ADRENO_POWER_VERSION, .ChipId=CR_ADRENO_POWER_A730_CHIP_ID,
  .GpuCount=13, .GmuCount=3, .InitialGpuIndex=12, .GmuBootIndex=1,
  .GmuBootHz=200000000, .HubBootHz=150000000,
  .Gpu={
    {0,0,0}, {220000000,56,0}, {285000000,56,0},
    {317000000,64,0}, {350000000,64,0}, {421000000,80,0},
    {492000000,128,0}, {545000000,144,0}, {599000000,192,0},
    {640000000,224,0}, {734000000,256,0}, {791000000,320,0},
    {818000000,384,0},
  },
  .Gmu={{0,0,0}, {200000000,64,0}, {500000000,128,0}},
};
CONST CR_ADRENO_POWER_PROFILE *CrAdrenoSm8450PowerProfile(VOID) { return &Profile; }
