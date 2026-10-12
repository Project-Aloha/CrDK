/* SM8350 QUP2 FIFO I2C resources. SPDX-License-Identifier: MIT
 * Linux sm8350.dtsi a53261b60834ce617cc4f3c7723dda8285c59789;
 * public Android lahaina-qupv3/pinctrl/hdk.dtsi
 * 13ca08fdf0979fdd61d5e8991661874bb2d19150.
 * Physical SE15 is wrapper2 engine1. No GPI or DMA grant is exposed.
 */
#include <Library/CrTargetQupLib.h>

STATIC CONST CR_TARGET_QUP_ICC_PATH mSe15Paths[] = {
  {"qcom,sm8350-qup2",0x5400,0x5401,1},
  {"qcom,sm8350-qup2",0x5402,0x5403,1},
  {"qcom,sm8350-qup2",0x5404,0x5405,1000},
};
STATIC CONST CR_TARGET_QUP_WRAPPER mQup[] = {{
  .Id=2,.Base=0x008c0000,.Size=0x6000,.SerialEngineCount=6,
  .SerialEngineBase={0x00880000,0x00884000,0x00888000,
                     0x0088c000,0x00890000,0x00894000},
  .SerialEngineSize=0x4000,.IommuStreamId=0x5e3,
  .SerialEngineInterconnect={[1]=mSe15Paths},
}};
/* The active Android board requires pull-up; generic upstream SoC pinctrl
 * uses bias-disable. Follow the board's GPIO60/61 function1/2mA contract. */
STATIC CONST CR_TARGET_QUP_I2C_ENDPOINT mI2c[] = {{
  .Instance=15,.Wrapper=2,.Engine=1,.Base=0x00884000,.Size=0x4000,
  .Pins={{60,GPIO_FUNC_1,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_2MA},
         {61,GPIO_FUNC_1,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_2MA}},
  .ClockController="gcc",
  .Clocks={"gcc_qupv3_wrap_2_m_ahb_clk","gcc_qupv3_wrap_2_s_ahb_clk",
           "gcc_qupv3_wrap2_s1_clk"},
  .SourceClockHz=19200000,.DefaultFrequencyKHz=100,
}};
STATIC CR_TARGET_QUP_CONTEXT mQupContext={
  .Qup=mQup,.QupCount=ARRAY_SIZE(mQup),.I2c=mI2c,.I2cCount=ARRAY_SIZE(mI2c),
};
CR_TARGET_QUP_CONTEXT *CrTargetGetQupContext(VOID) {return &mQupContext;}
