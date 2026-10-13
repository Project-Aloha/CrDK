/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssPlatformLib.h>
#define BIT_(n) (1U << (n))
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
CR_STATUS CrMdssPlatformEnablePhy(CR_MDSS_PLATFORM *P);
CR_STATUS CrMdssPlatformDisablePhy(CR_MDSS_PLATFORM *P);
CR_STATUS CrMdssPlatformBridgePrepare(CR_MDSS_PLATFORM *P);
CR_STATUS CrMdssPlatformBridgeUnprepare(CR_MDSS_PLATFORM *P);
CR_STATUS CrMdssPlatformPoll(CR_MDSS_PLATFORM *P, CR_MDSS_PLATFORM_WINDOW Window,
                            UINT32 Offset, UINT32 Mask, UINT32 Expected, UINT32 Timeout);
