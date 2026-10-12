/** Optional inherited SM8550 debug UART7; the board chooses its admission.
 * SPDX-License-Identifier: MIT
 */
#include <Library/CrTargetDebugUartLib.h>
#include <Library/PcdLib.h>

STATIC CrDebugUartContext mUart = {
  .BaseAddress = FixedPcdGet32 (PcdUartSerialBase),
  .InterruptConfig = {
    .InterruptNumber = GIC_SPI (579),
    .TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH,
  },
  // This provider describes an already configured console, never cold init.
  .IsDebugUart = TRUE,
  .Type = DEBUG_UART_TYPE_GENI,
  .BaudRate = 115200,
};

CrDebugUartContext *CrTargetGetDebugUartContext (VOID)
{
  // UART14/15 are normal GENI endpoints and need clock/ICC admission first.
  return FixedPcdGet32 (PcdUartSerialBase) == 0x00a9c000 ? &mUart : NULL;
}
