/* SM8450 owned DPU TOP0 interrupt bookkeeping. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

/* Shared register views: these values and callback fields also bind the
 * independent full-source quiet qualifier. Direct, bounded, nonpaged IO only.
 * The real mapping/lease owner admits and drains IO outside this interface;
 * callback presence does not establish a hardware or writer-retirement grant. */
typedef enum {
 CrMdssIrqParent=0,
 CrMdssIrqDpu,
 CrMdssIrqDsi0,
 CrMdssIrqDsi1,
 CrMdssIrqDpAhb,
 CrMdssIrqDpAux,
 CrMdssIrqWindowCount
} CR_MDSS_IRQ_WINDOW;
typedef struct {
 VOID *Context;
 CR_STATUS (*Read32)(VOID *,CR_MDSS_IRQ_WINDOW,UINT32 Offset,UINT32 *Value);
 CR_STATUS (*Write32)(VOID *,CR_MDSS_IRQ_WINDOW,UINT32 Offset,UINT32 Value);
 CR_STATUS (*Barrier)(VOID *);
} CR_MDSS_IRQ_IO;

#define CR_MDSS_IRQ_PARENT_STATUS 0x10U
#define CR_MDSS_IRQ_PARENT_MDP 1U
#define CR_MDSS_IRQ_TOP0_ENABLE 0x10U
#define CR_MDSS_IRQ_TOP0_STATUS 0x14U
#define CR_MDSS_IRQ_TOP0_CLEAR 0x18U
#define CR_MDSS_IRQ_WB2 (1U<<4)
#define CR_MDSS_IRQ_DSI0_VBLANK (1U<<27)
#define CR_MDSS_IRQ_DP0_UNDERRUN (1U<<24)
#define CR_MDSS_IRQ_DP0_VBLANK (1U<<25)
#define CR_MDSS_IRQ_DP0 (CR_MDSS_IRQ_DP0_UNDERRUN|CR_MDSS_IRQ_DP0_VBLANK)
#define CR_MDSS_IRQ_SUPPORTED (CR_MDSS_IRQ_WB2|CR_MDSS_IRQ_DSI0_VBLANK|CR_MDSS_IRQ_DP0)
#define CR_MDSS_IRQ_SLOT_COUNT 3U

typedef struct {
 VOID *Provider;
 UINT64 Incarnation,Serial;
 UINT32 Mask;
} CR_MDSS_IRQ_TOKEN;
typedef struct {
 CONST CR_MDSS_IRQ_TOKEN *Identity;
 CONST VOID *Owner;
 CONST VOID *Job;
 UINT64 Registration,Generation,Sequence;
 UINT32 Mask;
} CR_MDSS_IRQ_EVENT;
typedef struct {
 CR_MDSS_IRQ_TOKEN *Identity;
 CONST VOID *Owner;
 CONST VOID *Job;
 UINT64 Serial,Generation,Sequence;
 UINT32 Observed;
 BOOLEAN Armed,Pending;
} CR_MDSS_IRQ_SLOT;
typedef struct {
 CR_MDSS_IRQ_IO Io;
 UINT64 Incarnation,Serial,Generation;
 CR_MDSS_IRQ_SLOT Slots[CR_MDSS_IRQ_SLOT_COUNT];
 BOOLEAN Initialized,Closed,Faulted;
 CR_STATUS LastError;
} CR_MDSS_IRQ_CORE;

/* All calls share the actual interrupt synchronization lock. Init allocates
 * nothing and touches no registers. Storage must be zeroed/previously retired.
 * Init is not full parent qualification or a physical connect permission. */
CR_STATUS CrMdssIrqInit(CR_MDSS_IRQ_CORE *,CONST CR_MDSS_IRQ_IO *,UINT64 Incarnation);
/* Exact stable token address and owner. Mask is one supported leaf. A foreign
 * enabled leaf is rejected before ownership; committed partial failures return
 * Owned TRUE and retain the token. No callback context is stored by the core. */
CR_STATUS CrMdssIrqRegister(CR_MDSS_IRQ_CORE *,CONST VOID *Owner,UINT32 Mask,
 CR_MDSS_IRQ_TOKEN *,BOOLEAN *Owned);
/* WB Job is an exact stable identity; vblank Job must be NULL. Clear stale
 * status, then publish generation before enabling. This arms only the ledger:
 * no DMA/CTL kick, output readiness or producer stop is asserted here. The real
 * producer must execute its source-qualified final kick under the same lock. */
CR_STATUS CrMdssIrqArm(CR_MDSS_IRQ_CORE *,CONST VOID *Owner,CR_MDSS_IRQ_TOKEN *,
 CONST VOID *Job,UINT64 *Generation);
/* Called from the actual ISR after outer counted admission + exact hardware
 * Check. Outputs unchanged on error. Only observed enabled owned TOP0 bits are
 * acknowledged. WB is masked one-shot. Failures close new arm admission while
 * preserving every token/job; timeout/polling never manufactures an event. */
CR_STATUS CrMdssIrqCapture(CR_MDSS_IRQ_CORE *,BOOLEAN *Handled,BOOLEAN *Pending);
/* Synchronized event ledger drain. WB one-shot generation or latest coalesced
 * vblank sequence. Event remains unchanged on failure/no pending event. */
CR_STATUS CrMdssIrqTake(CR_MDSS_IRQ_CORE *,CONST VOID *Owner,CR_MDSS_IRQ_TOKEN *,
 CR_MDSS_IRQ_EVENT *Event);
/* Disarm changes only the exact owned bit and acknowledges observed own status.
 * It does not stop DMA. A retained Job remains until RetireJob after the real
 * producer stop/fence and DPC/output rundown; RetireJob retires metadata only. */
CR_STATUS CrMdssIrqDisarm(CR_MDSS_IRQ_CORE *,CONST VOID *Owner,CR_MDSS_IRQ_TOKEN *);
CR_STATUS CrMdssIrqRetireJob(CR_MDSS_IRQ_CORE *,CONST VOID *Owner,CR_MDSS_IRQ_TOKEN *,
 CONST VOID *Job);
/* Cancel an undelivered event as metadata after actual producer stop + DPC
 * rundown. Never delivers success. Requires the leaf already disarmed. */
CR_STATUS CrMdssIrqDiscard(CR_MDSS_IRQ_CORE *,CONST VOID *Owner,CR_MDSS_IRQ_TOKEN *);
CR_STATUS CrMdssIrqUnregister(CR_MDSS_IRQ_CORE *,CONST VOID *Owner,CR_MDSS_IRQ_TOKEN *);
/* Software-only irreversible capture/arm closure, safe after access loss. It
 * neither masks an asserted line nor disconnects it nor fences DMA. Outer
 * adapter must retain mappings/votes and disconnect/drain ISR/DPC separately. */
VOID CrMdssIrqQuarantine(CR_MDSS_IRQ_CORE *);
CR_STATUS CrMdssIrqUninit(CR_MDSS_IRQ_CORE *);
