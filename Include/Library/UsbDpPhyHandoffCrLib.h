/* Pure SM8450-HDK joint native PHY qualification. GPL-2.0-only */
#pragma once
#include <Library/UsbHandoffCrLib.h>
#include <Library/MdssHandoffCrLib.h>
#include <Library/UsbDpPhyCrLib.h>

typedef struct {
  UINT64 UsbBootGeneration, UsbTransferId, MdssGeneration, MdssTransferId;
  UINT32 ClockMask, RailMask;
  /* Slots use the exact UsbDpPhyCrLib Init policy order. Unselected slots
     remain zero; selected values are copied from the immutable native record. */
  CR_MDSS_NATIVE_CLOCK Clocks[CR_USB_DP_PHY_CLOCKS];
  CR_MDSS_NATIVE_RAIL Rails[CR_USB_DP_PHY_RAILS];
} CR_USB_DP_PHY_NATIVE_DEMANDS;

/* Inputs are already independently snapshotted immutable current-boot USBH
   and MDSH observations. Their locator/reserved-memory/producer-source
   validation is the caller's responsibility. The two producer generations
   are independent and must not be compared with each other.

   This validates actual native closure, USB halt/revocation, stopped/absent
   DP and the exact known native PHY journal. It does not acquire a Windows
   assignment, mapping, reset owner, dependency D0, SPB or policy grant. The
   caller must retain/apply the selected native demands under a distinct
   permanent owner before its first service vote/reset. No ReleaseOwner is
   authorized by this evidence. All inputs/output must be disjoint ordinary
   storage. Failure leaves Out unchanged; no allocation, IO or callbacks. */
CR_STATUS CrUsbDpPhyHandoffQualify(CONST CR_USB_NATIVE_HANDOFF *Usb,
    UINT64 ExpectedUsbBootGeneration, CONST CR_MDSS_NATIVE_HANDOFF *Mdss,
    CR_USB_DP_PHY_NATIVE_DEMANDS *Out);
