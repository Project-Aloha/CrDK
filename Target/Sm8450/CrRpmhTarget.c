#include <Library/CrTargetRpmhLib.h>
#include <oskal/common.h>

// Reference to device tree.

// Rpmh platform specific Target Information
STATIC RpmhDeviceContext RpmhContext = {
    .drv_base_address = 0x17a20000,
    .drv_id           = 2,
    .tcs_offset       = 0xd00,
    .InterruptConfig =
        {
            .InterruptNumber = GIC_SPI(5),
            .TriggerType     = CR_INTERRUPT_TRIGGER_LEVEL_HIGH,
        },
    .drv_registers = NULL,
    .tcs_config =
        {.active_tcs  = {.tcs_count = 3},
         .sleep_tcs   = {.tcs_count = 2},
         .wake_tcs    = {.tcs_count = 2},
         .control_tcs = {.tcs_count = 0}},
};

RpmhDeviceContext *CrTargetGetRpmhContext(VOID) { return &RpmhContext; }
