/* HDK8250 SDCC2, bounded high-voltage SDR/PIO resources.
 * Android cad09e061ef6cf689a4a1e54d27562e0f042236a kona-qrd.dtsi inherited
 * by kona-v2.1-hdk.dts; Linux a53261b60834ce617cc4f3c7723dda8285c59789
 * independently supplies SM8250 v5 registers, PM8150L PLDO and pin facts.
 * Run KonaCrPkg/Tools/generate_sd.py --check for pinned evidence.
 * SPDX-License-Identifier: MIT */
#include <Library/CrTargetSdLib.h>

STATIC CONST CR_TARGET_SD_CONTEXT mSd = {
  .Revision=CR_TARGET_SD_REVISION,.Size=sizeof(CR_TARGET_SD_CONTEXT),
  .Variant=CR_TARGET_SDHC_V5,.Base=0x08804000,.Bytes=0x1000,
  .PowerInterrupt=GIC_SPI(222),.PowerTrigger=CR_INTERRUPT_TRIGGER_LEVEL_HIGH,
  .ClockController="gcc",.Clocks={"gcc_sdcc2_ahb_clk","gcc_sdcc2_apps_clk"},
  /* Both consumer HIGH levels are 2960mV; 3008mV exceeds both board limits.
     PM8150L PLDO selector182 is1504+182*8=2960mV, HPM mode7. */
  .Rails={{"ldoc9",2960,7},{"ldoc6",2960,7}},
  /* Android's 25MB/s row covers the50MHz x4-bit SDR ceiling.
     Source/destination IDs are Crane's flattened Linux graph endpoints. */
  .Paths={{"qcom,sm8250-sdhci",0x8000,0x8001,65360,100000},
          {"qcom,sm8250-sdhci",0x8002,0x8003,100000,100000}},
  .Gpio={.CdPin=77,.FirstPad=181,.PadControlOffset=0x9b7000},
  .CardDetectActiveLow=TRUE,
};

CONST CR_TARGET_SD_CONTEXT *CrTargetGetSdContext(VOID) { return &mSd; }
