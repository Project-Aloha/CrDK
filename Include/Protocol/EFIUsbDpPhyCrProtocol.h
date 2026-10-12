/** Shared HDK8450 USB/DP PHY ownership. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Protocol/EFIUsbTypeCCrProtocol.h>

#define EFI_USB_DP_PHY_CR_PROTOCOL_REVISION 0x00010000ULL
#define EFI_USB_DP_PHY_CR_PROTOCOL_GUID \
  { 0xe3e74539, 0xcb26, 0x4631, { 0xa0, 0xa8, 0x43, 0x1a, 0x1e, 0xcd, 0x14, 0x97 } }
extern EFI_GUID gEfiUsbDpPhyCrProtocolGuid;
typedef struct _EFI_USB_DP_PHY_CR_PROTOCOL EFI_USB_DP_PHY_CR_PROTOCOL;
typedef enum { UsbDpPhyUsb, UsbDpPhyDp, UsbDpPhyClientCount } EFI_USB_DP_PHY_CLIENT;

/* One lease per consumer. Owned can be TRUE on failure; Release must then be
   retried until success. Changing orientation/lane allocation is rejected
   while either lease remains. The caller must stop its DMA before Release.
   State is the observed UCSI snapshot, never an unconfirmed role request. */
typedef EFI_STATUS (EFIAPI *EFI_USB_DP_PHY_ACQUIRE)(
  IN EFI_USB_DP_PHY_CR_PROTOCOL *This, IN EFI_USB_DP_PHY_CLIENT Client,
  IN CONST EFI_USB_TYPEC_STATE *State, IN BOOLEAN SuperSpeed, OUT BOOLEAN *Owned);
typedef EFI_STATUS (EFIAPI *EFI_USB_DP_PHY_RELEASE)(
  IN EFI_USB_DP_PHY_CR_PROTOCOL *This, IN EFI_USB_DP_PHY_CLIENT Client);
/* DP lease initially enables AUX. Link power is independent of USB PCS. */
typedef EFI_STATUS (EFIAPI *EFI_USB_DP_PHY_POWER_DP)(
  IN EFI_USB_DP_PHY_CR_PROTOCOL *This, IN UINT32 RateMbps, IN UINT8 Lanes);
/* This PHY uses the maximum requested voltage/pre-emphasis across its lanes. */
typedef EFI_STATUS (EFIAPI *EFI_USB_DP_PHY_SET_DP_LEVELS)(
  IN EFI_USB_DP_PHY_CR_PROTOCOL *This, IN UINT8 Voltage, IN UINT8 PreEmphasis);
struct _EFI_USB_DP_PHY_CR_PROTOCOL {
  UINT64 Revision;
  EFI_USB_DP_PHY_ACQUIRE Acquire;
  EFI_USB_DP_PHY_RELEASE Release;
  EFI_USB_DP_PHY_POWER_DP PowerDp;
  EFI_USB_DP_PHY_SET_DP_LEVELS SetDpLevels;
};
