/* HDK8450 SDCC2 board wiring; preserves the qualified native profile.
 * Linux sm8450.dtsi OPP paths and sch_hdk8450 DSW1/DSW2 card-detect polarity.
 * SPDX-License-Identifier: MIT */
#include <Library/CrTargetSdLib.h>
STATIC CONST CR_TARGET_SD_CONTEXT mSd = {
  .Revision=CR_TARGET_SD_REVISION,.Size=sizeof(CR_TARGET_SD_CONTEXT),.Variant=CR_TARGET_SDHC_V5,
  .Base=0x08804000,.Bytes=0x1000,.PowerInterrupt=255,
  .PowerTrigger=CR_INTERRUPT_TRIGGER_LEVEL_HIGH,.ClockController="gcc",
  .Clocks={"gcc_sdcc2_ahb_clk","gcc_sdcc2_apps_clk"},
  .Rails={{"ldoc9",3008,7},{"ldoc6",3008,7}},
  .Paths={{"qcom,sm8450-sdhci",0x8000,0x8001,80000,160000},
          {"qcom,sm8450-sdhci",0x8002,0x8003,50000,100000}},
  .Gpio={.CdPin=92,.FirstPad=211,.PadControlOffset=0x1d6000},.CardDetectActiveLow=FALSE
};
CONST CR_TARGET_SD_CONTEXT *CrTargetGetSdContext(VOID) { return &mSd; }
