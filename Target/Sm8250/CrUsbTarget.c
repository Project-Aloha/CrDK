/* Native QCS8245 IoT HDK USB profile. SPDX-License-Identifier: GPL-2.0-only
 * Resources: supplied HDK8245 Android DT, public Android kona-usb.dtsi /
 * kona-hdk.dtsi cad09e061ef6cf689a4a1e54d27562e0f042236a and Linux SM8250.
 * Cold USB2 uses the first three rails and no SS PHY programming. The
 * acknowledged RPMh boot floor supplies the always-on CXO PHY reference.
 */
#include <Library/HdkUsbTargetLib.h>
#include "CrUsbPhyGenerated.h"
STATIC CONST CR_HDK_USB_WRITE mHs0[]={{0x6c,0xc7,0},{0x70,3,0},{0x74,3,0}};
STATIC CONST CR_HDK_USB_WRITE mHs1[]={{0x70,0x43,0}};
/* Active PMIC5 LDO NORMAL mode is hardware HPM=7. Android qcom,init-mode
 * values are framework enums, not the RPMh hardware encoding. In particular
 * ldoa5 is also held by UFS at 880mV/HPM; use the same shared vote. */
#define COMMON_RAILS {{"ldoa5",880,7},{"ldoa12",1800,7},{"ldoa2",3072,7},{"ldoa18",912,7},{"ldoa9",1200,7}}
STATIC CONST CR_HDK_USB_PORT mPorts[]={
  {
    .Index=0,.Controller=0xa600000,.ControllerBytes=0x100000,
    .HsPhy=0x88e3000,.HsPhyBytes=0x110,.Eud=0x88e2000,
    .QmpPhy=0x88e8000,.QmpPhyBytes=0x3000,.StreamId=0,.FixedHost=FALSE,
    .VbusGpio=CR_HDK_USB_NO_GPIO,.HubGpio=CR_HDK_USB_NO_GPIO,.OrientationGpio=CR_HDK_USB_NO_GPIO,
    .IccProvider="qcom,sm8250-dwc3",.Gdsc="usb30_prim_gdsc",
    .CoreReset="gcc_usb30_prim_bcr",.HsReset="gcc_qusb2phy_prim_bcr",
    .SsReset={"gcc_usb3_dp_phy_prim_bcr","gcc_usb3_phy_prim_bcr"},
    .MemorySource=0x6000,.MemoryDestination=0x6001,.ConfigSource=0x6002,.ConfigDestination=0x6003,
    .CoreClocks={{"gcc_usb30_prim_master_clk",66666667},{"gcc_cfg_noc_usb3_prim_axi_clk",66666667},
      {"gcc_aggre_usb3_prim_axi_clk",66666667},{"gcc_usb30_prim_mock_utmi_clk",19200000},
      {"gcc_usb30_prim_sleep_clk",0},{"gcc_usb3_sec_clkref_en",0}},.CoreClockCount=6,
    .PhyClocks={{"gcc_usb3_prim_phy_aux_clk",19200000},{"gcc_usb3_prim_phy_com_aux_clk",19200000},
      {"gcc_usb3_prim_phy_pipe_clk",0}},.PhyClockCount=3,.HsPhyClockCount=0,
    .Rails=COMMON_RAILS,.RailCount=5,.HsRailCount=3,
    .HsOverrides=mHs0,.HsOverrideCount=ARRAY_SIZE(mHs0),
    .SsSequence=mSs0,.SsSequenceCount=ARRAY_SIZE(mSs0),
    .PcsReset=0x1c00,.PcsStart=0x1c44,.PcsPower=0x1c40,.PcsStatus=0x1c14,.PcsReadyMask=0x40,
  },
  {
    .Index=1,.Controller=0xa800000,.ControllerBytes=0x100000,
    .HsPhy=0x88e4000,.HsPhyBytes=0x110,.Eud=0,
    .QmpPhy=0x88eb000,.QmpPhyBytes=0x1000,.StreamId=0x20,.FixedHost=TRUE,
    .VbusGpio=22,.VbusValue=1,.HubGpio=CR_HDK_USB_NO_GPIO,.OrientationGpio=CR_HDK_USB_NO_GPIO,
    .IccProvider="qcom,sm8250-dwc3",.Gdsc="usb30_sec_gdsc",
    .CoreReset="gcc_usb30_sec_bcr",.HsReset="gcc_qusb2phy_sec_bcr",
    .SsReset={"gcc_usb3_phy_sec_bcr","gcc_usb3phy_phy_sec_bcr"},
    .MemorySource=0x6004,.MemoryDestination=0x6005,.ConfigSource=0x6006,.ConfigDestination=0x6007,
    .CoreClocks={{"gcc_usb30_sec_master_clk",66666667},{"gcc_cfg_noc_usb3_sec_axi_clk",66666667},
      {"gcc_aggre_usb3_sec_axi_clk",66666667},{"gcc_usb30_sec_mock_utmi_clk",19200000},
      {"gcc_usb30_sec_sleep_clk",0},{"gcc_usb3_sec_clkref_en",0}},.CoreClockCount=6,
    .PhyClocks={{"gcc_usb3_sec_phy_aux_clk",19200000},{"gcc_usb3_sec_phy_com_aux_clk",19200000},
      {"gcc_usb3_sec_phy_pipe_clk",0}},.PhyClockCount=3,.HsPhyClockCount=0,
    .Rails=COMMON_RAILS,.RailCount=5,.HsRailCount=3,
    .HsOverrides=mHs1,.HsOverrideCount=ARRAY_SIZE(mHs1),
    .SsSequence=mSs1,.SsSequenceCount=ARRAY_SIZE(mSs1),
    .PcsReset=0x800,.PcsStart=0x844,.PcsPower=0x840,.PcsStatus=0x814,.PcsReadyMask=0x40,
  },
};
STATIC CONST CR_HDK_USB_TARGET mTarget={0x8250,8245,mPorts,ARRAY_SIZE(mPorts)};
CONST CR_HDK_USB_TARGET *CrHdkUsbGetTarget(VOID) {return &mTarget;}
