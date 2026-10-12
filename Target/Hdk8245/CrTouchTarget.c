/* QCS8245IoTHDK supplied Android DTS + public Qualcomm FTS firmware.
 * DTS: focaltech@38 / SE13, gpio38 reset, gpio39 IRQ, 1080x2340/max5.
 * Android qcom SM8250 a9d9833cb5121c979b503d612b50595f1b5a28a7:
 * FTS config FT3518, firmware ID54/52. This is a supported-ID qualification,
 * not a claim that an unobserved physical device returned this pair.
 * SPDX-License-Identifier: MIT */
#include <Library/CrTargetTouchLib.h>
STATIC CONST CR_TARGET_TOUCH_CONTEXT mTouch={
 .Revision=1,.Size=sizeof(CR_TARGET_TOUCH_CONTEXT),.I2cInstance=14,
 .Address=0x38,.MaxContacts=5,.ChipIdHigh=0x54,.ChipIdLow=0x52,
 .XMax=1080,.YMax=2340,.PollPeriodUs=16000,
 .Reset={38,TRUE,GPIO_FUNC_NORMAL,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_LOW},
 .Interrupt={39,FALSE,GPIO_FUNC_NORMAL,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_HIGH},
 /* Native PMIC5 LDO HPM=7. Android init-mode enumeration is not reused. */
 .Rails={{"ldoa13",3000,7},{"ldoc1",1800,7}}
};
CONST CR_TARGET_TOUCH_CONTEXT *CrTargetGetTouchContext(VOID){return &mTouch;}
