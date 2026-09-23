#pragma once

#include <Library/gpio.h>
#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

GpioDeviceContext *CrTargetGetGpioContext (VOID);
