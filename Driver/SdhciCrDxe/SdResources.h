/* Native SDCC2 exact resource ownership. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/SdhciMsmLib.h>
EFI_STATUS CrSdResourcesAcquire(CR_SDHC *Controller);
EFI_STATUS CrSdResourcesReady(VOID);
EFI_STATUS CrSdResourcesSetClock(UINT32 Hz);
EFI_STATUS CrSdResourcesCardPresent(BOOLEAN *Present);
EFI_STATUS CrSdResourcesServicePower(CR_SDHC *Controller);
/* Returns only after a new observed matching BUS cause and successful ACK,
 * or an independently observed unchanged previously acknowledged state. */
EFI_STATUS CrSdResourcesRequestPower(CR_SDHC *Controller,BOOLEAN On);
EFI_STATUS CrSdResourcesRelease(CR_SDHC *Controller);
/* Closes IRQ/deferred admission without withdrawing prerequisites. Ready
 * remains valid for the adapter's synchronous final host reset/clock stop. */
EFI_STATUS CrSdResourcesFreeze(CR_SDHC *Controller);
