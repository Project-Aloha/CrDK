/** Pure charger UI snapshot; no RPC from the app polling path. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/BatteryChargerCrLib.h>
#define EFI_BATTERY_CHARGER_CR_PROTOCOL_REVISION 0x00010000ULL
#define EFI_BATTERY_CHARGER_CR_PROTOCOL_GUID \
  { 0x4bf1b04c, 0xe908, 0x4675, { 0x91, 0x2d, 0xe6, 0xf0, 0x49, 0x6c, 0x57, 0x51 } }
extern EFI_GUID gEfiBatteryChargerCrProtocolGuid;
typedef struct _EFI_BATTERY_CHARGER_CR_PROTOCOL EFI_BATTERY_CHARGER_CR_PROTOCOL;
/* Always copies current diagnostics into Out for valid arguments. SUCCESS
 * means current SOC is valid; NOT_READY permits UI to show unavailable state.
 * A valid field is usable only if its own ValidMask bit is present. <=TPL_CALLBACK. */
typedef EFI_STATUS (EFIAPI *EFI_BATTERY_CHARGER_GET_SNAPSHOT)(
  EFI_BATTERY_CHARGER_CR_PROTOCOL *This, CR_BATTERY_CHARGER_SNAPSHOT *Out);
struct _EFI_BATTERY_CHARGER_CR_PROTOCOL {
  UINT64 Revision;
  EFI_BATTERY_CHARGER_GET_SNAPSHOT GetSnapshot;
};
