/* Staged, pointer-free USB retirement observations. SPDX-License-Identifier: MIT
 * No native producer or Windows resource/DMA grant is installed by this header. */
#pragma once
#include <oskal/cr_types.h>
#include <stddef.h>
#include <Library/smmu.h>

#define CR_USBH_SIGNATURE 0x48425355U /* USBH */
#define CR_USBH_REVISION 1U
#define CR_USBH_UNCERTAIN 1U
#define CR_USBH_RETIRED 2U
#define CR_USBH_HOST 1U
#define CR_USBH_FUNCTION 2U
#define CR_USBH_MONITOR_WRITER 1U
#define CR_USBH_XHC_WRITER 2U
#define CR_USBH_FUNCTION_WRITER 4U
#define CR_USBH_ALL_WRITERS 7U
#define CR_USBH_CONTROLLER 0x0a600000ULL
#define CR_USBH_CONTROLLER_BYTES 0xfc100U
#define CR_USBH_QSCRATCH 0x0a6f8800ULL
#define CR_USBH_QSCRATCH_BYTES 0x400U

typedef struct {
  UINT32 Role, Id, Revision, GlobalControl;
  UINT32 HostCommand, HostStatus;
  UINT32 DeviceControl, DeviceStatus, DeviceEvents, DeviceEndpoints;
  UINT32 EventBuffers, MaskedEventBuffers;
} CR_USB_CONTROLLER_STOP;

typedef struct {
  UINT32 Signature, Revision, Length, Crc32;
  UINT64 Sequence, BootGeneration, TransferId;
  UINT32 Soc, Board;
  UINT64 Controller, Qscratch;
  UINT32 ControllerBytes, QscratchBytes, Sid, Reserved0;
  UINT32 ExpectedWriters, FrozenWriters, DrainedWriters, FailedWriters;
  UINT32 ActiveCalls[3], State;
  /* Exact zero is successful. Preserve raw native status, including warnings.
   * A callback having run does not populate a successful observation. */
  UINT64 FreezeStatus, HaltStatus, InterruptStatus, RevocationStatus;
  CR_USB_CONTROLLER_STOP Stop;
  /* Successful full-domain query after SmmuBlock/TLBI, SID0. This conveys
   * firmware revocation evidence only, never ownership of its OS tables. */
  SmmuMappingEvidence Revoked;
} CR_USB_NATIVE_HANDOFF;

CR_STATIC_ASSERT(sizeof(CR_USB_CONTROLLER_STOP)==48,"USB stop wire size");
CR_STATIC_ASSERT(sizeof(CR_USB_NATIVE_HANDOFF)==320,"USB handoff wire size");
CR_STATIC_ASSERT(offsetof(CR_USB_NATIVE_HANDOFF,Sequence)==16,"USB sequence offset");
CR_STATIC_ASSERT(offsetof(CR_USB_NATIVE_HANDOFF,Stop)==144,"USB stop offset");
CR_STATIC_ASSERT(offsetof(CR_USB_NATIVE_HANDOFF,Revoked)==192,"USB SMMU offset");
