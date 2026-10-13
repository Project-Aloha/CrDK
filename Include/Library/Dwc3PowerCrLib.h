/* SM8450 DWC3 prerequisite journal. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_DWC3_POWER_CLOCKS 6U
#define CR_DWC3_POWER_PATHS 2U
typedef struct { CONST CHAR8 *Name; UINT64 RateHz; } CR_DWC3_POWER_CLOCK;
extern CONST CR_DWC3_POWER_CLOCK CrDwc3PowerClocks[CR_DWC3_POWER_CLOCKS];
typedef struct {
  VOID *Context;
  /* Pure current access check. The adapter holds the actual provider/D0,
     source-clock and assigned-resource leases for this entire journal. */
  CR_STATUS (*Access)(VOID *);
  /* Absolute per-instance requests. Acquire reports its own retained reference
     even on failure. Release is idempotent and clears Owned only on success.
     Path index0 = dwc3 0x6000->0x6001; index1 = 0x6002->0x6003. kB/s units. */
  CR_STATUS (*Path)(VOID *,UINT32 Index,BOOLEAN On,UINT64 Average,UINT64 Peak,BOOLEAN *Owned);
  CR_STATUS (*Domain)(VOID *,BOOLEAN On,BOOLEAN *Owned);
  CR_STATUS (*Clock)(VOID *,CONST CHAR8 *Name,UINT64 RateHz,BOOLEAN On,BOOLEAN *Owned);
  /* Controller reset gcc_usb30_prim_bcr only; never the shared combo PHY. */
  CR_STATUS (*Reset)(VOID *,BOOLEAN Assert);
  /* Validate read-only UTMI postdivider =1 while its leaf is gated. The
     SM8450 GccUsbMock node has DividerReadOnly=TRUE/RequiredDivider=1;
     its provider's SetDivider entrypoint therefore only checks this value. */
  CR_STATUS (*UtmiDivider)(VOID *);
} CR_DWC3_POWER_IO;
typedef struct {
  CR_DWC3_POWER_IO Io;
  BOOLEAN Initialized,Started,SuperSpeed,Ready,ResetTouched,Domain;
  BOOLEAN Clocks[CR_DWC3_POWER_CLOCKS],Paths[CR_DWC3_POWER_PATHS];
  UINT32 AcquirePhase;
  BOOLEAN AcquirePending;
} CR_DWC3_POWER;

/* Zeroed permanent caller storage, exclusively serialized outside these calls;
   no callback may reenter. NT calls are PASSIVE. No allocations or DMA grants.
   This journal does not prove native transfer or former role-driver retirement.
   The caller must establish both before Acquire or Quiesce touches hardware.
   In particular, EvtUrsSetRole entry is not itself that proof.
   Acquire: ICB, reset pulse, GDSC, divider validation, six leaf clocks. On any
   failure retain the object/providers. Resume retries only the unacknowledged
   acquisition phase on the same exact object/SS request; callbacks must replay
   their absolute retained-owner requests without adding references. It is
   allowed only before any PHY acquisition/configuration or child admission,
   under the complete original retired-writer gate. Native reference callbacks
   which lack that absolute retry contract must not call Resume. Quiesce closes
   this acquisition recovery permanently, even if its own cleanup fails.
   Halt/drain the former role if needed, release the USB PHY after a fresh halt,
   then call Quiesce and sample fresh halt/IRQs again before Release. Quiesce completes a
   reset pulse (never leave it asserted across the next cold GDSC power-up).
   Release must follow acknowledged PHY release and retains every prerequisite
   below a failed cleanup operation. None of these calls stops a live child. */
CR_STATUS CrDwc3PowerInit(CR_DWC3_POWER *,CONST CR_DWC3_POWER_IO *);
CR_STATUS CrDwc3PowerAcquire(CR_DWC3_POWER *,BOOLEAN SuperSpeed);
CR_STATUS CrDwc3PowerResume(CR_DWC3_POWER *,BOOLEAN SuperSpeed);
CR_STATUS CrDwc3PowerQuiesce(CR_DWC3_POWER *);
CR_STATUS CrDwc3PowerRelease(CR_DWC3_POWER *);
CR_STATUS CrDwc3PowerUninit(CR_DWC3_POWER *);
