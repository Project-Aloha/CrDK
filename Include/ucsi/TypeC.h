/** OS-independent observed Type-C state. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

typedef enum { UsbTypeCNone, UsbTypeCHost, UsbTypeCDevice } CR_USB_TYPEC_DATA_ROLE;
typedef enum { UsbTypeCUnoriented, UsbTypeCNormal, UsbTypeCReverse } CR_USB_TYPEC_ORIENTATION;
typedef struct {
  UINT32 Sequence;
  UINT32 HpdIrqSequence;
  BOOLEAN PdReady, Connected, PowerSource;
  CR_USB_TYPEC_DATA_ROLE DataRole;
  CR_USB_TYPEC_ORIENTATION Orientation;
  BOOLEAN DpActive, Usb3, Hpd;
  UINT8 PinAssignment; /* PMIC DPAM_C/D/E/F = 3/4/5/6 */
  UINT8 DpLanes;
  CR_STATUS LastError;
} CR_USB_TYPEC_STATE;
