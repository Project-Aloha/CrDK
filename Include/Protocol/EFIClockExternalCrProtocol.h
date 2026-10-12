/* Native display external-clock owner. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/ClockExternalPlan.h>
#include <Library/FixedPllVoteLib.h>
#define EFI_CLOCK_EXTERNAL_CR_PROTOCOL_REVISION 4ULL
#define EFI_CLOCK_EXTERNAL_CR_PROTOCOL_GUID \
  {0xdc240fb2,0x4453,0x4965,{0x86,0x7f,0x91,0x86,0x88,0x1c,0x91,0x80}}
extern EFI_GUID gEfiClockExternalCrProtocolGuid;
typedef struct _EFI_CLOCK_EXTERNAL_CR_PROTOCOL EFI_CLOCK_EXTERNAL_CR_PROTOCOL;
/* StableOwner must remain alive through successful ReleaseOwner, including
   failures returning Owned. Reserve owns the access clocks and core domain;
   the caller first supplies rail/interconnect prerequisites and later prepares
   the PHY before SetPlan. No implicit hardware adoption or native retirement. */
struct _EFI_CLOCK_EXTERNAL_CR_PROTOCOL {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *Reserve)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *, VOID *StableOwner, BOOLEAN *Owned);
  EFI_STATUS (EFIAPI *SetPlan)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *, VOID *StableOwner,
      UINT32 Controller, CONST CR_CLOCK_DSI_PLAN *, BOOLEAN On, BOOLEAN *Owned);
  EFI_STATUS (EFIAPI *GetPlan)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *, VOID *StableOwner,
      UINT32 Controller, CR_CLOCK_DSI_PLAN *, BOOLEAN *On, BOOLEAN *Pending, UINT64 *Incarnation);
  EFI_STATUS (EFIAPI *ReleaseOwner)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *, VOID *StableOwner);
  /* Caller has stopped both DSI hosts/PHYs and inherited MDSS writers while
     retaining its rail/ICC floors. Partial retirement retains all access
     votes; the exact same owner/generation must retry. This is only a clock
     fence. Completed retirement is one-shot and never stops a new pipeline. */
  EFI_STATUS (EFIAPI *RetireInheritedDsi)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *,
      VOID *StableOwner, UINT64 OwnerGeneration);
  /* Actual read-only six-gate/four-RCG acknowledgement before SMMU transfer
     and cold initialization. Requires the still-live prerequisite lease. */
  EFI_STATUS (EFIAPI *CheckInheritedDsiOff)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *,
      VOID *StableOwner, UINT64 OwnerGeneration);
  /* Early boot core BCR, before functional sources are programmed. The
     stable owner/generation and live access lease survive every failure.
     Completed assert/deassert is one-shot; it cannot reset a new pipeline.
     Caller seals writers and holds reset for 20 ms before deassertion. */
  EFI_STATUS (EFIAPI *ResetCore)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *,
      VOID *StableOwner, UINT64 OwnerGeneration, BOOLEAN Assert);
  EFI_STATUS (EFIAPI *CheckCoreResetReleased)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *,
      VOID *StableOwner, UINT64 OwnerGeneration);
  /* After the core reset, retire only inherited MDP/LUT/VSYNC/ESC gates and
     their four private RCG force-on bits. Retain access/domain/PLL and all
     unrelated votes. New functional clocks may be acquired only afterwards. */
  EFI_STATUS (EFIAPI *RetireInheritedFunctional)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *,
      VOID *StableOwner, UINT64 OwnerGeneration);
  /* Exact source ownership following core reset/functional retirement. The
     retained SupplyCheck/context requalifies suppliers (cold writes require
     real MMCX>=192 plus MX/CX/XO)
     and may not call Clock or External APIs (provider lock already held).
     Inherited source remains read-only; only qualified cold Lucid5LPE starts.
     Caller storage/supply floor survives every pending attempt and cleanup. */
  EFI_STATUS (EFIAPI *PrepareFunctionalPll)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *,
      VOID *StableOwner,UINT64 OwnerGeneration,CR_CLOCK_PLL_SUPPLY_CHECK,VOID *SupplyContext);
  EFI_STATUS (EFIAPI *CheckFunctionalPll)(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *,
      VOID *StableOwner,UINT64 OwnerGeneration);
};
