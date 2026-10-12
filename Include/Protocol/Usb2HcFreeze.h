/** @file
  Optional BSP-only, one-way software retirement of a USB host controller.

  Freeze closes every public transfer/reset/port path and the poll callback.
  It neither halts hardware nor revokes DMA. The platform caller must do both,
  with interrupt masking, before treating the controller as retired. Once
  frozen, the ordinary host EBS callback makes no further hardware changes;
  controller resources and PCI attributes remain available to that caller.
  No protocol, event or DMA allocation is freed by Freeze. Repeated calls report
  remaining in-flight callbacks; success means the count has reached zero.
  It is legal only during permanent EBS preparation, at TPL <= TPL_NOTIFY.
  Failed EBS does not reopen this controller. The interface is not runtime.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef EDKII_USB2_HC_FREEZE_H_
#define EDKII_USB2_HC_FREEZE_H_

#include <Uefi.h>
#include <Protocol/Usb2HostController.h>

#define EDKII_USB2_HC_FREEZE_PROTOCOL_GUID \
  { 0x67d217aa, 0x3d49, 0x4b80, { 0x81, 0xe2, 0xcb, 0xbd, 0xd9, 0x50, 0x4f, 0xe7 } }
#define EDKII_USB2_HC_FREEZE_PROTOCOL_REVISION  1ULL

typedef struct _EDKII_USB2_HC_FREEZE_PROTOCOL EDKII_USB2_HC_FREEZE_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EDKII_USB2_HC_FREEZE)(
  IN EDKII_USB2_HC_FREEZE_PROTOCOL *This,
  OUT UINT32 *ActiveCalls
  );

struct _EDKII_USB2_HC_FREEZE_PROTOCOL {
  UINT64                  Revision;
  EFI_USB2_HC_PROTOCOL     *HostController;
  EDKII_USB2_HC_FREEZE      Freeze;
};

extern EFI_GUID gEdkiiUsb2HcFreezeProtocolGuid;
#endif
