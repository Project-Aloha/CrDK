/* Native USB retirement publication. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/UsbHandoffCrLib.h>
#include <Protocol/EFISmmuCrProtocol.h>

/* Called on the BSP at TPL_NOTIFY before SMMU revocation. Closes every native
   writer and supplies actual stop/IRQ observations without allocation/free. */
typedef EFI_STATUS (*CR_USB_NATIVE_FREEZE)(CR_USB_NATIVE_HANDOFF *Record);
EFI_STATUS CrUsbNativeInit(EFI_SMMU_CR_PROTOCOL *,CR_USB_NATIVE_FREEZE);
EFI_STATUS CrUsbNativeRelease(VOID);
VOID CrUsbNativeExit(VOID);
