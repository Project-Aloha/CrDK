/* Qualified HDK USB2 fixed-host cold-init journal. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/HdkUsbTargetLib.h>

typedef enum {HdkUsbController,HdkUsbHs,HdkUsbEud,HdkUsbWindows} CR_HDK_USB_WINDOW;
typedef struct {
  VOID *Context;
  EFI_STATUS (*Access)(VOID *);
  /* Provider acknowledges and retains its permanent CXO3/CX/MX boot floor. */
  EFI_STATUS (*Floor)(VOID *);
  EFI_STATUS (*Read)(VOID *,CR_HDK_USB_WINDOW,UINT32,UINT32 *);
  EFI_STATUS (*Write)(VOID *,CR_HDK_USB_WINDOW,UINT32,UINT32);
  VOID (*Delay)(VOID *,UINT32);
  EFI_STATUS (*Path)(VOID *,UINT32,BOOLEAN,BOOLEAN *);
  EFI_STATUS (*Domain)(VOID *,BOOLEAN,BOOLEAN *);
  EFI_STATUS (*Clock)(VOID *,CONST CHAR8 *,UINT64,BOOLEAN,BOOLEAN *);
  EFI_STATUS (*Rail)(VOID *,CONST CR_HDK_USB_RAIL *,BOOLEAN,BOOLEAN *);
  EFI_STATUS (*Reset)(VOID *,CONST CHAR8 *,BOOLEAN);
  EFI_STATUS (*Gpio)(VOID *,UINT32,BOOLEAN,BOOLEAN *);
  EFI_STATUS (*Attach)(VOID *);
  /* Required for primary Type-C: exact retained producer, pure epoch witness. */
  EFI_STATUS (*AdmitHost)(VOID *);
} CR_HDK_USB_IO;
typedef struct {
  CONST CR_HDK_USB_TARGET *Target;
  CONST CR_HDK_USB_PORT *Port;
  CR_HDK_USB_IO Io;
  BOOLEAN Initialized,Ready,Closed,Touched,HsTouched,ResetTouched,Domain,UncertainOwner;
  BOOLEAN Clocks[CR_HDK_USB_CLOCKS],PhyClocks[CR_HDK_USB_CLOCKS];
  BOOLEAN Rails[CR_HDK_USB_RAILS],Paths[2],Gpios[2];
  UINT32 Stage,LastOffset,LastValue,Id,Revision,VersionType;
  CR_HDK_USB_WINDOW LastWindow;
  EFI_STATUS LastStatus;
} CR_HDK_USB;

/* Permanent zeroed storage, exclusive caller serialization, no callback
 * reentry. All register views and providers stay live while any ownership
 * remains. Acquisition callbacks must report retained ownership on failure.
 * Only trusted board-selected USB1 fixed-host profiles are admitted. This
 * core does not publish a child, claim an inherited PD engine or supply VBUS
 * through session-valid override bits. Start configures USB2 only, with the
 * SS PIPE clock replaced by the public QSCRATCH UTMI sequence.
 */
EFI_STATUS CrHdkUsbInit(CR_HDK_USB *,CONST CR_HDK_USB_TARGET *,UINT32,CONST CR_HDK_USB_IO *);
EFI_STATUS CrHdkUsbStart(CR_HDK_USB *);
/* Caller has first stopped/disconnected every child. Failure retains all
 * remaining prerequisites. No shared PHY/DP or unrelated stream reset. */
EFI_STATUS CrHdkUsbRelease(CR_HDK_USB *);
/* One-way EBS close after exact child Freeze and drained-writer witness.
 * Stops xHCI and disables interrupts, preserving rings, mappings and power. */
EFI_STATUS CrHdkUsbFreeze(CR_HDK_USB *);
/* Exact read-only witness after cold wrapper reset and before child admission;
 * checks both xHCI and dormant function writer/DMA controls. */
EFI_STATUS CrHdkUsbVerifyStopped(CR_HDK_USB *);
