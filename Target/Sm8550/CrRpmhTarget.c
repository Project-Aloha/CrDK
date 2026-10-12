/** SM8550 apps RSC DRV2 facts from public Linux sm8550.dtsi.
 * SPDX-License-Identifier: MIT
 */
#include <Library/CrTargetRpmhLib.h>

STATIC RpmhDeviceContext mRpmh = {
  .drv_base_address = 0x17a20000,
  .drv_size = 0x10000,
  .drv_id = 2,
  .tcs_offset = 0xd00,
  .InterruptConfig = {
    .InterruptNumber = GIC_SPI (5),
    .TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH,
  },
  .tcs_config = {
    .active_tcs = {.tcs_count = 3},
    .sleep_tcs = {.tcs_count = 2},
    .wake_tcs = {.tcs_count = 2},
    .control_tcs = {.tcs_count = 0},
  },
};

RpmhDeviceContext *CrTargetGetRpmhContext (VOID) { return &mRpmh; }
