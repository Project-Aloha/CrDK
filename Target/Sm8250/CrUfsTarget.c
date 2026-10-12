/** SM8250 HDK UFS resources, from pinned Linux/Android board references.
 * SPDX-License-Identifier: MIT
 * Source coordinates and effective DTB: KonaCrPkg/Tests/sm8250-ufs-facts.json.
 * Numeric PHY tables use Linux sm8250_ufsphy_cfg; Android differences remain
 * explicit in that evidence. No claim about a physically observed HCI major.
 */
#include <Library/CrTargetUfsLib.h>
#include "CrUfsPhyGenerated.h"

STATIC CONST CR_TARGET_UFS_CLOCK mClocks[] = {
  {"gcc_ufs_phy_axi_clk",300000000}, {"gcc_ufs_phy_ice_core_clk",300000000},
  {"gcc_ufs_phy_unipro_core_clk",300000000}, {"gcc_aggre_ufs_phy_axi_clk",0},
  {"gcc_ufs_phy_ahb_clk",0}, {"gcc_ufs_phy_phy_aux_clk",19200000},
  {"gcc_ufs_1x_clkref_en",0},
  {"gcc_ufs_phy_tx_symbol_0_clk",0}, {"gcc_ufs_phy_rx_symbol_0_clk",0},
  {"gcc_ufs_phy_rx_symbol_1_clk",0},
};
STATIC CONST CR_TARGET_UFS_RAIL mRails[] = {
  {"ldoa5",880,7}, {"ldoa9",1200,7}, {"ldoa17",2504,7}, {"ldoa6",1200,7},
  {"smpa4",1800,7},
};
STATIC CONST CR_TARGET_UFS_POWER_RULE mPowerRules[] = {
  {"ldoa5",1U<<4,880000,912000,8000,320000,880000,7},
  {"ldoa9",1U<<5,1200000,1200000,8000,320000,1200000,7},
  {"ldoa17",1U<<0,1704000,3008000,8000,1504000,2504000,7},
  {"ldoa6",(1U<<1)|(1U<<3),1200000,1200000,8000,320000,1200000,7},
  {"smpa4",1U<<2,1800000,1920000,8000,320000,1800000,7},
};
STATIC CONST CR_TARGET_UFS_POWER_POLICY mPowerPolicy = {
  1,sizeof(CR_TARGET_UFS_POWER_POLICY),0x177004,TRUE,1800000,
  ARRAY_SIZE(mPowerRules),mPowerRules,"smpc8",1352000,(1U<<4)|(1U<<2)
};
STATIC CONST UINT16 mSids[] = {0x00e0,0x04e0};
STATIC CONST CR_TARGET_UFS_PHY_WRITE mBase[] = {SM8250_UFS_PHY_BASE_INITIALIZERS};
STATIC CONST CR_TARGET_UFS_PHY_WRITE mGear4[] = {SM8250_UFS_PHY_G4_INITIALIZERS};
STATIC CONST CR_TARGET_UFS_PHY_WRITE mRateB[] = {SM8250_UFS_PHY_RATE_B_INITIALIZERS};
STATIC CONST CR_TARGET_UFS_CONTEXT mUfs = {
  .Revision=CR_TARGET_UFS_REVISION, .Size=sizeof(CR_TARGET_UFS_CONTEXT),
  .HciBase=0x01d84000, .HciBytes=0x3000, .PhyBase=0x01d87000, .PhyBytes=0xe00,
  /* Admission for the implemented 300MHz timer profile, not a PHY-derived
     revision guess. Major3 needs a separately qualified 150MHz profile. */
  .AllowedHciMajors=(1U<<4), .PhyGeneration=4, .MaxHsGear=4, .Lanes=2,
  .ClockController="gcc", .Gdsc="ufs_phy_gdsc", .Reset="gcc_ufs_phy_bcr",
  .Clocks=mClocks, .ClockCount=ARRAY_SIZE(mClocks), .PrePhyClockCount=7,
  .Rails=mRails, .RailCount=ARRAY_SIZE(mRails),
  .Paths={{"qcom,sm8250-ufs",0x3000,0x3001,0,7643136},
          {"qcom,sm8250-ufs",0x3002,0x3003,0,819200}},
  /* UFS_RESET uses special pad drive encoding3 = 3.1mA. Generic pin binding
     calls this drive-strength8; it is not an ordinary TLMM 8mA pad. */
  .DeviceReset={.PinNumber=180,.OutputEnable=TRUE,.FunctionSel=GPIO_FUNC_UNCHANGE,
    .Pull=GPIO_PULL_DOWN,.DriveStrength=3,.OutputValue=GPIO_VALUE_LOW},
  .SmmuBase=0x15000000, .Sids=mSids, .SidCount=ARRAY_SIZE(mSids),
  .PhyStart=0xc00, .PhyPower=0xc04, .PhySwReset=0xc08,
  .PhyReady=0xd80, .PhyLineCfg=0xd48,
  .Base={mBase,ARRAY_SIZE(mBase)}, .Gear4={mGear4,ARRAY_SIZE(mGear4)},
  .RateB={mRateB,ARRAY_SIZE(mRateB)},
  .RxInterfaceOffsets={0x734,0xb34}, .RxInterfaceCount=2,
  /* Android derives local UniPro1.41 from standard HCI VERSION1.0/1.1,
     separately from Qualcomm HW_VERSION0xe4. Preclear, conditional postset. */
  .LineCfgPolicy=CrUfsLineCfgAndroidConditional,
  /* No qualified SM8250 operation for CFG1 bit31: leave it untouched. */
  .SkipIceForNonScsi=FALSE,
  .PowerPolicy=&mPowerPolicy,
  .RetainedRailMask=(1U<<0)|(1U<<4),
  .AcquireInheritedSmmu=TRUE,
};

CONST CR_TARGET_UFS_CONTEXT *CrTargetGetUfsContext(VOID)
{
  return &mUfs;
}
