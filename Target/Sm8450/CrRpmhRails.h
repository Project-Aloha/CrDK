/* Native HDK8450 RPMh inventory. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/CrDalLib.h>

/* sm8450-hdk.dts: combo PHY L1B/L6B, HS PHY L5B/L1C/L2B,
   FSA4480 PM8350C BOB, SDCC2 L6C/L9C. Existing L5B/L6B descriptions
   are deduplicated. These native names must include every SD power request. */
STATIC CONST CHAR8 *CONST mSm8450RailNames[] = {
  "ldob1", "ldob2", "ldob5", "ldob6", "ldoc1", "bobc1",
  "smpb10", "smpb11", "smpb12", "ldoc6", "ldoc9"
};
STATIC CONST CR_DAL_RPMH_RAILS mSm8450Rails = {
  sizeof(mSm8450RailNames) / sizeof(mSm8450RailNames[0]), mSm8450RailNames
};

/* sm8450-hdk.dts input supplies, confirmed by schematic sheets 23/26/30.
   Round the S11B/S12B DTS minima up to their 8mV hardware selectors. BOB's
   3296mV vendor default is within the mainline range and supplies 3072mV L2B.
   PMIC5 SMPS/BOB AUTO=6; these are not LDO HPM=7. The external M.2
   VREG_BOB_3P3 converter is distinct from this PM8350C BOB resource. */
STATIC CONST CR_DAL_RPMH_PARENT mSm8450RailParents[] = {
  {"ldob1", "smpb11",  968, 6},
  {"ldob2", "bobc1",  3296, 6},
  {"ldob5", "bobc1",  3296, 6},
  {"ldob6", "smpb12", 1352, 6},
  {"ldob7", "bobc1",  3296, 6},
  {"ldob9", "smpb12", 1352, 6},
  {"ldoc1", "bobc1",  3296, 6},
  {"ldoc6", "bobc1",  3296, 6},
  {"ldoc9", "bobc1",  3296, 6},
  {"ldoh2", "bobc1",  3296, 6},
};
STATIC CONST CR_DAL_RPMH_TOPOLOGY mSm8450RailTopology = {
  sizeof(mSm8450RailParents) / sizeof(mSm8450RailParents[0]), mSm8450RailParents
};
