/* Shared SM8450-HDK QMP combo/HS/FSA4480 ownership. GPL-2.0-only */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#include <oskal/cr_mutex.h>
#define CR_USB_DP_PHY_REVISION 1U
#define CR_USB_DP_PHY_SOC 0x8450U
#define CR_USB_DP_PHY_HDK 1U
#define CR_USB_DP_PHY_CLOCKS 4U
#define CR_USB_DP_PHY_RAILS 6U
#define CR_USB_DP_PHY_QMP_BYTES 0x2b00U
#define CR_USB_DP_PHY_HS_BYTES 0xa4U
/* The two windows are separate assigned translated aliases, never discovered
   physical addresses. A single provider owns them for both clients. */
typedef enum {CrUsbDpPhyQmp,CrUsbDpPhyHs,CrUsbDpPhyWindows} CR_USB_DP_PHY_WINDOW;
typedef enum {CrUsbDpPhyUsb,CrUsbDpPhyDp,CrUsbDpPhyClients} CR_USB_DP_PHY_CLIENT;
typedef struct {UINTN Address;UINT32 Bytes;} CR_USB_DP_PHY_VIEW;
typedef struct {UINT32 Revision,Soc,Board;CR_USB_DP_PHY_VIEW View[CrUsbDpPhyWindows];} CR_USB_DP_PHY_CONFIG;
typedef struct {
 UINT8 Orientation; /* 1 normal,2 reverse; observed UCSI values */
 UINT8 DpLanes,PinAssignment;
 BOOLEAN PdReady,Connected,DpActive,Usb3;
 CR_STATUS LastError;
} CR_USB_DP_PHY_ROUTE;
typedef struct {
 VOID *Context;
 /* Pure current mapped/provider lifetime health, not descriptors=>ready.
    Every callback operates on these exact retained dependencies. */
 CR_STATUS (*Ready)(VOID *);
 CR_STATUS (*Read32)(VOID *,CR_USB_DP_PHY_WINDOW,UINT32,UINT32 *);
 CR_STATUS (*Write32)(VOID *,CR_USB_DP_PHY_WINDOW,UINT32,UINT32);
 /* On commits the exact stable provider owner (including partial failure)
    through Owned. Off receives TRUE; only exact success + FALSE retires it.
    All operation callbacks return exact CR_SUCCESS for completed work. A
    malformed successful On with Owned=FALSE retains an uncertain private
    token (NOT_READY); Off must therefore be idempotent for that exact private
    owner and never withdraw a different owner's reference. */
 CR_STATUS (*Clock)(VOID *,CONST CHAR8 *,UINT32,BOOLEAN,BOOLEAN *Owned);
 CR_STATUS (*Rail)(VOID *,CONST CHAR8 *,UINT32,UINT8,BOOLEAN,BOOLEAN *Owned);
 CR_STATUS (*Reset)(VOID *,CONST CHAR8 *,BOOLEAN);
 /* Source-qualified FSA4480@0x42,standard100kHz. A read is an atomic SPB
    write-register/read-byte sequence, no stop between its two operations. */
 CR_STATUS (*SwitchRead)(VOID *,UINT8,UINT8 *);
 /* Dispatched stays FALSE for local bus preparation failure; it becomes
    TRUE before StartRequest, including a subsequently failed SPB transfer.
    This preserves probe-only cleanup without granting a safe switch state. */
 CR_STATUS (*SwitchWrite)(VOID *,UINT8,UINT8,BOOLEAN *Dispatched);
} CR_USB_DP_PHY_IO;
typedef struct {CONST CHAR8 *Name;UINT32 Rate;BOOLEAN Owned;CR_STATUS Status;} CR_USB_DP_PHY_CLOCK;
typedef struct {CONST CHAR8 *Name;UINT32 Millivolts;UINT8 Mode;BOOLEAN Owned;CR_STATUS Status;} CR_USB_DP_PHY_RAIL;
typedef struct {
 CR_USB_DP_PHY_CONFIG Config;CR_USB_DP_PHY_IO Io;CR_MUTEX Lock;
 CR_USB_DP_PHY_CLOCK Clocks[CR_USB_DP_PHY_CLOCKS];CR_USB_DP_PHY_RAIL Rails[CR_USB_DP_PHY_RAILS];
 VOID *Owner[CrUsbDpPhyClients];BOOLEAN Ready[CrUsbDpPhyClients];
 CR_USB_DP_PHY_ROUTE Route;
 BOOLEAN Initialized,CommonReady,CommonTouched,UsbTouched,DpTouched,UsbSs;
 BOOLEAN SwitchKnown,SwitchTouched;UINT8 SwitchEnable,SwitchSelect;
 UINT32 DpRate;UINT8 DpLanes;CR_STATUS CommonStatus,ClientStatus[CrUsbDpPhyClients],IoStatus;
 UINT32 LastOffset,LastValue;CR_USB_DP_PHY_WINDOW LastWindow;
 volatile UINT32 Frozen,Active;
} CR_USB_DP_PHY;
/* Caller owns resource assignment, provider references and physical writer
   retirement. Init only validates immutable metadata; it creates no grant.
   Provider storage and callback code outlive retained ownership. Blocking
   operations require PASSIVE in NT and a normal service TPL in UEFI. The
   native adapter must exclude lower-TPL callbacks while its spin mutex is
   held. Storage starts zeroed; Init/Uninit require external lifetime drain. */
CR_STATUS CrUsbDpPhyInit(CR_USB_DP_PHY *,CONST CR_USB_DP_PHY_CONFIG *,CONST CR_USB_DP_PHY_IO *);
CR_STATUS CrUsbDpPhyAcquire(CR_USB_DP_PHY *,VOID *StableOwner,CR_USB_DP_PHY_CLIENT,
 CONST CR_USB_DP_PHY_ROUTE *,BOOLEAN SuperSpeed,BOOLEAN *Owned);
/* Acquire's Owned output means a new client admission, including partial
   failure. A duplicate returns ALREADY_STARTED and FALSE; the caller must
   preserve its earlier lease and the core's existing owner remains intact. */
/* Consumer excludes/stops its DMA and controller before Release. A partial
   off keeps exact journals/owner until success; foreign owners cannot alter
   route or hardware. The common bank remains until both clients retire. */
CR_STATUS CrUsbDpPhyRelease(CR_USB_DP_PHY *,VOID *StableOwner,CR_USB_DP_PHY_CLIENT);
CR_STATUS CrUsbDpPhyPowerDp(CR_USB_DP_PHY *,VOID *StableOwner,UINT32 RateMbps,UINT8 Lanes);
CR_STATUS CrUsbDpPhySetDpLevels(CR_USB_DP_PHY *,VOID *StableOwner,UINT8 Voltage,UINT8 PreEmphasis);
/* Fixed DP AUX latch clear through the sole owned PHY view, under its lock.
   Exact acquired DP owner/current route and powered lifetime are required.
   No arbitrary offset or alias escapes; this is not controller/link stop. */
CR_STATUS CrUsbDpPhyClearDpAuxInterrupts(CR_USB_DP_PHY *,VOID *StableOwner);
/* One-way no-wait/no-allocation admission close, usable at native EBS notify
   or NT DISPATCH. CR_BUSY keeps admission closed and forbids a final snapshot
   until the preempted operation leaves. No hardware or provider call here. */
CR_STATUS CrUsbDpPhyFreeze(CR_USB_DP_PHY *);
CR_STATUS CrUsbDpPhyUninit(CR_USB_DP_PHY *);
