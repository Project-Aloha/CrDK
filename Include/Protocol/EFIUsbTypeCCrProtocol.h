/** Native Type-C state and asynchronous PD role requests. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <ucsi/TypeC.h>

#define EFI_USB_TYPEC_CR_PROTOCOL_REVISION 0x00010000ULL
#define EFI_USB_TYPEC_NON_PD_WITNESS_REVISION 0x00010001ULL
#define EFI_USB_TYPEC_CR_PROTOCOL_GUID \
  { 0xa6e1ac8d, 0x4945, 0x49e8, { 0x9b, 0x73, 0x65, 0xa4, 0xa0, 0x5f, 0x8f, 0x16 } }
extern EFI_GUID gEfiUsbTypeCCrProtocolGuid;
typedef struct _EFI_USB_TYPEC_CR_PROTOCOL EFI_USB_TYPEC_CR_PROTOCOL;
typedef CR_USB_TYPEC_DATA_ROLE EFI_USB_TYPEC_DATA_ROLE;
typedef CR_USB_TYPEC_ORIENTATION EFI_USB_TYPEC_ORIENTATION;
typedef CR_USB_TYPEC_STATE EFI_USB_TYPEC_STATE;

typedef EFI_STATUS (EFIAPI *EFI_USB_TYPEC_GET_STATE)(
  IN EFI_USB_TYPEC_CR_PROTOCOL *This, OUT EFI_USB_TYPEC_STATE *State);
/* Success queues a PD request. Only a subsequent observed state establishes
   the negotiated role. Firmware retains control of VBUS and CC signaling. */
typedef EFI_STATUS (EFIAPI *EFI_USB_TYPEC_SET_DATA_ROLE)(
  IN EFI_USB_TYPEC_CR_PROTOCOL *This, IN EFI_USB_TYPEC_DATA_ROLE Role);
typedef EFI_STATUS (EFIAPI *EFI_USB_TYPEC_SET_POWER_ROLE)(
  IN EFI_USB_TYPEC_CR_PROTOCOL *This, IN BOOLEAN Source);
/* Pure cached query, no BS, allocation, MMIO or SPMI transaction. IRQ,
 * unstable snapshots and disconnect invalidate the epoch. Consumers must
 * retain this exact protocol producer before caching this entry point. */
typedef EFI_STATUS (EFIAPI *EFI_USB_TYPEC_GET_NON_PD_WITNESS)(
  IN EFI_USB_TYPEC_CR_PROTOCOL *This, OUT CR_TYPEC_NON_PD_HOST_WITNESS *Witness);

struct _EFI_USB_TYPEC_CR_PROTOCOL {
  UINT64 Revision;
  EFI_USB_TYPEC_GET_STATE GetState;
  EFI_USB_TYPEC_SET_DATA_ROLE SetDataRole;
  EFI_USB_TYPEC_SET_POWER_ROLE SetPowerRole;
  EFI_USB_TYPEC_GET_NON_PD_WITNESS GetNonPdHostWitness;
};
