/* HDK8250 QUP0 SE1 resource facts. SPDX-License-Identifier: MIT
 * Linux sm8250.dtsi at a53261b60834ce617cc4f3c7723dda8285c59789;
 * Android kona-hdk.dtsi at cad09e061ef6cf689a4a1e54d27562e0f042236a.
 * FIFO buses cover legacy SE1 and HDK8245 FocalTech SE13. No DMA grant.
 */
#include <Library/CrTargetQupLib.h>

STATIC CONST CR_TARGET_QUP_ICC_PATH mSe1Paths[] = {
  {"qcom,sm8250-qup0",0x5100,0x5101,1},
  {"qcom,sm8250-qup0",0x5102,0x5103,1},
  {"qcom,sm8250-qup0",0x5104,0x5105,1000},
};
STATIC CONST CR_TARGET_QUP_ICC_PATH mSe13Paths[] = {
  {"qcom,sm8250-qup1",0x5300,0x5301,1},
  {"qcom,sm8250-qup1",0x5302,0x5303,1},
  {"qcom,sm8250-qup1",0x5304,0x5305,1000},
};
STATIC CONST CR_TARGET_QUP_WRAPPER mQup[] = {{
  .Id=0,.Base=0x009c0000,.Size=0x6000,.SerialEngineCount=8,
  .SerialEngineBase={0x00980000,0x00984000,0x00988000,0x0098c000,
                     0x00990000,0x00994000,0x00998000,0x0099c000},
  .SerialEngineSize=0x4000,.IommuStreamId=0x5a3,
  .SerialEngineInterconnect={NULL,mSe1Paths},
},{
  .Id=1,.Base=0x00ac0000,.Size=0x6000,.SerialEngineCount=6,
  .SerialEngineBase={0x00a80000,0x00a84000,0x00a88000,0x00a8c000,0x00a90000,0x00a94000},
  .SerialEngineSize=0x4000,.IommuStreamId=0x43,
  .SerialEngineInterconnect={[5]=mSe13Paths},
}};
/* Both Linux qup_i2c1_default and Android qupv3_se1_i2c_active select
 * GPIO4/5 function1, bias-disable and 2mA. 100kHz is the conservative native
 * default; callers may select the separately supported 400kHz/1MHz timings. */
STATIC CONST CR_TARGET_QUP_I2C_ENDPOINT mI2c[] = {{
  .Instance=2,.Wrapper=0,.Engine=1,.Base=0x00984000,.Size=0x4000,
  .Pins={{4,GPIO_FUNC_1,GPIO_PULL_NONE,GPIO_DRIVE_STRENGTH_2MA},
         {5,GPIO_FUNC_1,GPIO_PULL_NONE,GPIO_DRIVE_STRENGTH_2MA}},
  .ClockController="gcc",
  .Clocks={"gcc_qupv3_wrap_0_m_ahb_clk","gcc_qupv3_wrap_0_s_ahb_clk",
           "gcc_qupv3_wrap0_s1_clk"},
  .SourceClockHz=19200000,.DefaultFrequencyKHz=100,
},{
  .Instance=14,.Wrapper=1,.Engine=5,.Base=0x00a94000,.Size=0x4000,
  .Pins={{36,GPIO_FUNC_1,GPIO_PULL_NONE,GPIO_DRIVE_STRENGTH_2MA},
         {37,GPIO_FUNC_1,GPIO_PULL_NONE,GPIO_DRIVE_STRENGTH_2MA}},
  .ClockController="gcc",
  .Clocks={"gcc_qupv3_wrap_1_m_ahb_clk","gcc_qupv3_wrap_1_s_ahb_clk",
           "gcc_qupv3_wrap1_s5_clk"},
  .SourceClockHz=19200000,.DefaultFrequencyKHz=100,
}};
/* SM8250 GPI0 has thirteen interrupts and a 0x70000 window. The current
 * generic target ABI has twelve slots, so never truncate that inventory or
 * substitute SM8450's twelve-IRQ/0x60000 descriptor. DMA remains absent. */
STATIC CR_TARGET_QUP_CONTEXT mQupContext={
  .Qup=mQup,.QupCount=ARRAY_SIZE(mQup),.I2c=mI2c,.I2cCount=ARRAY_SIZE(mI2c)
};
CR_TARGET_QUP_CONTEXT *CrTargetGetQupContext(VOID) {return &mQupContext;}
