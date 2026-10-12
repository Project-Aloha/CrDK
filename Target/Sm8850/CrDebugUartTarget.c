/** QRD uart7: kaanapali{,-qrd}.dts. SPDX-License-Identifier: MIT */
#include <Library/CrTargetDebugUartLib.h>
STATIC CrDebugUartContext mUart={
 .BaseAddress=0x00a9c000,
 .InterruptConfig={.InterruptNumber=GIC_SPI(835),.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
 .IsDebugUart=TRUE,.Type=DEBUG_UART_TYPE_GENI,.BaudRate=115200,
};
CrDebugUartContext *CrTargetGetDebugUartContext(VOID){return &mUart;}
