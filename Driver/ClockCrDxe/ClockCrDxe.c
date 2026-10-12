/** @file
  Crane-owned SM8450 clock protocol backed directly by ClockLib.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>

#include <Library/CrDalLib.h>
#include <Library/CrTargetClockLib.h>
#include <Library/clock.h>
#include <Library/ClockExternalLease.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIClockExternalCrProtocol.h>
#include "GpuBootAccess.h"
STATIC CR_GPU_BOOT_CLOCK mGpuBoot;
STATIC EFI_GUID mGpuBootGuid=EFI_CR_ADRENO_BOOT_ACCESS_GUID;

#define CLOCK_CR_MAX_CLOCK_VOTES         64U
#define CLOCK_CR_MAX_POWER_DOMAIN_VOTES  8U

/* Private backend ABI. This deliberately has no dependency on a Qualcomm
 * clock protocol or its GUID. The backend is implemented by ClockLib below
 * and exists only to keep the vote code small. */
#define CRANE_CLOCK_BACKEND_REVISION  1ULL
typedef struct _CRANE_CLOCK_BACKEND CRANE_CLOCK_BACKEND;
typedef enum {
  CraneClockFrequencyHzAtMost = 1,
  CraneClockFrequencyHzExact  = 3
} CRANE_CLOCK_FREQUENCY_TYPE;
typedef enum {
  CraneClockResetDeassert = 0,
  CraneClockResetAssert   = 1
} CRANE_CLOCK_RESET_TYPE;
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_GET_ID)(
  CRANE_CLOCK_BACKEND *, CONST CHAR8 *, UINTN *);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_ENABLE)(CRANE_CLOCK_BACKEND *, UINTN);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_DISABLE)(CRANE_CLOCK_BACKEND *, UINTN);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_IS_ENABLED)(
  CRANE_CLOCK_BACKEND *, UINTN, BOOLEAN *);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_IS_ON)(
  CRANE_CLOCK_BACKEND *, UINTN, BOOLEAN *);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_SET_FREQ_HZ)(
  CRANE_CLOCK_BACKEND *, UINTN, UINT32, CRANE_CLOCK_FREQUENCY_TYPE, UINT32 *);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_GET_FREQ_HZ)(
  CRANE_CLOCK_BACKEND *, UINTN, UINT32 *);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_CALC_FREQ_HZ)(
  CRANE_CLOCK_BACKEND *, UINTN, UINT32 *);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_SELECT_EXTERNAL_SOURCE)(
  CRANE_CLOCK_BACKEND *, UINTN, UINT32, UINT32, UINT32, UINT32, UINT32, UINT32);
typedef EFI_STATUS (EFIAPI *CRANE_POWER_DOMAIN_GET_ID)(
  CRANE_CLOCK_BACKEND *, CONST CHAR8 *, UINTN *);
typedef EFI_STATUS (EFIAPI *CRANE_POWER_DOMAIN_ENABLE)(CRANE_CLOCK_BACKEND *, UINTN);
typedef EFI_STATUS (EFIAPI *CRANE_POWER_DOMAIN_DISABLE)(CRANE_CLOCK_BACKEND *, UINTN);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_SET_DIVIDER)(
  CRANE_CLOCK_BACKEND *, UINTN, UINT32);
typedef EFI_STATUS (EFIAPI *CRANE_CLOCK_RESET)(
  CRANE_CLOCK_BACKEND *, UINTN, CRANE_CLOCK_RESET_TYPE);

struct _CRANE_CLOCK_BACKEND {
  UINT64                              Version;
  CRANE_CLOCK_GET_ID                  GetClockID;
  CRANE_CLOCK_ENABLE                  EnableClock;
  CRANE_CLOCK_DISABLE                 DisableClock;
  CRANE_CLOCK_IS_ENABLED              IsClockEnabled;
  CRANE_CLOCK_IS_ON                   IsClockOn;
  CRANE_CLOCK_SET_FREQ_HZ             SetClockFreqHz;
  CRANE_CLOCK_GET_FREQ_HZ             GetClockFreqHz;
  CRANE_CLOCK_CALC_FREQ_HZ            CalcClockFreqHz;
  CRANE_CLOCK_SELECT_EXTERNAL_SOURCE  SelectExternalSource;
  CRANE_POWER_DOMAIN_GET_ID           GetClockPowerDomainID;
  CRANE_POWER_DOMAIN_ENABLE           EnableClockPowerDomain;
  CRANE_POWER_DOMAIN_DISABLE          DisableClockPowerDomain;
  CRANE_CLOCK_SET_DIVIDER             SetClockDivider;
  CRANE_CLOCK_RESET                   ResetClock;
};

typedef struct {
  UINTN   NativeId;
  UINT32  References;
  UINT32  RateHz;
  BOOLEAN Uncertain;
  BOOLEAN CleanupPending;
  EFI_STATUS FailureStatus;
} CLOCK_CR_VOTE;

STATIC CRANE_CLOCK_BACKEND *mNativeClock;
STATIC ClockDriverContext *mClockContext;

/* Requested rates are kept separately because the Crane clock ABI splits a
 * rate vote from the subsequent enable call. */
typedef struct {
  ClockNode *Node;
  UINT32     RateHz;
} CRANE_CLOCK_RATE;

STATIC CRANE_CLOCK_RATE mCraneRates[CLOCK_CR_MAX_CLOCK_VOTES];

STATIC
BOOLEAN
IsPcieClockName (
  IN CONST CHAR8 *Name
  )
{
  STATIC CONST CHAR8 Prefix[] = "gcc_pcie_";
  UINTN Index;

  if (Name == NULL) {
    return FALSE;
  }
  for (Index = 0; Prefix[Index] != '\0'; Index++) {
    if (Name[Index] != Prefix[Index]) {
      return FALSE;
    }
  }
  return TRUE;
}

/* Keep a raw GCC snapshot beside each first PCIe clock vote.  A successful
 * ClockLib call only proves that the RCG update and branch gate callbacks
 * returned; this trace shows the selector/divider, root command, branch gate
 * and PHY mux values that the hardware actually exposed at that instant. */
STATIC
VOID
DumpPcieClockState (
  IN CONST CHAR8 *NativeName,
  IN UINT32       RequestedRate,
  IN BOOLEAN      Enable
  )
{
  ClockNode *Node;
  ClockNode *Source;
  UINTN     Base;
  UINTN     BranchAddress;
  UINTN     HaltAddress;
  UINTN     SourceCommand;
  UINTN     SourceConfig;
  UINTN     MuxAddress;
  UINT32    BranchValue;
  UINT32    HaltValue;
  UINT32    CommandValue;
  UINT32    ConfigValue;
  UINT32    MuxValue;
  UINT32    Selector;
  UINT32    Divider;

  if ((NativeName == NULL) || !IsPcieClockName (NativeName) ||
      (mClockContext == NULL)) {
    return;
  }
  if (EFI_ERROR (GetClockNode (mClockContext, NativeName, NULL, NULL, &Node)) ||
      (Node == NULL) || (Node->ParentController == NULL)) {
    return;
  }

  Base = Node->ParentController->Address;
  BranchAddress = HaltAddress = SourceCommand = SourceConfig = MuxAddress = 0;
  BranchValue = HaltValue = CommandValue = ConfigValue = MuxValue = 0;
  Source = Node;
  if ((Node->Type == CLOCK_NODE_TYPE_BRANCH) ||
      (Node->Type == CLOCK_NODE_TYPE_BRANCH_2)) {
    BranchAddress = Base + Node->EnableRegister;
    HaltAddress = Base + Node->HaltRegister;
    BranchValue = CrMmioRead32 (BranchAddress);
    HaltValue = CrMmioRead32 (HaltAddress);
    if ((Node->ParentCount == 1) && (Node->Parents != NULL) &&
        (Node->Parents[0] != NULL)) {
      Source = Node->Parents[0];
    }
  }
  if ((Source->Type == CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) &&
      (Source->ParentController != NULL)) {
    SourceCommand = Source->ParentController->Address + Source->CmdRegister;
    SourceConfig = SourceCommand +
                   Source->CmdRCfgOffset +
                   CLOCK_NODE_RCG_CMD_REGISTER_CFG_REG_OFFSET;
    CommandValue = CrMmioRead32 (SourceCommand);
    ConfigValue = CrMmioRead32 (SourceConfig);
  } else if ((Source->Type == CLOCK_NODE_TYPE_PHY_MUX) ||
             (Source->Type == CLOCK_NODE_TYPE_MUX)) {
    MuxAddress = Source->ParentController->Address + Source->MuxRegister;
    MuxValue = CrMmioRead32 (MuxAddress);
  }

  Selector = (ConfigValue & CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK) >> 8;
  Divider = ((ConfigValue & GEN_MSK (4, 0)) + 1U) / 2U;
  DEBUG ((DEBUG_INFO,
          "ClockCrDxe: PCIe clock %a rate=%u enable=%u branch@0x%lx=0x%08x halt@0x%lx=0x%08x cmd@0x%lx=0x%08x cfg@0x%lx=0x%08x selector=%u divider=%u mux@0x%lx=0x%08x\n",
          NativeName, RequestedRate, Enable ? 1 : 0,
          BranchAddress, BranchValue, HaltAddress, HaltValue,
          SourceCommand, CommandValue, SourceConfig, ConfigValue,
          Selector, Divider, MuxAddress, MuxValue));
}

STATIC EFI_STATUS
CrStatusToEfi (
  IN CR_STATUS Status
  )
{
  return Status;
}

STATIC
CRANE_CLOCK_RATE *
FindCraneRate (
  IN ClockNode *Node,
  IN BOOLEAN    Allocate
  )
{
  UINTN Index;
  CRANE_CLOCK_RATE *Free;

  Free = NULL;
  for (Index = 0; Index < ARRAY_SIZE (mCraneRates); Index++) {
    if (mCraneRates[Index].Node == Node) {
      return &mCraneRates[Index];
    }
    if ((Free == NULL) && (mCraneRates[Index].Node == NULL)) {
      Free = &mCraneRates[Index];
    }
  }
  return Allocate ? Free : NULL;
}

STATIC EFI_STATUS EFIAPI
CraneClockGetId (
  IN CRANE_CLOCK_BACKEND *This,
  IN CONST CHAR8        *Name,
  OUT UINTN             *Id
  )
{
  ClockNode *Node;
  CR_STATUS Status;

  if ((This == NULL) || (Name == NULL) || (Id == NULL) ||
      (mClockContext == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = GetClockNode (mClockContext, Name, NULL, NULL, &Node);
  if (EFI_ERROR (Status)) {
    return CrStatusToEfi (Status);
  }
  *Id = (UINTN)Node;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
CraneClockEnable (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id
  )
{
  if (CrGpuBootClockGuard(&mGpuBoot,(ClockNode *)(UINTN)Id)!=EFI_SUCCESS) return EFI_ACCESS_DENIED;
  ClockNode *Node;
  CRANE_CLOCK_RATE *Rate;

  if ((This == NULL) || (mClockContext == NULL) || (Id == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  Node = (ClockNode *)(UINTN)Id;
  Rate = FindCraneRate (Node, FALSE);
  return CrStatusToEfi (ClockEnable (
                         mClockContext, Node,
                         (Rate == NULL) ? 0 : Rate->RateHz, TRUE));
}

STATIC EFI_STATUS EFIAPI
CraneClockDisable (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id
  )
{
  if (CrGpuBootClockGuard(&mGpuBoot,(ClockNode *)(UINTN)Id)!=EFI_SUCCESS) return EFI_ACCESS_DENIED;
  if ((This == NULL) || (mClockContext == NULL) || (Id == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  return CrStatusToEfi (ClockEnable (
                         mClockContext, (ClockNode *)(UINTN)Id, 0, FALSE));
}

STATIC EFI_STATUS EFIAPI
CraneClockIsEnabled (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id,
  OUT BOOLEAN            *Enabled
  )
{
  ClockNode *Node;

  if ((This == NULL) || (mClockContext == NULL) || (Id == 0) ||
      (Enabled == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Node = (ClockNode *)(UINTN)Id;
  *Enabled = (Node->ReferenceCount != 0);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
CraneClockIsOn (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id,
  OUT BOOLEAN            *On
  )
{
  return CraneClockIsEnabled (This, Id, On);
}

STATIC EFI_STATUS EFIAPI
CraneClockSetFreq (
  IN CRANE_CLOCK_BACKEND      *This,
  IN UINTN                    Id,
  IN UINT32                   Freq,
  IN CRANE_CLOCK_FREQUENCY_TYPE Match,
  OUT UINT32                 *Result OPTIONAL
  )
{
  if (CrGpuBootClockGuard(&mGpuBoot,(ClockNode *)(UINTN)Id)!=EFI_SUCCESS) return EFI_ACCESS_DENIED;
  ClockNode *Node;
  CRANE_CLOCK_RATE *Rate;

  if ((This == NULL) || (mClockContext == NULL) || (Id == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  Node = (ClockNode *)(UINTN)Id;
  Rate = FindCraneRate (Node, TRUE);
  if (Rate == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  /* Stage only. ClockAcquire programs and checks the rate under the core
     ownership lock; an eager RCG write could change a foreign owner's clock
     before the later acquisition rejects the conflicting request. */
  (VOID)Match;
  Rate->Node = Node;
  Rate->RateHz = Freq;
  if (Result != NULL) {
    *Result = Freq;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
CraneClockGetFreq (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id,
  OUT UINT32             *Freq
  )
{
  CRANE_CLOCK_RATE *Rate;

  if ((This == NULL) || (Freq == NULL) || (Id == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  Rate = FindCraneRate ((ClockNode *)(UINTN)Id, FALSE);
  if (Rate == NULL) {
    return EFI_NOT_FOUND;
  }
  *Freq = Rate->RateHz;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
CraneClockCalcFreq (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id,
  OUT UINT32             *Freq
  )
{
  return CraneClockGetFreq (This, Id, Freq);
}

STATIC EFI_STATUS EFIAPI
CraneClockSelectSource (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id,
  IN UINT32              FreqHz,
  IN UINT32              Source,
  IN UINT32              Divider,
  IN UINT32              M,
  IN UINT32              N,
  IN UINT32              TwiceD
  )
{
  if (CrGpuBootClockGuard(&mGpuBoot,(ClockNode *)(UINTN)Id)!=EFI_SUCCESS) return EFI_ACCESS_DENIED;
  (VOID)FreqHz;
  if (This==NULL || mClockContext==NULL || Id==0) return EFI_INVALID_PARAMETER;
  return CrStatusToEfi(ClockSelectExternalSource(mClockContext,(ClockNode *)Id,
                                                Source,Divider,M,N,TwiceD));
}

STATIC EFI_STATUS EFIAPI
CranePowerDomainGetId (
  IN CRANE_CLOCK_BACKEND *This,
  IN CONST CHAR8        *Name,
  OUT UINTN             *Id
  )
{
  ClockNode *Node;
  CR_STATUS Status;

  if ((This == NULL) || (Name == NULL) || (Id == NULL) ||
      (mClockContext == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = GetGdscNode (mClockContext, Name, &Node);
  if (EFI_ERROR (Status)) {
    return CrStatusToEfi (Status);
  }
  *Id = (UINTN)Node;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
CranePowerDomainEnable (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id
  )
{
  if (CrGpuBootClockGuard(&mGpuBoot,(ClockNode *)(UINTN)Id)!=EFI_SUCCESS) return EFI_ACCESS_DENIED;
  if ((This == NULL) || (mClockContext == NULL) || (Id == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  return CrStatusToEfi (ClockGdscSet (
                         mClockContext, (ClockNode *)(UINTN)Id, TRUE));
}

STATIC EFI_STATUS EFIAPI
CranePowerDomainDisable (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id
  )
{
  if (CrGpuBootClockGuard(&mGpuBoot,(ClockNode *)(UINTN)Id)!=EFI_SUCCESS) return EFI_ACCESS_DENIED;
  if ((This == NULL) || (mClockContext == NULL) || (Id == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  return CrStatusToEfi (ClockGdscSet (
                         mClockContext, (ClockNode *)(UINTN)Id, FALSE));
}

STATIC EFI_STATUS EFIAPI
CraneClockSetDivider (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               Id,
  IN UINT32              Divider
  )
{
  if (CrGpuBootClockGuard(&mGpuBoot,(ClockNode *)(UINTN)Id)!=EFI_SUCCESS) return EFI_ACCESS_DENIED;
  if (This==NULL || mClockContext==NULL || Id==0) return EFI_INVALID_PARAMETER;
  return CrStatusToEfi(ClockSetDivider(mClockContext,(ClockNode *)Id,Divider));
}

STATIC EFI_STATUS EFIAPI
CraneClockReset (
  IN CRANE_CLOCK_BACKEND  *This,
  IN UINTN                Id,
  IN CRANE_CLOCK_RESET_TYPE ResetType
  )
{
  (VOID)This;
  (VOID)Id;
  (VOID)ResetType;
  return EFI_UNSUPPORTED;
}

STATIC CRANE_CLOCK_BACKEND mCraneNativeClock = {
  .Version                   = CRANE_CLOCK_BACKEND_REVISION,
  .GetClockID               = CraneClockGetId,
  .EnableClock              = CraneClockEnable,
  .DisableClock             = CraneClockDisable,
  .IsClockEnabled           = CraneClockIsEnabled,
  .IsClockOn                = CraneClockIsOn,
  .SetClockFreqHz           = CraneClockSetFreq,
  .GetClockFreqHz           = CraneClockGetFreq,
  .CalcClockFreqHz          = CraneClockCalcFreq,
  .SelectExternalSource     = CraneClockSelectSource,
  .GetClockPowerDomainID    = CranePowerDomainGetId,
  .EnableClockPowerDomain   = CranePowerDomainEnable,
  .DisableClockPowerDomain  = CranePowerDomainDisable,
  .SetClockDivider          = CraneClockSetDivider,
  .ResetClock               = CraneClockReset,
};
STATIC CLOCK_CR_VOTE       mClockVotes[CLOCK_CR_MAX_CLOCK_VOTES];
STATIC CLOCK_CR_VOTE       mPowerDomainVotes[CLOCK_CR_MAX_POWER_DOMAIN_VOTES];
STATIC EFI_LOCK            mVoteLock = EFI_INITIALIZE_LOCK_VARIABLE (TPL_NOTIFY);
STATIC CR_CLOCK_EXTERNAL_LEASE mExternalLease;
STATIC EFI_CLOCK_EXTERNAL_CR_PROTOCOL mExternalProtocol;

/* Same outer serialization as every generic mutation; nested ClockLib locks
   protect registers. A reservation blocks even inactive plans and partial
   access cleanup from being changed by anonymous legacy calls. */
STATIC EFI_STATUS GuardExternalNode(UINTN Id) {
  if (mNativeClock != &mCraneNativeClock) return EFI_SUCCESS;
  EFI_STATUS S=CrGpuBootClockGuard(&mGpuBoot,(ClockNode *)Id);
  return S!=EFI_SUCCESS?S:CrStatusToEfi(CrClockExternalLeaseGuard(&mExternalLease,(ClockNode *)Id));
}
STATIC EFI_STATUS ExternalAdmission(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This) {
  if (This != &mExternalProtocol || EfiGetCurrentTpl() > TPL_NOTIFY)
    return EFI_INVALID_PARAMETER;
  return mExternalLease.External.Initialized ? EFI_SUCCESS : EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI ExternalReserve(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner, BOOLEAN *Owned) {
  if (!CrGpuBootOutputAllowed(&mGpuBoot,Owned,sizeof(*Owned))) return EFI_INVALID_PARAMETER;
  *Owned=FALSE;
  EFI_STATUS S=ExternalAdmission(This); if(EFI_ERROR(S)) return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseReserve(&mExternalLease,Owner,Owned));
  EfiReleaseLock(&mVoteLock);
  return S;
}
STATIC EFI_STATUS EFIAPI ExternalSetPlan(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner, UINT32 Controller, CONST CR_CLOCK_DSI_PLAN *Plan,
    BOOLEAN On, BOOLEAN *Owned) {
  if (!CrGpuBootOutputAllowed(&mGpuBoot,Owned,sizeof(*Owned))) return EFI_INVALID_PARAMETER;
  *Owned=FALSE;
  EFI_STATUS S=ExternalAdmission(This); if(EFI_ERROR(S)) return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseSetPlan(&mExternalLease,Owner,Controller,Plan,On,Owned));
  EfiReleaseLock(&mVoteLock);
  return S;
}
STATIC EFI_STATUS EFIAPI ExternalGetPlan(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner, UINT32 Controller, CR_CLOCK_DSI_PLAN *Plan,
    BOOLEAN *On, BOOLEAN *Pending, UINT64 *Incarnation) {
  EFI_STATUS S=ExternalAdmission(This); if(EFI_ERROR(S)) return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseGetPlan(&mExternalLease,Owner,Controller,Plan,On,Pending,Incarnation));
  EfiReleaseLock(&mVoteLock);
  return S;
}
STATIC EFI_STATUS EFIAPI ExternalRelease(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This, VOID *Owner) {
  EFI_STATUS S=ExternalAdmission(This); if(EFI_ERROR(S)) return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseRelease(&mExternalLease,Owner));
  EfiReleaseLock(&mVoteLock);
  return S;
}
STATIC EFI_STATUS EFIAPI ExternalRetireInheritedDsi(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner,UINT64 Generation) {
  EFI_STATUS S=ExternalAdmission(This);if(EFI_ERROR(S))return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseRetireInheritedDsi(&mExternalLease,Owner,Generation));
  EfiReleaseLock(&mVoteLock);return S;
}
STATIC EFI_STATUS EFIAPI ExternalCheckInheritedDsiOff(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner,UINT64 Generation) {
  EFI_STATUS S=ExternalAdmission(This);if(EFI_ERROR(S))return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseCheckInheritedDsiOff(&mExternalLease,Owner,Generation));
  EfiReleaseLock(&mVoteLock);return S;
}
STATIC EFI_STATUS EFIAPI ExternalResetCore(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner,UINT64 Generation,BOOLEAN Assert) {
  EFI_STATUS S=ExternalAdmission(This);if(EFI_ERROR(S))return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseResetCore(&mExternalLease,Owner,Generation,Assert));
  EfiReleaseLock(&mVoteLock);return S;
}
STATIC EFI_STATUS EFIAPI ExternalCheckCoreResetReleased(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner,UINT64 Generation) {
  EFI_STATUS S=ExternalAdmission(This);if(EFI_ERROR(S))return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseCheckCoreResetReleased(&mExternalLease,Owner,Generation));
  EfiReleaseLock(&mVoteLock);return S;
}
STATIC EFI_STATUS EFIAPI ExternalRetireInheritedFunctional(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner,UINT64 Generation) {
  EFI_STATUS S=ExternalAdmission(This);if(EFI_ERROR(S))return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseRetireInheritedFunctional(&mExternalLease,Owner,Generation));
  EfiReleaseLock(&mVoteLock);return S;
}
STATIC EFI_STATUS EFIAPI ExternalPrepareFunctionalPll(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner,UINT64 Generation,CR_CLOCK_PLL_SUPPLY_CHECK Supply,VOID *Context) {
  EFI_STATUS S=ExternalAdmission(This);if(EFI_ERROR(S))return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeasePrepareFunctionalPll(&mExternalLease,Owner,Generation,Supply,Context));
  EfiReleaseLock(&mVoteLock);return S;
}
STATIC EFI_STATUS EFIAPI ExternalCheckFunctionalPll(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *This,
    VOID *Owner,UINT64 Generation) {
  EFI_STATUS S=ExternalAdmission(This);if(EFI_ERROR(S))return S;
  EfiAcquireLock(&mVoteLock);
  S=CrStatusToEfi(CrClockExternalLeaseCheckFunctionalPll(&mExternalLease,Owner,Generation));
  EfiReleaseLock(&mVoteLock);return S;
}
STATIC EFI_CLOCK_EXTERNAL_CR_PROTOCOL mExternalProtocol = {
  EFI_CLOCK_EXTERNAL_CR_PROTOCOL_REVISION, ExternalReserve, ExternalSetPlan,
  ExternalGetPlan, ExternalRelease,ExternalRetireInheritedDsi,ExternalCheckInheritedDsiOff,
  ExternalResetCore,ExternalCheckCoreResetReleased,ExternalRetireInheritedFunctional,
  ExternalPrepareFunctionalPll,ExternalCheckFunctionalPll
};


STATIC
EFI_STATUS
ResolveNativeResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  EFI_STATUS Status;

  if ((Controller == NULL) || (Id == NULL) || (NativeId == NULL) ||
      (Flags == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CrDalClockResolveResource (
             Controller,
             Id,
             Kind,
             NativeId,
             Flags
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((*NativeId == NULL) ||
      ((*Flags & ~CR_DAL_CLOCK_RESOURCE_VALID_FLAGS) != 0)) {
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

STATIC
CLOCK_CR_VOTE *
FindVote (
  IN CLOCK_CR_VOTE *Votes,
  IN UINTN          VoteCount,
  IN UINTN          NativeId,
  IN BOOLEAN        Allocate
  )
{
  CLOCK_CR_VOTE *FreeVote;
  UINTN          Index;

  FreeVote = NULL;
  for (Index = 0; Index < VoteCount; Index++) {
    if ((Votes[Index].References != 0) &&
        (Votes[Index].NativeId == NativeId)) {
      return &Votes[Index];
    }
    if ((FreeVote == NULL) && (Votes[Index].References == 0)) {
      FreeVote = &Votes[Index];
    }
  }
  return Allocate ? FreeVote : NULL;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetClockInternal (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN UINT64                 RateHz,
  IN BOOLEAN                Enable,
  OUT BOOLEAN               *Owned OPTIONAL
  )
{
  CONST CHAR8    *NativeName;
  CLOCK_CR_VOTE  *Vote;
  EFI_STATUS      EnableStatus;
  EFI_STATUS      Status;
  UINT32          Flags;
  UINT32          ResultRate;
  UINTN           NativeId;
  BOOLEAN         IsEnabled;
  BOOLEAN         WasEnabled;
  BOOLEAN         CoreOwned;

  if (Owned != NULL) {
    if (!CrGpuBootOutputAllowed(&mGpuBoot,Owned,sizeof(*Owned))) return EFI_INVALID_PARAMETER;
    *Owned = FALSE;
  }

  if ((This == NULL) || (Controller == NULL) || (Id == NULL) || Enable > 1) {
    return EFI_INVALID_PARAMETER;
  }
  if (mNativeClock == NULL) {
    return EFI_NOT_READY;
  }

  Status = ResolveNativeResource (
             Controller,
             Id,
             CrDalClockResourceClock,
             &NativeName,
             &Flags
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: resolve clock %a/%a failed: %r\n",
            Controller, Id, Status));
    return Status;
  }
  if ((RateHz > MAX_UINT32) &&
      ((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = mNativeClock->GetClockID (mNativeClock, NativeName, &NativeId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: GetClockID %a/%a -> %a failed: %r\n",
            Controller, Id, NativeName, Status));
    return Status;
  }

  EfiAcquireLock (&mVoteLock);
  Status = GuardExternalNode (NativeId);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&mVoteLock);
    return Status;
  }
  Vote = FindVote (
           mClockVotes,
           CLOCK_CR_MAX_CLOCK_VOTES,
           NativeId,
           Enable
           );
  if (!Enable) {
    if (Vote == NULL) {
      EfiReleaseLock (&mVoteLock);
      return EFI_SUCCESS;
    }
    if (Vote->Uncertain) {
      Status = mNativeClock->IsClockEnabled (
                               mNativeClock,
                               NativeId,
                               &IsEnabled
                               );
      if (!EFI_ERROR (Status) && !IsEnabled) {
        Vote->References = 0;
        Vote->RateHz = 0;
        Vote->Uncertain = FALSE;
        Vote->CleanupPending = FALSE;
        Vote->FailureStatus = EFI_SUCCESS;
        EfiReleaseLock (&mVoteLock);
        return EFI_SUCCESS;
      }

      Status = Vote->FailureStatus;
      EfiReleaseLock (&mVoteLock);
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
    if (Vote->References > 1) {
      Vote->References--;
      EfiReleaseLock (&mVoteLock);
      return EFI_SUCCESS;
    }
    Status = mNativeClock->DisableClock (mNativeClock, NativeId);
    if (!EFI_ERROR (Status)) {
      DumpPcieClockState (NativeName, Vote->RateHz, FALSE);
      Vote->References = 0;
      Vote->RateHz = 0;
      Vote->CleanupPending = FALSE;
      Vote->FailureStatus = EFI_SUCCESS;
    } else {
      Vote->CleanupPending = TRUE;
      Vote->FailureStatus = Status;
      DEBUG ((DEBUG_ERROR,
              "ClockCrDxe: DisableClock %a/%a (id=0x%lx) failed: %r\n",
              Controller, Id, NativeId, Status));
    }
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  if (Vote == NULL) {
    EfiReleaseLock (&mVoteLock);
    return EFI_OUT_OF_RESOURCES;
  }
  if (Vote->References != 0) {
    if (Vote->Uncertain || Vote->CleanupPending) {
      Status = Vote->FailureStatus;
      EfiReleaseLock (&mVoteLock);
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
    if (Vote->References == MAX_UINT32) {
      EfiReleaseLock (&mVoteLock);
      return EFI_OUT_OF_RESOURCES;
    }
    if (((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0) &&
        (RateHz != 0) && (Vote->RateHz != (UINT32)RateHz)) {
      EfiReleaseLock (&mVoteLock);
      return EFI_ACCESS_DENIED;
    }
    /* A protocol reference shares the existing core lease. Revalidate its
       inherited fixed/read-only sources before reporting a new owner. */
    if (mNativeClock == &mCraneNativeClock) {
      CRANE_CLOCK_RATE *Rate = FindCraneRate ((ClockNode *)NativeId, FALSE);
      Status = CrStatusToEfi (ClockCheckAcquired (mClockContext,
          (ClockNode *)NativeId, Rate != NULL ? Rate->RateHz : 0));
      if (EFI_ERROR (Status)) {
        EfiReleaseLock (&mVoteLock);
        return Status;
      }
    }
    Vote->References++;
    if (Owned != NULL) *Owned = TRUE;
    EfiReleaseLock (&mVoteLock);
    return EFI_SUCCESS;
  }
  if ((RateHz != 0) &&
      ((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0)) {
    ResultRate = 0;
    Status = mNativeClock->SetClockFreqHz (
                             mNativeClock,
                             NativeId,
                             (UINT32)RateHz,
                             CraneClockFrequencyHzExact,
                             &ResultRate
                             );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
              "ClockCrDxe: SetClockFreqHz %a/%a (native=%a id=0x%lx rate=%u) failed: %r\n",
              Controller, Id, NativeName, NativeId,
              (UINT32)RateHz, Status));
      EfiReleaseLock (&mVoteLock);
      return Status;
    }
    if (ResultRate != (UINT32)RateHz) {
      EfiReleaseLock (&mVoteLock);
      return EFI_PROTOCOL_ERROR;
    }
  }
  if ((Flags & CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE) != 0 &&
      mNativeClock != &mCraneNativeClock) {
    Status = mNativeClock->SelectExternalSource (
                             mNativeClock,
                             NativeId,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0
                             );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
              "ClockCrDxe: SelectExternalSource %a/%a (native=%a id=0x%lx) failed: %r\n",
              Controller, Id, NativeName, NativeId, Status));
      EfiReleaseLock (&mVoteLock);
      return Status;
    }
  }
  Status = mNativeClock->IsClockEnabled (
                           mNativeClock,
                           NativeId,
                           &WasEnabled
                           );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: IsClockEnabled %a/%a (native=%a id=0x%lx) failed: %r\n",
            Controller, Id, NativeName, NativeId, Status));
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  CoreOwned = FALSE;
  if (mNativeClock == &mCraneNativeClock) {
    CRANE_CLOCK_RATE *Rate = FindCraneRate ((ClockNode *)NativeId, FALSE);
    if ((Flags & CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE) != 0) {
      EnableStatus = CrStatusToEfi (ClockAcquireExternal (mClockContext,
          (ClockNode *)NativeId, Rate != NULL ? Rate->RateHz : 0, 0, &CoreOwned));
    } else {
      EnableStatus = CrStatusToEfi (ClockAcquire (mClockContext,
          (ClockNode *)NativeId, Rate != NULL ? Rate->RateHz : 0, &CoreOwned));
    }
  } else {
    EnableStatus = mNativeClock->EnableClock (mNativeClock, NativeId);
  }
  if (EFI_ERROR (EnableStatus)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: EnableClock %a/%a (native=%a id=0x%lx) failed: %r\n",
            Controller, Id, NativeName, NativeId, EnableStatus));
    Status = mNativeClock->IsClockEnabled (
                             mNativeClock,
                             NativeId,
                             &IsEnabled
                             );
    if (CoreOwned || (mNativeClock != &mCraneNativeClock &&
        !WasEnabled && (EFI_ERROR (Status) || IsEnabled))) {
      /*
       * A failed synchronous enable normally carries no native reference.
       * If its observable state changed, retain an uncertain local owner so
       * rollback cannot silently disable a pre-existing or unowned vote.
       */
      Vote->NativeId = NativeId;
      Vote->References = 1;
      Vote->RateHz = ((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0) ?
                     (UINT32)RateHz : 0;
      /* The shared core reports an exact cleanup reference; the old
         compatibility backend can report only uncertain observable state. */
      Vote->Uncertain = !CoreOwned;
      Vote->CleanupPending = TRUE;
      Vote->FailureStatus = EnableStatus;
      if (Owned != NULL) *Owned = TRUE;
    }
    EfiReleaseLock (&mVoteLock);
    return EnableStatus;
  }

  Vote->NativeId   = NativeId;
  Vote->References = 1;
  Vote->RateHz = ((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0) ?
                 (UINT32)RateHz : 0;
  Vote->Uncertain = FALSE;
  Vote->CleanupPending = FALSE;
  Vote->FailureStatus = EFI_SUCCESS;
  if (Owned != NULL) *Owned = TRUE;
  DumpPcieClockState (NativeName, Vote->RateHz, TRUE);
  EfiReleaseLock (&mVoteLock);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI ProtocolSetClock (EFI_CLOCK_CR_PROTOCOL *This,
  CONST CHAR8 *Controller, CONST CHAR8 *Id, UINT64 RateHz, BOOLEAN Enable)
{
  return ProtocolSetClockInternal (This, Controller, Id, RateHz, Enable, NULL);
}

STATIC EFI_STATUS EFIAPI ProtocolAcquireClock (EFI_CLOCK_CR_PROTOCOL *This,
  CONST CHAR8 *Controller, CONST CHAR8 *Id, UINT64 RateHz, BOOLEAN *Owned)
{
  if (Owned == NULL) return EFI_INVALID_PARAMETER;
  return ProtocolSetClockInternal (This, Controller, Id, RateHz, TRUE, Owned);
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetGdscInternal (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN BOOLEAN                Enable,
  OUT BOOLEAN               *Owned OPTIONAL
  )
{
  CONST CHAR8    *NativeName;
  CLOCK_CR_VOTE  *Vote;
  EFI_STATUS      Status;
  UINT32          Flags;
  UINTN           NativeId;
  BOOLEAN         CoreOwned;

  if (Owned != NULL) {
    if (!CrGpuBootOutputAllowed(&mGpuBoot,Owned,sizeof(*Owned))) return EFI_INVALID_PARAMETER;
    *Owned = FALSE;
  }

  if ((This == NULL) || (Controller == NULL) || (Id == NULL) || Enable > 1) {
    return EFI_INVALID_PARAMETER;
  }
  if (mNativeClock == NULL) {
    return EFI_NOT_READY;
  }

  Status = ResolveNativeResource (
             Controller,
             Id,
             CrDalClockResourcePowerDomain,
             &NativeName,
             &Flags
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: resolve GDSC %a/%a failed: %r\n",
            Controller, Id, Status));
    return Status;
  }
  Status = mNativeClock->GetClockPowerDomainID (
                           mNativeClock,
                           NativeName,
                           &NativeId
                           );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: GetClockPowerDomainID %a/%a -> %a failed: %r\n",
            Controller, Id, NativeName, Status));
    return Status;
  }

  EfiAcquireLock (&mVoteLock);
  Status = GuardExternalNode (NativeId);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&mVoteLock);
    return Status;
  }
  Vote = FindVote (
           mPowerDomainVotes,
           CLOCK_CR_MAX_POWER_DOMAIN_VOTES,
           NativeId,
           Enable
           );
  if (!Enable) {
    if (Vote == NULL) {
      EfiReleaseLock (&mVoteLock);
      return EFI_SUCCESS;
    }
    if (Vote->References > 1) {
      Vote->References--;
      EfiReleaseLock (&mVoteLock);
      return EFI_SUCCESS;
    }
    Status = mNativeClock->DisableClockPowerDomain (mNativeClock, NativeId);
    if (!EFI_ERROR (Status)) {
      Vote->References = 0;
      Vote->CleanupPending = FALSE;
      Vote->FailureStatus = EFI_SUCCESS;
    } else {
      Vote->CleanupPending = TRUE;
      Vote->FailureStatus = Status;
      DEBUG ((DEBUG_ERROR,
              "ClockCrDxe: DisableClockPowerDomain %a/%a (native=%a id=0x%lx) failed: %r\n",
              Controller, Id, NativeName, NativeId, Status));
    }
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  if (Vote == NULL) {
    EfiReleaseLock (&mVoteLock);
    return EFI_OUT_OF_RESOURCES;
  }
  if (Vote->References != 0) {
    if (Vote->CleanupPending) {
      Status = Vote->FailureStatus;
      EfiReleaseLock (&mVoteLock);
      return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    }
    if (Vote->References == MAX_UINT32) {
      EfiReleaseLock (&mVoteLock);
      return EFI_OUT_OF_RESOURCES;
    }
    Vote->References++;
    if (Owned != NULL) *Owned = TRUE;
    EfiReleaseLock (&mVoteLock);
    return EFI_SUCCESS;
  }
  CoreOwned = FALSE;
  if (mNativeClock == &mCraneNativeClock) {
    Status = CrStatusToEfi (ClockGdscAcquire (mClockContext,
                            (ClockNode *)NativeId, &CoreOwned));
  } else {
    Status = mNativeClock->EnableClockPowerDomain (mNativeClock, NativeId);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: EnableClockPowerDomain %a/%a (native=%a id=0x%lx) failed: %r\n",
            Controller, Id, NativeName, NativeId, Status));
    if (CoreOwned) {
      Vote->NativeId = NativeId;
      Vote->References = 1;
      Vote->CleanupPending = TRUE;
      Vote->FailureStatus = Status;
      if (Owned != NULL) *Owned = TRUE;
    }
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  Vote->NativeId   = NativeId;
  Vote->References = 1;
  Vote->RateHz = 0;
  Vote->Uncertain = FALSE;
  Vote->CleanupPending = FALSE;
  Vote->FailureStatus = EFI_SUCCESS;
  if (Owned != NULL) *Owned = TRUE;
  EfiReleaseLock (&mVoteLock);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI ProtocolSetGdsc (EFI_CLOCK_CR_PROTOCOL *This,
  CONST CHAR8 *Controller, CONST CHAR8 *Id, BOOLEAN Enable)
{
  return ProtocolSetGdscInternal (This, Controller, Id, Enable, NULL);
}

STATIC EFI_STATUS EFIAPI ProtocolAcquireGdsc (EFI_CLOCK_CR_PROTOCOL *This,
  CONST CHAR8 *Controller, CONST CHAR8 *Id, BOOLEAN *Owned)
{
  if (Owned == NULL) return EFI_INVALID_PARAMETER;
  return ProtocolSetGdscInternal (This, Controller, Id, TRUE, Owned);
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetReset (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN BOOLEAN                Assert
  )
{
  CONST CHAR8 *NativeName;
  EFI_STATUS   Status;
  UINT32       Flags;
  UINTN        NativeId;

  if ((This == NULL) || (Controller == NULL) || (Id == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mNativeClock == NULL) {
    return EFI_NOT_READY;
  }

  /* Resets are target resources rather than clock nodes.  Keep this call on
   * the native Crane target path; the compatibility ABI still exposes the
   * same SetReset entry point to consumers. */
  if (mNativeClock == &mCraneNativeClock) {
    EfiAcquireLock (&mVoteLock);
    /* Target reset table currently accepts only unrelated GCC blocks. Keep
       future DISPCC reset admission tied to the same exclusive reservation. */
    if (CrGpuBootResetGuard(&mGpuBoot,Controller,Id)!=EFI_SUCCESS)
      Status=EFI_ACCESS_DENIED;
    else if (mExternalLease.External.Owner != NULL && AsciiStrCmp (Controller,"dispcc") == 0)
      Status = EFI_ACCESS_DENIED;
    else
      Status = CrStatusToEfi (CrDalClockReset (Controller, Id, Assert));
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  Status = ResolveNativeResource (
             Controller,
             Id,
             CrDalClockResourceReset,
             &NativeName,
             &Flags
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: resolve reset %a/%a failed: %r\n",
            Controller, Id, Status));
    return Status;
  }
  Status = mNativeClock->GetClockID (mNativeClock, NativeName, &NativeId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: GetClockID reset %a/%a -> %a failed: %r\n",
            Controller, Id, NativeName, Status));
    return Status;
  }
  EfiAcquireLock (&mVoteLock);
  Status = mNativeClock->ResetClock (
                           mNativeClock,
                           NativeId,
                           Assert ? CraneClockResetAssert : CraneClockResetDeassert
                           );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "ClockCrDxe: ResetClock %a/%a -> %a (id=0x%lx assert=%u) failed: %r\n",
            Controller, Id, NativeName, NativeId, Assert, Status));
  }
  EfiReleaseLock (&mVoteLock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolConfigureExternalSource (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN UINT32                 RateHz,
  IN UINT32                 Source,
  IN UINT32                 Divider,
  IN UINT32                 M,
  IN UINT32                 N,
  IN UINT32                 TwiceD
  )
{
  CONST CHAR8   *NativeName;
  CLOCK_CR_VOTE *Vote;
  EFI_STATUS     Status;
  UINT32         Flags;
  UINTN          NativeId;
  BOOLEAN        Enabled;

  if ((This == NULL) || (Controller == NULL) || (Id == NULL) ||
      (Divider == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mNativeClock == NULL) {
    return EFI_NOT_READY;
  }
  Status = ResolveNativeResource (Controller, Id, CrDalClockResourceClock,
                                  &NativeName, &Flags);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = mNativeClock->GetClockID (mNativeClock, NativeName, &NativeId);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  EfiAcquireLock (&mVoteLock);
  Status = GuardExternalNode (NativeId);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&mVoteLock);
    return Status;
  }
  Vote = FindVote (mClockVotes, CLOCK_CR_MAX_CLOCK_VOTES, NativeId, FALSE);
  if ((Vote != NULL) && (Vote->References != 0)) {
    EfiReleaseLock (&mVoteLock);
    return EFI_ACCESS_DENIED;
  }
  Status = mNativeClock->IsClockEnabled (mNativeClock, NativeId, &Enabled);
  if (!EFI_ERROR (Status) && Enabled) {
    Status = EFI_ACCESS_DENIED;
  } else if (!EFI_ERROR (Status)) {
    Status = mNativeClock->SelectExternalSource (
                             mNativeClock, NativeId, RateHz, Source, Divider,
                             M, N, TwiceD);
  }
  EfiReleaseLock (&mVoteLock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetDivider (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN UINT32                 Divider
  )
{
  CONST CHAR8   *NativeName;
  CLOCK_CR_VOTE *Vote;
  EFI_STATUS     Status;
  UINT32         Flags;
  UINTN          NativeId;
  BOOLEAN        Enabled;

  if ((This == NULL) || (Controller == NULL) || (Id == NULL) ||
      (Divider == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mNativeClock == NULL) {
    return EFI_NOT_READY;
  }
  Status = ResolveNativeResource (Controller, Id, CrDalClockResourceClock,
                                  &NativeName, &Flags);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = mNativeClock->GetClockID (mNativeClock, NativeName, &NativeId);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  EfiAcquireLock (&mVoteLock);
  Status = GuardExternalNode (NativeId);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&mVoteLock);
    return Status;
  }
  Vote = FindVote (mClockVotes, CLOCK_CR_MAX_CLOCK_VOTES, NativeId, FALSE);
  if ((Vote != NULL) && (Vote->References != 0)) {
    EfiReleaseLock (&mVoteLock);
    return EFI_ACCESS_DENIED;
  }
  Status = mNativeClock->IsClockEnabled (mNativeClock, NativeId, &Enabled);
  if (!EFI_ERROR (Status) && Enabled) {
    Status = EFI_ACCESS_DENIED;
  } else if (!EFI_ERROR (Status)) {
    Status = mNativeClock->SetClockDivider (mNativeClock, NativeId, Divider);
  }
  EfiReleaseLock (&mVoteLock);
  return Status;
}

STATIC EFI_STATUS EFIAPI
ProtocolSetParent (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN UINT32                 Source
  )
{
  CONST CHAR8   *NativeName;
  ClockNode     *Node;
  ClockNode     *Mux;
  CLOCK_CR_VOTE *Vote;
  EFI_STATUS     Status;
  UINT32         Flags;

  if ((This == NULL) || (Controller == NULL) || (Id == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((mClockContext == NULL) || (mNativeClock != &mCraneNativeClock)) {
    return EFI_NOT_READY;
  }
  Status = ResolveNativeResource (Controller, Id, CrDalClockResourceClock,
                                  &NativeName, &Flags);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CrStatusToEfi (GetClockNode (
                          mClockContext, NativeName, NULL, NULL, &Node));
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Mux = Node;
  if ((Node->ParentCount == 1) && (Node->Parents != NULL)) {
    Mux = Node->Parents[0];
  }
  if ((Mux == NULL) || (Mux->Type != CLOCK_NODE_TYPE_MUX)) {
    return EFI_UNSUPPORTED;
  }
  EfiAcquireLock (&mVoteLock);
  Status = GuardExternalNode ((UINTN)Node);
  if (!EFI_ERROR (Status)) Status = GuardExternalNode ((UINTN)Mux);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&mVoteLock);
    return Status;
  }
  Vote = FindVote (mClockVotes, CLOCK_CR_MAX_CLOCK_VOTES, (UINTN)Node, FALSE);
  /* A live mux change must not alter a second consumer's source. */
  if (((Vote != NULL) && (Vote->Uncertain || Vote->CleanupPending || (Vote->References > 1))) ||
      (Node->ReferenceCount > 1) || (Mux->ReferenceCount > 1)) {
    Status = EFI_ACCESS_DENIED;
  } else {
    Status = CraneClockSelectSource (
               &mCraneNativeClock, (UINTN)Node, 0, Source, 0, 0, 0, 0);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ClockCrDxe: parent %a/%a source=%u failed: %r\n",
            Controller, Id, Source, Status));
  }
  EfiReleaseLock (&mVoteLock);
  return Status;
}

STATIC EFI_CLOCK_CR_PROTOCOL mClockCrProtocol = {
  .Revision = EFI_CLOCK_CR_PROTOCOL_REVISION,
  .SetClock = ProtocolSetClock,
  .SetGdsc  = ProtocolSetGdsc,
  .SetReset = ProtocolSetReset,
  .ConfigureExternalSource = ProtocolConfigureExternalSource,
  .SetDivider = ProtocolSetDivider,
  .SetParent = ProtocolSetParent,
  .AcquireClock = ProtocolAcquireClock,
  .AcquireGdsc = ProtocolAcquireGdsc,
};

EFI_STATUS
EFIAPI
ClockEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)SystemTable;
  mClockContext = CrDalGetClockContext ();
  if (mClockContext == NULL) {
    DEBUG ((DEBUG_ERROR, "ClockCrDxe: Crane clock context unavailable\n"));
    return EFI_NOT_FOUND;
  }
  Status = CrStatusToEfi (ClockLibInit (&mClockContext));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ClockCrDxe: native ClockLibInit failed: %r\n", Status));
    mClockContext = NULL;
    return Status;
  }
  Status = CrStatusToEfi (CrDalClockInit (mClockContext));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ClockCrDxe: native target clock init failed: %r\n", Status));
    mClockContext = NULL;
    return Status;
  }
  mNativeClock = &mCraneNativeClock;
  if ((mNativeClock == NULL) ||
      (mNativeClock->Version < CRANE_CLOCK_BACKEND_REVISION) ||
      (mNativeClock->GetClockID == NULL) ||
      (mNativeClock->EnableClock == NULL) ||
      (mNativeClock->DisableClock == NULL) ||
      (mNativeClock->IsClockEnabled == NULL) ||
      (mNativeClock->SetClockFreqHz == NULL) ||
      (mNativeClock->SelectExternalSource == NULL) ||
      (mNativeClock->SetClockDivider == NULL) ||
      (mNativeClock->GetClockPowerDomainID == NULL) ||
      (mNativeClock->EnableClockPowerDomain == NULL) ||
      (mNativeClock->DisableClockPowerDomain == NULL) ||
      (mNativeClock->ResetClock == NULL)) {
    mNativeClock = NULL;
    return EFI_INCOMPATIBLE_VERSION;
  }

  CONST CR_MDSS_TARGET *DisplayTarget = CrDalGetMdssContext();
  Status = CrStatusToEfi (DisplayTarget ?
      CrClockExternalLeaseInitForSoc(&mExternalLease,mClockContext,DisplayTarget->Soc) :
      CrClockExternalLeaseInit(&mExternalLease,mClockContext));
  if (!EFI_ERROR (Status)) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &ImageHandle, &gEfiClockCrProtocolGuid, &mClockCrProtocol,
                    &gEfiClockExternalCrProtocolGuid, &mExternalProtocol, NULL);
  } else {
    /* Other target graphs retain their existing clock protocol; the selected
       external-plan service is not published on an unqualified graph. */
    DEBUG ((DEBUG_WARN, "ClockCrDxe: external plan service unavailable: %r\n", Status));
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &ImageHandle, &gEfiClockCrProtocolGuid, &mClockCrProtocol, NULL);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ClockCrDxe: protocol installation failed: %r\n", Status));
    mNativeClock = NULL;
    mClockContext = NULL;
  }
  if (Status==EFI_SUCCESS && CrGpuBootClockInit(&mGpuBoot,mClockContext,&mVoteLock)==EFI_SUCCESS) {
    EFI_STATUS GpuStatus=gBS->InstallMultipleProtocolInterfaces(&ImageHandle,&mGpuBootGuid,&mGpuBoot.Protocol,NULL);
    if (GpuStatus!=EFI_SUCCESS) DEBUG((DEBUG_WARN,"ClockCrDxe: private GPU access service absent: %r\n",GpuStatus));
  }
  return Status;
}
