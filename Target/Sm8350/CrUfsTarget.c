/** SM8350 HDK UFS profile, qualified from public Android and Linux.
 * SPDX-License-Identifier: MIT
 * References: LahainaCrPkg/Tests/sm8350-ufs-facts.json. The PHY is the
 * SM8350-selected mainline v5 layout, named v4-lahaina by Android R11.
 * A BL-FDT power record is mandatory. Defaults below are qualified preferred
 * votes, never permission to replace missing or invalid boot supply data.
 */
#include <Library/CrTargetUfsLib.h>
#include "CrUfsPhyGenerated.h"

STATIC CONST CR_TARGET_UFS_CLOCK mClocks[] = {
  {"gcc_ufs_phy_axi_clk",300000000}, {"gcc_ufs_phy_ice_core_clk",300000000},
  {"gcc_ufs_phy_unipro_core_clk",300000000}, {"gcc_aggre_ufs_phy_axi_clk",0},
  {"gcc_ufs_phy_ahb_clk",0}, {"gcc_ufs_phy_phy_aux_clk",19200000},
  {"gcc_ufs_1_clkref_en",0}, {"gcc_ufs_phy_tx_symbol_0_clk",0},
  {"gcc_ufs_phy_rx_symbol_0_clk",0}, {"gcc_ufs_phy_rx_symbol_1_clk",0},
};
STATIC CONST CR_TARGET_UFS_RAIL mRails[] = {
  {"ldob5",880,7}, {"ldob6",1200,7}, {"ldob7",2504,7}, {"ldob9",1200,7},
};
STATIC CONST CR_TARGET_UFS_POWER_RULE mPowerRules[] = {
  {"ldob5",1U<<4,880000,888000,8000,320000,880000,7},
  {"ldob6",1U<<5,1200000,1208000,8000,320000,1200000,7},
  {"ldob7",1U<<0,2400000,3008000,8000,1504000,2504000,7},
  {"ldob9",(1U<<1)|(1U<<3),1200000,1200000,8000,320000,1200000,7},
};
/* Android requests enable alone for its optional S12b supplier. Provider
 * init voltage is not a UFS vote. Mainline omits this consumer supply.
 * Neither tree requests VCCQ2. BOB parent leases are owned by the provider. */
STATIC CONST CR_TARGET_UFS_POWER_POLICY mPowerPolicy = {
  2,sizeof(CR_TARGET_UFS_POWER_POLICY),0x177004,TRUE,0,
  ARRAY_SIZE(mPowerRules),mPowerRules,"smpb12",0,0
};
STATIC CONST UINT16 mSids[] = {0x00e0};
STATIC CONST CR_TARGET_UFS_PHY_WRITE mBase[] = {SM8350_UFS_PHY_BASE_INITIALIZERS};
STATIC CONST CR_TARGET_UFS_PHY_WRITE mGear4[] = {SM8350_UFS_PHY_G4_INITIALIZERS};
STATIC CONST CR_TARGET_UFS_PHY_WRITE mRateB[] = {SM8350_UFS_PHY_RATE_B_INITIALIZERS};
STATIC CONST CR_TARGET_UFS_CONTEXT mUfs = {
  .Revision=CR_TARGET_UFS_REVISION, .Size=sizeof(CR_TARGET_UFS_CONTEXT),
  /* Every used register lies in the first0xe00 bytes, inside both Android's
   *0xe10 window and mainline's0x1000. Do not claim unused secondary PCS. */
  .HciBase=0x01d84000, .HciBytes=0x3000, .PhyBase=0x01d87000, .PhyBytes=0xe00,
  /* Hardware revision is read at runtime. Major4 is an admitted timer
   * profile, not an inference from a compatible string or PHY generation. */
  .AllowedHciMajors=(1U<<4), .PhyGeneration=5, .MaxHsGear=4, .Lanes=2,
  .ClockController="gcc", .Gdsc="ufs_phy_gdsc", .Reset="gcc_ufs_phy_bcr",
  .Clocks=mClocks, .ClockCount=ARRAY_SIZE(mClocks), .PrePhyClockCount=7,
  .Rails=mRails, .RailCount=ARRAY_SIZE(mRails),
  .Paths={{"qcom,sm8350-ufs",0x3000,0x3001,0,7643136},
          {"qcom,sm8350-ufs",0x3002,0x3003,0,819200}},
  /* Both DTs expose reset-gpios203, without a reset pinctrl drive/pull
   * state. Preserve those fields as gpiod_direction_output does. */
  .DeviceReset={.PinNumber=203,.OutputEnable=TRUE,.FunctionSel=GPIO_FUNC_UNCHANGE,
    .Pull=GPIO_PULL_UNCHANGE,.DriveStrength=GPIO_DRIVE_STRENGTH_UNCHANGE,
    .OutputValue=GPIO_VALUE_LOW},
  .SmmuBase=0x15000000, .Sids=mSids, .SidCount=ARRAY_SIZE(mSids),
  .PhyStart=0xc00, .PhyPower=0xc04, .PhySwReset=0xc08,
  .PhyReady=0xd80, .PhyLineCfg=0xd48,
  .Base={mBase,ARRAY_SIZE(mBase)}, .Gear4={mGear4,ARRAY_SIZE(mGear4)},
  .RateB={mRateB,ARRAY_SIZE(mRateB)},
  .RxInterfaceOffsets={0x734,0xb34}, .RxInterfaceCount=2,
  .LineCfgPolicy=CrUfsLineCfgAndroidConditional,
  .SkipIceForNonScsi=FALSE, .PowerPolicy=&mPowerPolicy, .RetainedRailMask=0,
  .AcquireInheritedSmmu=TRUE,
};

CONST CR_TARGET_UFS_CONTEXT *CrTargetGetUfsContext(VOID) { return &mUfs; }
