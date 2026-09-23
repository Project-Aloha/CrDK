/** @file
 *  Host-test delay override.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_types.h>

VOID
CrTestSleep(IN UINT64 Microseconds);

#define cr_sleep(Microseconds) CrTestSleep(Microseconds)
