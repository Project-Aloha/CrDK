/* Exact SM8450 HDK SDCC2 pad transport. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#define WDF_GPIO_SD_INTERFACE_REVISION 2U
DEFINE_GUID(GUID_GPIO_SD_INTERFACE,0x2183e119,0xfaa2,0x43df,0x98,0x1e,0x61,0xa8,0x2a,0xb2,0xb7,0xcd);
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Acquire)(PVOID Context,PVOID StableOwner);
  NTSTATUS (*SetActive)(PVOID Context,PVOID StableOwner,BOOLEAN Active);
  NTSTATUS (*ReadCardDetect)(PVOID Context,PVOID StableOwner,BOOLEAN *PhysicalLevel);
  NTSTATUS (*Release)(PVOID Context,PVOID StableOwner);
} WDF_GPIO_SD_INTERFACE;
/* Revision2 owns ONLY SDCC2 special pads211/212/213. Acquire/SetActive/Release
 * are PASSIVE. ReadCardDetect's retained ABI slot returns NOT_SUPPORTED; card
 * detect must use the actual shared GpioClx input connection/interrupt.
 * Acquire fences before snapshot; ANY failed Acquire must pair with Release.
 * Failed writes/restore retain original baseline and acquired pad fences.
 * Hold the queried reference and provider D0 through successful Release.
 * No CD92 ownership, IRQ route or implicit host/DMA ownership is granted. */
