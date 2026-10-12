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

/* Direct PM8150B cached snapshot. This proves only a stable, already-powered
 * non-PD USB2 source attachment; it is not negotiated PD/DP admission. */
#define CR_TYPEC_NON_PD_WITNESS_REVISION 1U
#define CR_TYPEC_WITNESS_VALID 1U
#define CR_TYPEC_WITNESS_ATTACHED 2U
#define CR_TYPEC_WITNESS_SOURCE 4U
#define CR_TYPEC_WITNESS_VBUS_DETECT 8U
#define CR_TYPEC_WITNESS_VSAFE5V 16U
#define CR_TYPEC_WITNESS_PD_DISABLED 32U
#define CR_TYPEC_WITNESS_HOST 64U
#define CR_TYPEC_WITNESS_REVERSE 128U
#define CR_TYPEC_WITNESS_REQUIRED 127U
typedef struct {UINT32 Revision,Board,Epoch,Flags;} CR_TYPEC_NON_PD_HOST_WITNESS;
