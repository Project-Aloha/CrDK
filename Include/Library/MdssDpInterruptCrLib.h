/* Owned SM8450 DP AUX interrupt ledger. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/MdssDpuInterruptCrLib.h>

#define CR_MDSS_IRQ_PARENT_DP (1U << 12)
#define CR_MDSS_DP_AUX_IRQ_STATUS ((1U << 3) | (1U << 6) | (1U << 9) | \
  (1U << 12) | (1U << 15) | (1U << 18) | (1U << 21) | (1U << 24) | (1U << 27))
#define CR_MDSS_DP_AUX_IRQ_MASK (CR_MDSS_DP_AUX_IRQ_STATUS << 2)

typedef struct {
  VOID *Provider;
  UINT64 Incarnation, Serial;
} CR_MDSS_DP_IRQ_TOKEN;
typedef struct {
  CONST CR_MDSS_DP_IRQ_TOKEN *Identity;
  CONST VOID *Owner, *Request;
  UINT64 Registration, Generation, Sequence;
  UINT32 Status;
} CR_MDSS_DP_IRQ_EVENT;
typedef struct {
  CR_MDSS_IRQ_IO Io;
  CR_MDSS_DP_IRQ_TOKEN *Identity;
  CONST VOID *Owner, *Request;
  UINT64 Incarnation, Serial, Generation, Sequence;
  UINT32 Status;
  BOOLEAN Initialized, Armed, Pending, Closed, Faulted;
  CR_STATUS LastError;
  /* Separate STATUS2 READY/IDLE child, same parent connection and lock. */
  struct {
    CR_MDSS_DP_IRQ_TOKEN *Identity;
    CONST VOID *Owner,*Request;
    UINT64 Serial,Generation,Sequence;
    UINT32 Expected,Status;
    BOOLEAN Armed,Pending;
  } Link;
} CR_MDSS_DP_IRQ_CORE;

/* All calls share the real parent interrupt lock. Caller owns current maps,
   powered lifetime and native/foreign writer exclusion. The ledger grants
   none of those. Token and Request addresses are permanent exact identities.
   Init performs no IO; committed Register/Arm failures retain their journal. */
CR_STATUS CrMdssDpIrqInit(CR_MDSS_DP_IRQ_CORE *, CONST CR_MDSS_IRQ_IO *, UINT64);
CR_STATUS CrMdssDpIrqRegister(CR_MDSS_DP_IRQ_CORE *, CONST VOID *,
  CR_MDSS_DP_IRQ_TOKEN *, BOOLEAN *Owned);
CR_STATUS CrMdssDpIrqArm(CR_MDSS_DP_IRQ_CORE *, CONST VOID *,
  CR_MDSS_DP_IRQ_TOKEN *, CONST VOID *Request, UINT64 *Generation);
CR_STATUS CrMdssDpIrqCapture(CR_MDSS_DP_IRQ_CORE *, BOOLEAN *Handled, BOOLEAN *Pending);
CR_STATUS CrMdssDpIrqTake(CR_MDSS_DP_IRQ_CORE *, CONST VOID *,
  CR_MDSS_DP_IRQ_TOKEN *, CR_MDSS_DP_IRQ_EVENT *);
/* Disarm masks/ACKs the exact AUX family, not GO/controller/PHY stop. Retire
   Request only after actual GO-off/reset and waiter/DPC rundown. */
CR_STATUS CrMdssDpIrqDisarm(CR_MDSS_DP_IRQ_CORE *, CONST VOID *, CR_MDSS_DP_IRQ_TOKEN *);
CR_STATUS CrMdssDpIrqDiscard(CR_MDSS_DP_IRQ_CORE *, CONST VOID *, CR_MDSS_DP_IRQ_TOKEN *);
CR_STATUS CrMdssDpIrqRetireRequest(CR_MDSS_DP_IRQ_CORE *, CONST VOID *,
  CR_MDSS_DP_IRQ_TOKEN *, CONST VOID *Request);
CR_STATUS CrMdssDpIrqUnregister(CR_MDSS_DP_IRQ_CORE *, CONST VOID *, CR_MDSS_DP_IRQ_TOKEN *);
VOID CrMdssDpIrqQuarantine(CR_MDSS_DP_IRQ_CORE *);
CR_STATUS CrMdssDpIrqUninit(CR_MDSS_DP_IRQ_CORE *);
/* Exact same copied IO projection must bind both cores. No caller-supplied
   AllowedParent bitmask; unowned DP remains forbidden by the legacy API. */
CR_STATUS CrMdssIrqCaptureWithDp(CR_MDSS_IRQ_CORE *, CR_MDSS_DP_IRQ_CORE *,
  BOOLEAN *Handled, BOOLEAN *Pending);

#define CR_MDSS_DP_LINK_READY 1U
#define CR_MDSS_DP_LINK_IDLE (1U<<3)
#define CR_MDSS_DP_LINK_STATUS (CR_MDSS_DP_LINK_READY|CR_MDSS_DP_LINK_IDLE)
#define CR_MDSS_DP_LINK_MASK (CR_MDSS_DP_LINK_STATUS<<2)
/* READY/IDLE only. Register/Arm do not assert mainlink/scanout completion.
 * Real typed backend must execute the exact permitted final state write under
 * the SAME synchronization after Arm, and retain Request through stop/drain. */
CR_STATUS CrMdssDpLinkIrqRegister(CR_MDSS_DP_IRQ_CORE *,CONST VOID *,CR_MDSS_DP_IRQ_TOKEN *,BOOLEAN *);
CR_STATUS CrMdssDpLinkIrqArm(CR_MDSS_DP_IRQ_CORE *,CONST VOID *,CR_MDSS_DP_IRQ_TOKEN *,CONST VOID *Request,UINT32 Expected,UINT64 *);
CR_STATUS CrMdssDpLinkIrqCapture(CR_MDSS_DP_IRQ_CORE *,BOOLEAN *,BOOLEAN *);
CR_STATUS CrMdssDpLinkIrqTake(CR_MDSS_DP_IRQ_CORE *,CONST VOID *,CR_MDSS_DP_IRQ_TOKEN *,CR_MDSS_DP_IRQ_EVENT *);
CR_STATUS CrMdssDpLinkIrqDisarm(CR_MDSS_DP_IRQ_CORE *,CONST VOID *,CR_MDSS_DP_IRQ_TOKEN *);
CR_STATUS CrMdssDpLinkIrqDiscard(CR_MDSS_DP_IRQ_CORE *,CONST VOID *,CR_MDSS_DP_IRQ_TOKEN *);
CR_STATUS CrMdssDpLinkIrqRetireRequest(CR_MDSS_DP_IRQ_CORE *,CONST VOID *,CR_MDSS_DP_IRQ_TOKEN *,CONST VOID *);
CR_STATUS CrMdssDpLinkIrqUnregister(CR_MDSS_DP_IRQ_CORE *,CONST VOID *,CR_MDSS_DP_IRQ_TOKEN *);
