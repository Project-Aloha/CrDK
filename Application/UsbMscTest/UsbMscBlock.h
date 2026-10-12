/* Automatic UFS LUN0 and SD BlockIo backends. SPDX-License-Identifier: MIT */
#pragma once
#include <Protocol/BlockIo.h>
#include <Library/UsbMscCrLib.h>

#define MSC_DISK_MAX 2U
typedef struct {
  EFI_HANDLE Handle, Agent;
  EFI_BLOCK_IO_PROTOCOL *Io;
  EFI_BLOCK_IO_MEDIA Media;
  VOID *Allocation, *Buffer;
  BOOLEAN Changed, Opened, Reconnect;
  CHAR16 Serial[17];
} MSC_BLOCK;

/* Blocks/Backends each have MSC_DISK_MAX entries; outputs are initialized even
 * on failure. Caller must release both entries (reverse order) after any call.
 * UFS hardware LUN0 is mandatory; a present SD card becomes USB LUN1. */
EFI_STATUS MscAttachBlocks(EFI_HANDLE Image, MSC_BLOCK *Blocks,
                           CR_USB_MSC_BLOCK_BACKEND *Backends, UINTN *Count);
EFI_STATUS MscReleaseBlock(MSC_BLOCK *Block);
