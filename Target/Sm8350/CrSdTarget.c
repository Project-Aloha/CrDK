/** HDK8350 SDCC2, final Android R11 board policy. SPDX-License-Identifier: MIT
 * Pinned/effective DT evidence: LahainaCrPkg/Tests/sm8350-storage-facts.json.
 * Native profile: per-pad drive policy and explicit shared regulator parents.
 */
#include <Library/CrTargetSdLib.h>
STATIC CONST CR_TARGET_SD_CONTEXT mSd = {
  .Revision=CR_TARGET_SD_REVISION,.Size=sizeof(CR_TARGET_SD_CONTEXT),
  .Variant=CR_TARGET_SDHC_V5,.Base=0x08804000,.Bytes=0x1000,
  .PowerInterrupt=GIC_SPI(223),.PowerTrigger=CR_INTERRUPT_TRIGGER_LEVEL_HIGH,
  .ClockController="gcc",.Clocks={"gcc_sdcc2_ahb_clk","gcc_sdcc2_apps_clk"},
  .Rails={{"ldoc9",2960,7},{"ldoc6",2960,7}},
  .Paths={{"qcom,sm8350-sdhci",0x8000,0x8001,65360,100000},
          {"qcom,sm8350-sdhci",0x8002,0x8003,100000,100000}},
  .Gpio={.CdPin=92,.FirstPad=204,.PadControlOffset=0xcf000,
         .ActiveDriveMa={16,10,10}},
  /* lahaina-hdk.dtsi overrides qrd-common's LOW; effective HDK is HIGH. */
  .CardDetectActiveLow=FALSE,
};
CONST CR_TARGET_SD_CONTEXT *CrTargetGetSdContext(VOID) { return &mSd; }
