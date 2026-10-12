/* HDK8350 USB resources from public Android R11 and Linux sm8350-hdk.
 * SPDX-License-Identifier: MIT
 * Primary USB/DP remains reserved for healthy PMIC GLINK UCSI/altmode policy.
 * Secondary cold USB2 host is board-enabled by public mainline HDK; the
 * supplied Android description keeps it disabled despite its host override.
 * GPIO42 output-low enables the board hub, not a claimed VBUS switch. PM8350
 * GPIO8/9 pinctrl definitions have no USB consumer and are not used here.
 */
#include <Library/HdkUsbTargetLib.h>
#include "CrUsbPhyGenerated.h"
STATIC CONST CR_HDK_USB_PORT mPorts[]={
  {
    .Index=0,.Controller=0x0a600000,.ControllerBytes=0xcd00,
    .HsPhy=0x088e3000,.HsPhyBytes=0x114,.Eud=0x088e2000,
    .QmpPhy=0x088e8000,.QmpPhyBytes=0x3000,.StreamId=0,.FixedHost=FALSE,
    .VbusGpio=CR_HDK_USB_NO_GPIO,.HubGpio=CR_HDK_USB_NO_GPIO,.OrientationGpio=81,
    .IccProvider="qcom,sm8350-dwc3",.Gdsc="usb30_prim_gdsc",
    .CoreReset="gcc_usb30_prim_bcr",.HsReset="gcc_qusb2phy_prim_bcr",
    .SsReset={"gcc_usb3_dp_phy_prim_bcr","gcc_usb3_phy_prim_bcr"},
    .MemorySource=0x6000,.MemoryDestination=0x6001,.ConfigSource=0x6002,.ConfigDestination=0x6003,
    .CoreClocks={{"gcc_usb30_prim_master_clk",66666667},{"gcc_cfg_noc_usb3_prim_axi_clk",66666667},
      {"gcc_aggre_usb3_prim_axi_clk",66666667},{"gcc_usb30_prim_mock_utmi_clk",19200000},
      {"gcc_usb30_prim_sleep_clk",0}},.CoreClockCount=5,
    /* RPMh XO is retained by the provider's acknowledged boot floor. */
    .PhyClocks={{"gcc_usb3_prim_phy_aux_clk",19200000},{"gcc_usb3_prim_phy_com_aux_clk",19200000},
      {"gcc_usb3_prim_phy_pipe_clk",0}},.PhyClockCount=3,.HsPhyClockCount=0,
    .Rails={{"ldob5",880,7},{"ldoc1",1800,7},{"ldob2",3072,7},{"ldob1",912,7},{"ldob6",1200,7}},
    .RailCount=5,.HsRailCount=3,.HsOverrides=mUsb0HS,.HsOverrideCount=ARRAY_SIZE(mUsb0HS),
    .SsSequence=mUsb0SS,.SsSequenceCount=ARRAY_SIZE(mUsb0SS),
    .PcsReset=0x1c00,.PcsStart=0x1c44,.PcsPower=0x1c40,.PcsStatus=0x1c14,.PcsReadyMask=64,
  },
  {
    .Index=1,.Controller=0x0a800000,.ControllerBytes=0xcd00,
    .HsPhy=0x088e4000,.HsPhyBytes=0x114,.QmpPhy=0x088eb000,.QmpPhyBytes=0x2000,
    .StreamId=0x20,.FixedHost=TRUE,.VbusGpio=CR_HDK_USB_NO_GPIO,.HubGpio=42,.HubValue=0,
    .OrientationGpio=CR_HDK_USB_NO_GPIO,.IccProvider="qcom,sm8350-dwc3",.Gdsc="usb30_sec_gdsc",
    .CoreReset="gcc_usb30_sec_bcr",.HsReset="gcc_qusb2phy_sec_bcr",
    .SsReset={"gcc_usb3_phy_sec_bcr","gcc_usb3phy_phy_sec_bcr"},
    .MemorySource=0x6004,.MemoryDestination=0x6005,.ConfigSource=0x6006,.ConfigDestination=0x6007,
    .CoreClocks={{"gcc_usb30_sec_master_clk",66666667},{"gcc_cfg_noc_usb3_sec_axi_clk",66666667},
      {"gcc_aggre_usb3_sec_axi_clk",66666667},{"gcc_usb30_sec_mock_utmi_clk",19200000},
      {"gcc_usb30_sec_sleep_clk",0},{"gcc_usb3_sec_clkref_en",0}},.CoreClockCount=6,
    .PhyClocks={{"gcc_usb3_sec_phy_aux_clk",19200000},{"gcc_usb3_sec_phy_com_aux_clk",19200000},
      {"gcc_usb3_sec_phy_pipe_clk",0}},.PhyClockCount=3,.HsPhyClockCount=0,
    .Rails={{"ldob5",880,7},{"ldoc1",1800,7},{"ldob2",3072,7},{"ldob5",880,7},{"ldob6",1200,7}},
    .RailCount=5,.HsRailCount=3,.SsSequence=mUsb1SS,.SsSequenceCount=ARRAY_SIZE(mUsb1SS),
    .PcsReset=0x200,.PcsStart=0x244,.PcsPower=0x240,.PcsStatus=0x214,.PcsReadyMask=64,
  },
};
STATIC CONST CR_HDK_USB_TARGET mTarget={0x8350,8350,mPorts,ARRAY_SIZE(mPorts)};
CONST CR_HDK_USB_TARGET *CrHdkUsbGetTarget(VOID){return &mTarget;}
