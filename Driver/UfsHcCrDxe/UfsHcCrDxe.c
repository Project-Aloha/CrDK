/**
  Qualcomm target-configured UFS host-controller adapter.

  This module supplies the standard MU/EDKII UFS host-controller and platform
  protocols for the non-PCI UFS controller described by the HDK8450 Linux DT.
  The actual UFS command engine remains the open-source MU UfsPassThruDxe
  implementation; this file only provides the MMIO and DMA contract that its
  HCI code consumes.

  Register ranges and resources follow Linux sm8450.dtsi and sm8450-hdk.dts;
  RPMH_CXO_CLK is the board's 19.2 MHz reference. PHY calibration and host
  sequencing follow Linux phy-qcom-qmp-ufs.c and Android ufs-qcom.c.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/IoMmu.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>

#define CR_UFS_HCI_BASE (mUfs.Target->HciBase)
#define CR_UFS_HCI_LENGTH (mUfs.Target->HciBytes)
#define CR_UFS_REFCLK_19P2    EdkiiUfsCardRefClkFreq19p2Mhz


#define CR_UFS_PHY_BASE (mUfs.Target->PhyBase)
#define CR_UFS_PHY_READY (mUfs.Target->PhyReady)
#define CR_UFS_PHY_PWR_CTRL (mUfs.Target->PhyPower)
#define CR_UFS_PHY_SW_RESET (mUfs.Target->PhySwReset)
#define CR_UFS_PHY_START (mUfs.Target->PhyStart)
#define CR_UFS_PHY_LINECFG_DISABLE (mUfs.Target->PhyLineCfg)
#define CR_UFS_PHY_RX_LINECFG_DISABLE BIT1
#define CR_UFS_CFG1            0x00DCU
#define CR_UFS_PHY_SOFT_RESET  BIT1
#define CR_UFS_QUNIPRO_SEL     BIT0
#define CR_UFS_DEV_REFCLK_EN   BIT26
#define CR_UFS_PARAM0          0x00D0U
#define CR_UFS_GDSC_ID (mUfs.Target->Gdsc)
#define CR_UFS_CONTROLLER (mUfs.Target->ClockController)
#define CR_UFS_SMMU_BASE (mUfs.Target->SmmuBase)
#define CR_UFS_DEVICE_RESET_GPIO (mUfs.Target->DeviceReset.PinNumber)
#define CR_UFS_MAX_LANES (mUfs.Target->Lanes)
/* Public Android/Linux ufshci.h INT_FATAL_ERRORS, excluding generic UIC_ERROR. */
#define CR_UFS_FATAL_ERRORS (BIT7 | BIT11 | BIT12 | BIT16 | BIT17 | BIT18)

typedef struct CR_UFS_MAPPING {
  EDKII_IOMMU_PROTOCOL                  *Iommu;
  VOID                                  *IommuMapping;
  struct CR_UFS_MAPPING                  *PendingNext;
  BOOLEAN                               Pending;
} CR_UFS_MAPPING;

/* SM8450 DT boot/max rate. Linux lowers the HS gear before scaling down. */
#define CR_UFS_CORE_RATE_HZ  300000000U
#define CR_UFS_CYCLES_US    (CR_UFS_CORE_RATE_HZ / 1000000U)
#define CR_UFS_CYCLES_40NS  12U

typedef struct {
  VENDOR_DEVICE_PATH                  Vendor;
  EFI_DEVICE_PATH_PROTOCOL            End;
} CR_UFS_DEVICE_PATH;

typedef struct {
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL   Host;
  EDKII_UFS_HC_PLATFORM_PROTOCOL       Platform;
  EFI_HANDLE                           Handle;
  CR_UFS_DEVICE_PATH                   DevicePath;
  EFI_INTERCONNECT_CR_PROTOCOL         *Interconnect;
  EFI_GPIO_CR_PROTOCOL                 *Gpio;
  EFI_SMMU_CR_PROTOCOL                 *Smmu;
  EDKII_IOMMU_PROTOCOL                  *Iommu;
  INTERCONNECT_PATH_HANDLE             MemPath;
  INTERCONNECT_PATH_HANDLE             CfgPath;
  BOOLEAN                              InterconnectReady;
  UINT32                               SmmuAttached;
  CONST CR_TARGET_UFS_CONTEXT          *Target;
  BOOLEAN                              ResourcesReady;
  BOOLEAN                              DmaPolicyLogged;
  EFI_CLOCK_CR_PROTOCOL                *Clock;
  EFI_RPMH_CR_PROTOCOL                 *Rpmh;
  UINTN                                ClockCount;
  BOOLEAN                              RailRequested[CR_TARGET_UFS_MAX_RAILS];
  BOOLEAN                              GdscEnabled;
  BOOLEAN                              PhyPowered;
  BOOLEAN                              CoreResetAsserted;
  BOOLEAN                              CoreResetCompleted;
  BOOLEAN                              CleanupPending;
  BOOLEAN                              Initializing;
  BOOLEAN                              HceAttempted;
  UINT32                               MaxHsGear;
  CR_UFS_MAPPING                      *PendingMapping;
  BOOLEAN                              ExitObserversRegistered;
  BOOLEAN                              ExitStarted;
  BOOLEAN                              ExitComplete;
  EFI_STATUS                           ExitStopStatus;
  UINT32                               FatalError;
} CR_UFS_PRIVATE;

STATIC CR_UFS_PRIVATE  mUfs;

STATIC EFI_STATUS CrUfsInitializeResources (VOID);

/* Warnings do not establish a resource or DMA grant. The protocol caller
   must see a failure, and attempted effects stay owned until final cleanup. */
STATIC EFI_STATUS CrUfsFinalStatus (EFI_STATUS Status)
{
  return (Status == EFI_SUCCESS || EFI_ERROR (Status)) ? Status : EFI_DEVICE_ERROR;
}

/* The default HDK DSDT exposes _CCA=1 without an OS IORT route. StorUfs
   therefore submits physical addresses. Keep translated DMA throughout UEFI,
   then quiesce before the SMMU producer revokes and changes only this stream.
   All MU HCI accesses pass through this adapter, including its async timer;
   ExitStarted closes those accesses before any queue or HCE write below. */
STATIC BOOLEAN CrUfsExitStopped (VOID)
{
  return MmioRead32 (CR_UFS_HCI_BASE + 0x24) == 0 &&
         (MmioRead32 (CR_UFS_HCI_BASE + 0x34) & BIT0) == 0 &&
         (MmioRead32 (CR_UFS_HCI_BASE + 0x60) & BIT0) == 0 &&
         (MmioRead32 (CR_UFS_HCI_BASE + 0x80) & BIT0) == 0 &&
         MmioRead32 (CR_UFS_HCI_BASE + 0x58) == 0 &&
         MmioRead32 (CR_UFS_HCI_BASE + 0x78) == 0;
}

STATIC EFI_STATUS EFIAPI CrUfsPrepareExit (VOID *Context)
{
  if (Context != &mUfs) return EFI_INVALID_PARAMETER;
  if (mUfs.ExitStarted) return mUfs.ExitStopStatus;
  mUfs.ExitStopStatus = EFI_NOT_READY;
  mUfs.ExitStarted = TRUE;
  MemoryFence ();
  if (!mUfs.ResourcesReady || mUfs.Initializing || mUfs.CleanupPending ||
      mUfs.SmmuAttached != mUfs.Target->SidCount) return mUfs.ExitStopStatus;

  MmioWrite32 (CR_UFS_HCI_BASE + 0x24, 0);  // Interrupt enable
  MmioWrite32 (CR_UFS_HCI_BASE + 0x60, 0);  // Transfer list run/stop
  MmioWrite32 (CR_UFS_HCI_BASE + 0x80, 0);  // Task list run/stop
  MmioWrite32 (CR_UFS_HCI_BASE + 0x34, 0);  // HCE stops outstanding DMA
  MemoryFence ();
  mUfs.ExitStopStatus = EFI_TIMEOUT;
  for (UINTN Retry = 0; Retry < 1000; ++Retry) {
    if (CrUfsExitStopped ()) {
      mUfs.ExitStopStatus = EFI_SUCCESS;
      break;
    }
    MicroSecondDelay (10);
  }
  DEBUG ((DEBUG_WARN, "Crane UFS: EBS stop=%r HCE=%x IE=%x RSR=%x/%x DBR=%x/%x\n",
          mUfs.ExitStopStatus, MmioRead32 (CR_UFS_HCI_BASE + 0x34),
          MmioRead32 (CR_UFS_HCI_BASE + 0x24),
          MmioRead32 (CR_UFS_HCI_BASE + 0x60), MmioRead32 (CR_UFS_HCI_BASE + 0x80),
          MmioRead32 (CR_UFS_HCI_BASE + 0x58), MmioRead32 (CR_UFS_HCI_BASE + 0x78)));
  return mUfs.ExitStopStatus;
}

STATIC VOID EFIAPI CrUfsCompleteExit (VOID *Context, EFI_STATUS RevokeStatus)
{
  EFI_STATUS Status = EFI_NOT_READY;
  (VOID)RevokeStatus; // The producer independently verifies this exact domain.
  if (Context != &mUfs || mUfs.ExitComplete) return;
  mUfs.ExitComplete = TRUE;
  if (mUfs.ExitStarted && mUfs.ExitStopStatus == EFI_SUCCESS && CrUfsExitStopped ())
    Status = mUfs.Smmu->ReleaseUfsToCoherentPhysicalAtExit (mUfs.Smmu);
  DEBUG ((DEBUG_WARN, "Crane UFS: legacy coherent physical DMA handoff: %r\n", Status));
}

STATIC EFI_STATUS CrUfsRegisterLegacyExit (VOID)
{
  EFI_STATUS Status;
  if (!FixedPcdGetBool (PcdUfsLegacyCoherentDmaHandoff)) return EFI_SUCCESS;
  if (mUfs.ExitObserversRegistered) return EFI_SUCCESS;
  if (mUfs.Target->HciBase != 0x01D84000 ||
      mUfs.Target->SmmuBase != 0x15000000 || mUfs.Target->SidCount != 1 ||
      mUfs.Target->Sids[0] != 0xE0 ||
      mUfs.Smmu->Revision < EFI_SMMU_CR_EXIT_UFS_COHERENT_REVISION ||
      !mUfs.Smmu->SetExitPrepareObserver || !mUfs.Smmu->SetExitObserver ||
      !mUfs.Smmu->ReleaseUfsToCoherentPhysicalAtExit) return EFI_UNSUPPORTED;
  Status = mUfs.Smmu->SetExitPrepareObserver (mUfs.Smmu, &mUfs, CrUfsPrepareExit, &mUfs);
  if (Status != EFI_SUCCESS) return CrUfsFinalStatus (Status);
  Status = mUfs.Smmu->SetExitObserver (mUfs.Smmu, &mUfs, CrUfsCompleteExit, &mUfs);
  if (Status != EFI_SUCCESS) {
    // mUfs is permanent even if unregister fails; its callback remains valid.
    (VOID)mUfs.Smmu->SetExitPrepareObserver (mUfs.Smmu, &mUfs, NULL, NULL);
    return CrUfsFinalStatus (Status);
  }
  mUfs.ExitObserversRegistered = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS CrUfsResolveTarget (VOID)
{
  if (mUfs.Target == NULL) {
    CONST CR_TARGET_UFS_CONTEXT *Target = CrDalGetUfsContext ();
    if (!CrTargetUfsValid (Target)) return EFI_UNSUPPORTED;
    mUfs.Target = Target;
  }
  return EFI_SUCCESS;
}

/* TLMM's generic Set/Config callbacks wrap void register helpers. A successful
   callback alone does not establish pad configuration or the reset latch. */
STATIC EFI_STATUS CrUfsCheckReset (UINT8 Value, BOOLEAN CheckConfiguration)
{
  GpioConfigParams Actual = {
    .PinNumber = CR_UFS_DEVICE_RESET_GPIO, .OutputEnable = FALSE,
    .FunctionSel = GPIO_FUNC_UNCHANGE, .Pull = GPIO_PULL_UNCHANGE,
    .DriveStrength = GPIO_DRIVE_STRENGTH_UNCHANGE, .OutputValue = GPIO_VALUE_UNCHANGE
  };
  EFI_STATUS Status = mUfs.Gpio->GetGpioConfig (mUfs.Gpio, &Actual);
  if (Status != EFI_SUCCESS) return CrUfsFinalStatus (Status);
  if (Actual.PinNumber != CR_UFS_DEVICE_RESET_GPIO || !Actual.OutputEnable ||
      Actual.OutputValue != Value) {
    DEBUG ((DEBUG_ERROR, "Crane UFS: GPIO%u reset readback OE=%u value=%u expected=%u\n",
            CR_UFS_DEVICE_RESET_GPIO, Actual.OutputEnable, Actual.OutputValue, Value));
    return EFI_DEVICE_ERROR;
  }
  CONST GpioConfigParams *Expected = &mUfs.Target->DeviceReset;
  if (CheckConfiguration &&
      ((Expected->FunctionSel != GPIO_FUNC_UNCHANGE && Actual.FunctionSel != Expected->FunctionSel) ||
       (Expected->Pull != GPIO_PULL_UNCHANGE && Actual.Pull != Expected->Pull) ||
       (Expected->DriveStrength != GPIO_DRIVE_STRENGTH_UNCHANGE &&
        Actual.DriveStrength != Expected->DriveStrength))) {
    DEBUG ((DEBUG_ERROR, "Crane UFS: GPIO%u pad readback pull=%u drive=%u expected=%u/%u\n",
            CR_UFS_DEVICE_RESET_GPIO, Actual.Pull, Actual.DriveStrength,
            Expected->Pull, Expected->DriveStrength));
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI CrUfsVerifyStoppedOwner (VOID *Context)
{
  if (Context != &mUfs || !mUfs.Initializing || mUfs.ResourcesReady ||
      mUfs.CleanupPending || !mUfs.GdscEnabled || !mUfs.InterconnectReady ||
      !mUfs.PhyPowered || mUfs.ClockCount != mUfs.Target->ClockCount ||
      mUfs.CoreResetAsserted || !mUfs.CoreResetCompleted || mUfs.PendingMapping != NULL)
    return EFI_NOT_READY;
  EFI_STATUS Status = CrUfsCheckReset (0, TRUE);
  if (EFI_ERROR (Status)) return Status;
  /* Full HCI register zeros, read twice while resources are owned. A supplied
     boolean, reset latch alone or an unclocked all-zero window is insufficient. */
  STATIC CONST UINT16 Offsets[] = {0x24,0x34,0x60,0x80,0x58,0x78};
  for (UINTN Pass = 0; Pass < 2; Pass++) {
    for (UINTN Index = 0; Index < ARRAY_SIZE (Offsets); Index++)
      if (MmioRead32 (CR_UFS_HCI_BASE + Offsets[Index]) != 0) return EFI_ACCESS_DENIED;
    MemoryFence ();
  }
  return EFI_SUCCESS;
}

STATIC VOID CrUfsProgramPhy (CONST CR_TARGET_UFS_PHY_TABLE *Table)
{
  for (UINTN Index = 0; Index < Table->Count; ++Index)
    MmioWrite32 (CR_UFS_PHY_BASE + Table->Writes[Index].Offset,
                 Table->Writes[Index].Value);
}


/* MU cleanup ignores Unmap errors before FreeBuffer. Retain every failed
   revoke without allocating another object, and block new DMA/free until
   the complete quarantine can be revoked. */
STATIC VOID CrUfsQueueMapping (CR_UFS_MAPPING *Map)
{
  if (!Map->Pending) {
    Map->PendingNext = mUfs.PendingMapping;
    Map->Pending = TRUE;
    mUfs.PendingMapping = Map;
  }
}

STATIC VOID CrUfsForgetMapping (CR_UFS_MAPPING *Map)
{
  CR_UFS_MAPPING **Link;

  if (Map->Pending) {
    for (Link = &mUfs.PendingMapping; *Link != NULL;
         Link = &(*Link)->PendingNext) {
      if (*Link == Map) {
        *Link = Map->PendingNext;
        break;
      }
    }
  }
  FreePool (Map);
}

STATIC EFI_STATUS CrUfsRetryPendingMapping (VOID)
{
  CR_UFS_MAPPING *Map;
  EFI_STATUS Status;

  while ((Map = mUfs.PendingMapping) != NULL) {
    Status = Map->Iommu->Unmap (Map->Iommu, Map->IommuMapping);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    CrUfsForgetMapping (Map);
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CrUfsGetMmioBar (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT UINTN                                *MmioBar
  )
{
  EFI_STATUS Status;

  if ((This == NULL) || (MmioBar == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *MmioBar = 0;
  /* MU reads VER/CAP before its PreHce callback. Power up before returning
     an MMIO address, including callers that access that address directly. */
  Status = CrUfsInitializeResources ();
  if (Status != EFI_SUCCESS) {
    return CrUfsFinalStatus (Status);
  }
  *MmioBar = (UINTN)CR_UFS_HCI_BASE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CrUfsAllocateBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  EFI_ALLOCATE_TYPE                   Type,
  IN  EFI_MEMORY_TYPE                     MemoryType,
  IN  UINTN                               Pages,
  OUT VOID                                **HostAddress,
  IN  UINT64                              Attributes
  )
{
  EFI_STATUS Status;

  /* Type is explicitly ignored by EDKII_UFS_HOST_CONTROLLER_PROTOCOL. */
  (VOID)Type;
  if (HostAddress != NULL) {
    *HostAddress = NULL;
  }
  if ((This == NULL) || (HostAddress == NULL) || (Pages == 0) ||
      (Pages > MAX_UINTN / EFI_PAGE_SIZE) ||
      ((MemoryType != EfiBootServicesData) &&
       (MemoryType != EfiRuntimeServicesData))) {
    return EFI_INVALID_PARAMETER;
  }
  /* Like UfsPciHcDxe, use PCI allocation attribute bits, not GCD cache bits. */
  if ((Attributes & ~(EDKII_IOMMU_ATTRIBUTE_MEMORY_WRITE_COMBINE |
                      EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED)) != 0) {
    return EFI_UNSUPPORTED;
  }
  if (mUfs.FatalError != 0) return EFI_DEVICE_ERROR;
  Status = CrUfsInitializeResources ();
  if (Status != EFI_SUCCESS) {
    return CrUfsFinalStatus (Status);
  }
  if (mUfs.Target->DmaCoherent) {
    if ((Attributes & EDKII_IOMMU_ATTRIBUTE_MEMORY_WRITE_COMBINE) != 0) {
      return EFI_UNSUPPORTED;
    }
    /* Match coherent SMMU translation with cacheable CPU storage, as in
       Android dma_alloc_coherent. A failed WB allocation must not fall back
       to NC storage behind a WB device mapping. */
    Attributes |= EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED;
  }
  Status = mUfs.Iommu->AllocateBuffer (
             mUfs.Iommu, AllocateAnyPages, MemoryType, Pages, HostAddress,
             Attributes);
  if (Status == EFI_SUCCESS) {
    ZeroMem (*HostAddress, EFI_PAGES_TO_SIZE (Pages));
    if (mUfs.Target->DmaCoherent && !mUfs.DmaPolicyLogged) {
      DEBUG ((DEBUG_INFO, "Crane UFS: common-buffer policy=CPU-WB coherent DMA\n"));
      mUfs.DmaPolicyLogged = TRUE;
    }
  }
  return CrUfsFinalStatus (Status);
}

STATIC
EFI_STATUS
EFIAPI
CrUfsFreeBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN UINTN Pages,
  IN VOID *HostAddress
  )
{
  EFI_STATUS Status;
  if (mUfs.ExitStarted) return EFI_ACCESS_DENIED;
  if ((This == NULL) || (HostAddress == NULL) || (Pages == 0) ||
      (Pages > MAX_UINTN / EFI_PAGE_SIZE)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mUfs.Iommu == NULL) {
    return EFI_NOT_READY;
  }
  Status = CrUfsRetryPendingMapping ();
  if (Status != EFI_SUCCESS) {
    return CrUfsFinalStatus (Status);
  }
  return CrUfsFinalStatus (mUfs.Iommu->FreeBuffer (mUfs.Iommu, Pages, HostAddress));
}

STATIC
EFI_STATUS
EFIAPI
CrUfsMap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN EDKII_UFS_HOST_CONTROLLER_OPERATION Operation,
  IN VOID *HostAddress,
  IN OUT UINTN *NumberOfBytes,
  OUT EFI_PHYSICAL_ADDRESS *DeviceAddress,
  OUT VOID **Mapping
  )
{
  CR_UFS_MAPPING *Map;
  EDKII_IOMMU_OPERATION IommuOperation;
  UINT64 Access;
  UINTN RequestedBytes;
  EFI_STATUS Status;

  if (Mapping != NULL) {
    *Mapping = NULL;
  }
  if ((This == NULL) || (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (*NumberOfBytes == 0) || (DeviceAddress == NULL) || (Mapping == NULL) ||
      ((UINT32)Operation >= EdkiiUfsHcOperationMaximum)) {
    return EFI_INVALID_PARAMETER;
  }
  *DeviceAddress = 0;
  if (mUfs.FatalError != 0) return EFI_DEVICE_ERROR;
  RequestedBytes = *NumberOfBytes;
  Status = CrUfsInitializeResources ();
  if (Status != EFI_SUCCESS) {
    return CrUfsFinalStatus (Status);
  }

  /* BusMasterRead means the DEVICE reads memory. SmmuCrDxe performs the
     cache clean/bounce before reads, and invalidation/copyback after writes.
     Never fall back to physical addresses behind an attached SMMU stream. */
  switch (Operation) {
    case EdkiiUfsHcOperationBusMasterRead:
      IommuOperation = EdkiiIoMmuOperationBusMasterRead64;
      Access = EDKII_IOMMU_ACCESS_READ;
      break;
    case EdkiiUfsHcOperationBusMasterWrite:
      IommuOperation = EdkiiIoMmuOperationBusMasterWrite64;
      Access = EDKII_IOMMU_ACCESS_WRITE;
      break;
    default:
      IommuOperation = EdkiiIoMmuOperationBusMasterCommonBuffer64;
      Access = EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE;
      break;
  }
  Map = AllocateZeroPool (sizeof (*Map));
  if (Map == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Map->Iommu = mUfs.Iommu;
  Status = Map->Iommu->Map (Map->Iommu, IommuOperation, HostAddress,
                           NumberOfBytes, DeviceAddress, &Map->IommuMapping);
  if (Status != EFI_SUCCESS || Map->IommuMapping == NULL) {
    /* Nonfinal Map can still return owned backing; revoke before discarding
       the token, and quarantine it if the provider cannot finish cleanup. */
    if (Map->IommuMapping != NULL &&
        Map->Iommu->Unmap (Map->Iommu, Map->IommuMapping) != EFI_SUCCESS)
      CrUfsQueueMapping (Map);
    else
      FreePool (Map);
    *DeviceAddress = 0;
    *NumberOfBytes = 0;
    return Status == EFI_SUCCESS ? EFI_DEVICE_ERROR : CrUfsFinalStatus (Status);
  }
  /* MU's UFS data path does not unmap a successful short mapping. Revoke it
     here so its caller never receives a live DMA mapping it will abandon. */
  if (*NumberOfBytes != RequestedBytes) {
    EFI_STATUS CleanupStatus;
    CleanupStatus = Map->Iommu->Unmap (Map->Iommu, Map->IommuMapping);
    if (CleanupStatus != EFI_SUCCESS) {
      DEBUG ((DEBUG_ERROR, "Crane UFS: short-map revoke failed: %r\n", CleanupStatus));
      CrUfsQueueMapping (Map);
    } else {
      FreePool (Map);
    }
    *DeviceAddress = 0;
    *NumberOfBytes = 0;
    return EFI_OUT_OF_RESOURCES;
  }
  /* Each configured stream needs the same exact mapping. Backend Unmap
     revokes all grants recorded on this token, including partial failure. */
  for (UINT32 Index = 0; Index < mUfs.Target->SidCount; ++Index) {
    Status = Map->Iommu->SetAttributeById (
               Map->Iommu, CR_UFS_SMMU_BASE, mUfs.Target->Sids[Index],
               Map->IommuMapping, Access);
    if (Status != EFI_SUCCESS) break;
  }
  if (Status != EFI_SUCCESS) {
    EFI_STATUS CleanupStatus;
    CleanupStatus = Map->Iommu->Unmap (Map->Iommu, Map->IommuMapping);
    if (CleanupStatus != EFI_SUCCESS) {
      /* The backend keeps failed revocations allocated. Do not report a
         usable DMA address or free a buffer that could remain reachable. */
      DEBUG ((DEBUG_ERROR, "Crane UFS: failed-map revoke failed: %r\n", CleanupStatus));
      CrUfsQueueMapping (Map);
    } else {
      FreePool (Map);
    }
    *DeviceAddress = 0;
    return CrUfsFinalStatus (Status);
  }
  *Mapping = Map;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CrUfsUnmap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN VOID *Mapping
  )
{
  CR_UFS_MAPPING *Map;
  EFI_STATUS Status;

  if (mUfs.ExitStarted) return EFI_ACCESS_DENIED;

  if ((This == NULL) || (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Map = Mapping;
  Status = Map->Iommu->Unmap (Map->Iommu, Map->IommuMapping);
  /* Published tokens remain owned by the caller on failure. Autoretrying
     them in InitializeResources/FreeBuffer could free a token the caller
     still holds, or copy data back after its buffer lifetime has ended. */
  if (Status == EFI_SUCCESS) CrUfsForgetMapping (Map);
  return CrUfsFinalStatus (Status);
}

STATIC
EFI_STATUS
EFIAPI
CrUfsFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  if (mUfs.ExitStarted) return EFI_ACCESS_DENIED;
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mUfs.ResourcesReady) {
    return EFI_NOT_READY;
  }
  MemoryFence ();
  /* A dependent read orders posted HCI writes on the ARM interconnect. */
  (VOID)MmioRead32 (CR_UFS_HCI_BASE);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CrUfsReadWrite (
  IN      EDKII_UFS_HOST_CONTROLLER_PROTOCOL         *This,
  IN      EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH   Width,
  IN      UINT64                                     Offset,
  IN      UINTN                                      Count,
  IN OUT  VOID                                       *Buffer,
  IN      BOOLEAN                                    Write
  )
{
  UINTN  ElementSize;
  UINTN  Index;
  UINT64 End;
  EFI_STATUS Status;

  if ((This == NULL) || (Buffer == NULL) || (Count == 0) ||
      ((UINT32)Width >= EfiUfsHcWidthMaximum)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CrUfsResolveTarget ();
  if (Status != EFI_SUCCESS) return CrUfsFinalStatus (Status);
  ElementSize = (UINTN)1 << Width;
  if ((Offset > CR_UFS_HCI_LENGTH) ||
      (Count > (MAX_UINT64 - Offset) / ElementSize)) {
    return EFI_UNSUPPORTED;
  }
  End = Offset + ((UINT64)Count * ElementSize);
  if (End > CR_UFS_HCI_LENGTH) {
    return EFI_UNSUPPORTED;
  }

  if ((Offset & (ElementSize - 1)) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CrUfsInitializeResources ();
  if (Status != EFI_SUCCESS) {
    return CrUfsFinalStatus (Status);
  }

  for (Index = 0; Index < Count; Index++) {
    UINTN Address = (UINTN)(CR_UFS_HCI_BASE + Offset + (Index * ElementSize));
    VOID *Element = (UINT8 *)Buffer + (Index * ElementSize);
    if (Write) {
      // Cleanup may stop HCE/queues and read back state. A failed controller
      // must not be restarted by another ConnectController/driver instance.
      if (mUfs.FatalError != 0 && Width == EfiUfsHcWidthUint32 &&
          ((Address == CR_UFS_HCI_BASE + 0x58 && *(UINT32 *)Element != 0) ||
           (Address == CR_UFS_HCI_BASE + 0x34 && (*(UINT32 *)Element & BIT0))))
        return EFI_DEVICE_ERROR;
      switch (Width) {
        case EfiUfsHcWidthUint8:  MmioWrite8  (Address, *(UINT8  *)Element); break;
        case EfiUfsHcWidthUint16: MmioWrite16 (Address, *(UINT16 *)Element); break;
        case EfiUfsHcWidthUint32: MmioWrite32 (Address, *(UINT32 *)Element); break;
        case EfiUfsHcWidthUint64: MmioWrite64 (Address, *(UINT64 *)Element); break;
        default: return EFI_INVALID_PARAMETER;
      }
    } else {
      switch (Width) {
        case EfiUfsHcWidthUint8:  *(UINT8  *)Element = MmioRead8  (Address); break;
        case EfiUfsHcWidthUint16: *(UINT16 *)Element = MmioRead16 (Address); break;
        case EfiUfsHcWidthUint32: *(UINT32 *)Element = MmioRead32 (Address); break;
        case EfiUfsHcWidthUint64: *(UINT64 *)Element = MmioRead64 (Address); break;
        default: return EFI_INVALID_PARAMETER;
      }
      if (Address == CR_UFS_HCI_BASE + 0x20 && Width == EfiUfsHcWidthUint32 &&
          (*(UINT32 *)Element & CR_UFS_FATAL_ERRORS) != 0 && mUfs.FatalError == 0) {
        UINTN SidIndex;
        mUfs.FatalError = *(UINT32 *)Element & CR_UFS_FATAL_ERRORS;
        DEBUG ((DEBUG_ERROR, "Crane UFS: fatal IS=%08x; DMA fault snapshot before cleanup\n",
                *(UINT32 *)Element));
        // Public Android ufs-qcom raw debug registers, shifted by 0x400
        // on HW v3+. Read the Waipio v4 AXI masters without enabling RAM
        // debug or changing HCI/fault state.
        UINT32 HwVersion = MmioRead32 (CR_UFS_HCI_BASE + 0xE4);
        DEBUG ((DEBUG_ERROR, "Crane UFS: fault HW=%08x CFG1=%08x CFG2=%08x AHIT=%08x\n",
                HwVersion, MmioRead32 (CR_UFS_HCI_BASE + 0xDC),
                MmioRead32 (CR_UFS_HCI_BASE + 0xE0), MmioRead32 (CR_UFS_HCI_BASE + 0x18)));
        if ((HwVersion >> 28) == 4) {
          DEBUG ((DEBUG_ERROR, "Crane UFS: UAWM[500] %08x %08x %08x %08x\n",
                  MmioRead32 (CR_UFS_HCI_BASE + 0x500), MmioRead32 (CR_UFS_HCI_BASE + 0x504),
                  MmioRead32 (CR_UFS_HCI_BASE + 0x508), MmioRead32 (CR_UFS_HCI_BASE + 0x50C)));
          DEBUG ((DEBUG_ERROR, "Crane UFS: UARM[600] %08x %08x %08x %08x\n",
                  MmioRead32 (CR_UFS_HCI_BASE + 0x600), MmioRead32 (CR_UFS_HCI_BASE + 0x604),
                  MmioRead32 (CR_UFS_HCI_BASE + 0x608), MmioRead32 (CR_UFS_HCI_BASE + 0x60C)));
        }
        for (SidIndex = 0; SidIndex < mUfs.SmmuAttached; SidIndex++) {
          EFI_STATUS FaultStatus = EFI_UNSUPPORTED;
          if (mUfs.Smmu->Revision >= EFI_SMMU_CR_FAULT_DIAGNOSTIC_REVISION &&
              mUfs.Smmu->DumpFault != NULL)
            FaultStatus = mUfs.Smmu->DumpFault (mUfs.Smmu, 0, 0xFFFF,
                                             mUfs.Target->Sids[SidIndex]);
          DEBUG ((DEBUG_ERROR, "Crane UFS: SID=0x%x fault capture: %r\n",
                  mUfs.Target->Sids[SidIndex], FaultStatus));
        }
      }
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI CrUfsRead (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width,
  IN UINT64 Offset, IN UINTN Count, IN OUT VOID *Buffer
  )
{
  return CrUfsReadWrite (This, Width, Offset, Count, Buffer, FALSE);
}

STATIC EFI_STATUS EFIAPI CrUfsWrite (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width,
  IN UINT64 Offset, IN UINTN Count, IN OUT VOID *Buffer
  )
{
  return CrUfsReadWrite (This, Width, Offset, Count, Buffer, TRUE);
}

STATIC EFI_STATUS EFIAPI CrUfsOverrideHcInfo (
  IN EFI_HANDLE ControllerHandle,
  IN OUT EDKII_UFS_HC_INFO *HcInfo
  )
{
  if ((ControllerHandle == NULL) || (HcInfo == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}


STATIC
EFI_STATUS
CrUfsSetClock (
  IN EFI_CLOCK_CR_PROTOCOL  *Clock,
  IN CONST CHAR8            *Id,
  IN UINT64                 RateHz,
  OUT BOOLEAN               *Owned
  )
{
  EFI_STATUS Status;

  Status = CrAcquireClock (Clock, CR_UFS_CONTROLLER, Id, RateHz, Owned);
  if (Status != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "Crane UFS: enable clock %a failed: %r\n", Id, Status));
  }
  return CrUfsFinalStatus (Status);
}

STATIC
EFI_STATUS
CrUfsReleaseResources (VOID)
{
  EFI_STATUS Status;
  UINTN Index;
  BOOLEAN NeedCoreReset;

  /* A failed reset deassert must be reasserted before rollback.  A normal
     teardown already quiesces the PHY below while the core is running, so it
     must not issue a BCR reset when the required core clock was never enabled. */
  NeedCoreReset = mUfs.CoreResetAsserted;
  mUfs.ResourcesReady = FALSE;
  mUfs.CoreResetCompleted = FALSE;
  mUfs.CleanupPending = TRUE;
  /* Keep the device quiescent until every resource can be released. */
  Status = mUfs.Gpio->SetIoValue (mUfs.Gpio, CR_UFS_DEVICE_RESET_GPIO, 0);
  if (Status == EFI_SUCCESS) Status = CrUfsCheckReset (0, FALSE);
  if (Status != EFI_SUCCESS) {
    return CrUfsFinalStatus (Status);
  }
  /* Linux qmp_ufs_power_off() powers down the PCS before disabling its
     clocks.  The HCI BCR reset is asserted after the PHY MMIO sequence so
     cleanup never touches HCI/PHY registers while that reset is active. */
  if (mUfs.PhyPowered) {
    MmioAnd32 (CR_UFS_HCI_BASE + CR_UFS_CFG1, ~CR_UFS_DEV_REFCLK_EN);
    (VOID)MmioRead32 (CR_UFS_HCI_BASE + CR_UFS_CFG1);
    MmioOr32 (CR_UFS_HCI_BASE + CR_UFS_CFG1, CR_UFS_PHY_SOFT_RESET);
    MmioAnd32 (CR_UFS_PHY_BASE + CR_UFS_PHY_START, ~BIT0);
    MmioAnd32 (CR_UFS_PHY_BASE + CR_UFS_PHY_PWR_CTRL, ~BIT0);
    (VOID)MmioRead32 (CR_UFS_PHY_BASE + CR_UFS_PHY_PWR_CTRL);
    for (Index = 0; Index < mUfs.Target->RxInterfaceCount; ++Index)
      MmioOr32 (CR_UFS_PHY_BASE + mUfs.Target->RxInterfaceOffsets[Index], BIT5);
    mUfs.PhyPowered = FALSE;
  }
  if (NeedCoreReset) {
    /* Keep the controller in reset while clocks and power domains are
       removed; a partially initialized HCI must not be exposed again. */
    Status = mUfs.Clock->SetReset (mUfs.Clock, CR_UFS_CONTROLLER,
                                 mUfs.Target->Reset, TRUE);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    mUfs.CoreResetAsserted = FALSE;
    MicroSecondDelay (1000);
  }
  while (mUfs.ClockCount != 0) {
    Index = mUfs.ClockCount - 1;
    Status = mUfs.Clock->SetClock (mUfs.Clock, CR_UFS_CONTROLLER,
                                 mUfs.Target->Clocks[Index].Name, 0, FALSE);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    mUfs.ClockCount--;
  }
  for (Index = mUfs.Target->RailCount; Index-- != 0;) {
    if (mUfs.RailRequested[Index]) {
      Status = mUfs.Rpmh->RpmhEnableVreg (mUfs.Rpmh, mUfs.Target->Rails[Index].Name, FALSE);
      if (Status != EFI_SUCCESS) {
        return CrUfsFinalStatus (Status);
      }
      mUfs.RailRequested[Index] = FALSE;
    }
  }
  if (mUfs.GdscEnabled) {
    Status = mUfs.Clock->SetGdsc (mUfs.Clock, CR_UFS_CONTROLLER,
                                CR_UFS_GDSC_ID, FALSE);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    mUfs.GdscEnabled = FALSE;
  }
  /* ReleasePath also drops its votes; retain each handle until successful. */
  if (mUfs.CfgPath != INTERCONNECT_INVALID_PATH) {
    Status = mUfs.Interconnect->ReleasePath (mUfs.Interconnect, mUfs.CfgPath);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    mUfs.CfgPath = INTERCONNECT_INVALID_PATH;
  }
  if (mUfs.MemPath != INTERCONNECT_INVALID_PATH) {
    Status = mUfs.Interconnect->ReleasePath (mUfs.Interconnect, mUfs.MemPath);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    mUfs.MemPath = INTERCONNECT_INVALID_PATH;
  }
  mUfs.InterconnectReady = FALSE;
  mUfs.CleanupPending = FALSE;
  /* The SMMU domain belongs to apps-smmu, survives host reset, and has no
     outstanding DMA maps here. Keep the attachment instead of allocating
     another domain after a failed initialization. */
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CrUfsInitializeResources (VOID)
{
  EFI_STATUS Status;
  EFI_STATUS CleanupStatus;
  UINTN Index;
  UINT32 HwVersion, Major;
  CONST CHAR8 *Stage = "providers";

  if (mUfs.ExitStarted) return EFI_ACCESS_DENIED;

  Status = CrUfsResolveTarget ();
  if (Status != EFI_SUCCESS) return CrUfsFinalStatus (Status);

  Status = CrUfsRetryPendingMapping ();
  if (Status != EFI_SUCCESS) {
    return CrUfsFinalStatus (Status);
  }
  if (mUfs.ResourcesReady) {
    return EFI_SUCCESS;
  }
  if (mUfs.Initializing) {
    return EFI_NOT_READY;
  }
  if (mUfs.CleanupPending) {
    Status = CrUfsReleaseResources ();
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
  }

  /* Resolve every provider before the first vote or MMIO access. APRIORI
     must not bypass this driver's resource DEPEX. */
  Status = gBS->LocateProtocol (&gEfiGpioCrProtocolGuid, NULL, (VOID **)&mUfs.Gpio);
  if ((Status != EFI_SUCCESS) || (mUfs.Gpio == NULL) ||
      (mUfs.Gpio->SetIoValue == NULL) || (mUfs.Gpio->ConfigGpio == NULL) ||
      (mUfs.Gpio->GetGpioConfig == NULL)) {
    return EFI_NOT_READY;
  }
  Status = gBS->LocateProtocol (&gEfiClockCrProtocolGuid, NULL, (VOID **)&mUfs.Clock);
  if ((Status != EFI_SUCCESS) || (mUfs.Clock == NULL) ||
      (mUfs.Clock->SetClock == NULL) || (mUfs.Clock->SetReset == NULL) ||
      (mUfs.Clock->SetGdsc == NULL)) {
    return EFI_NOT_READY;
  }
  Status = gBS->LocateProtocol (&gEfiRpmhCrProtocolGuid, NULL, (VOID **)&mUfs.Rpmh);
  if ((Status != EFI_SUCCESS) || (mUfs.Rpmh == NULL) ||
      (mUfs.Rpmh->Revision < EFI_RPMH_CR_PROTOCOL_REVISION) ||
      (mUfs.Rpmh->RpmhEnableVreg == NULL) ||
      (mUfs.Rpmh->AcquireVreg == NULL) ||
      (mUfs.Rpmh->EnsureBootFloor == NULL) ||
      (mUfs.Target->RetainedRailMask && mUfs.Rpmh->EnsureRetainedVreg == NULL) ||
      (mUfs.Target->RetainedEnableSupply &&
       (mUfs.Rpmh->Revision < 0x10005 || mUfs.Rpmh->EnsureRetainedVregEnabled == NULL))) {
    return EFI_NOT_READY;
  }
  Status = gBS->LocateProtocol (&gEfiInterconnectCrProtocolGuid, NULL,
                                (VOID **)&mUfs.Interconnect);
  if ((Status != EFI_SUCCESS) || (mUfs.Interconnect == NULL) ||
      (mUfs.Interconnect->AcquirePath == NULL) ||
      (mUfs.Interconnect->SetBandwidth == NULL) ||
      (mUfs.Interconnect->ReleasePath == NULL)) {
    return EFI_NOT_READY;
  }
  Status = gBS->LocateProtocol (&gEfiSmmuCrProtocolGuid, NULL, (VOID **)&mUfs.Smmu);
  if ((Status != EFI_SUCCESS) || (mUfs.Smmu == NULL) || (mUfs.Smmu->Attach == NULL)) {
    return EFI_NOT_READY;
  }
  Status = gBS->LocateProtocol (&gEdkiiIoMmuProtocolGuid, NULL, (VOID **)&mUfs.Iommu);
  if ((Status != EFI_SUCCESS) || (mUfs.Iommu == NULL) ||
      (mUfs.Iommu->Revision < EDKII_IOMMU_PROTOCOL_REVISION) ||
      (mUfs.Iommu->Map == NULL) || (mUfs.Iommu->Unmap == NULL) ||
      (mUfs.Iommu->AllocateBuffer == NULL) || (mUfs.Iommu->FreeBuffer == NULL) ||
      (mUfs.Iommu->SetAttributeById == NULL)) {
    mUfs.Iommu = NULL;
    return EFI_NOT_READY;
  }

  Status = CrUfsRegisterLegacyExit ();
  if (Status != EFI_SUCCESS) return Status;

  Stage = "reset-low";
  mUfs.Initializing = TRUE;
  DEBUG ((DEBUG_INIT, "Crane UFS: stage reset-low GPIO%u\n", CR_UFS_DEVICE_RESET_GPIO));
  GpioConfigParams ResetConfig = mUfs.Target->DeviceReset;
  Status = mUfs.Gpio->ConfigGpio (mUfs.Gpio, &ResetConfig);
  if (Status == EFI_SUCCESS) Status = CrUfsCheckReset (0, TRUE);
  if (Status != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "Crane UFS: stage %a failed: %r\n", Stage, Status));
    goto Done;
  }
  MicroSecondDelay (10);
  DEBUG ((DEBUG_INIT, "Crane UFS: reset-low done; stage ICC acquire\n"));
  Stage = "ICC memory acquire";
  Status = mUfs.Interconnect->AcquirePath (
             mUfs.Interconnect, mUfs.Target->Paths[0].Provider,
             mUfs.Target->Paths[0].Source, mUfs.Target->Paths[0].Destination, &mUfs.MemPath);
  if (Status != EFI_SUCCESS) {
    goto Rollback;
  }
  Stage = "ICC config acquire";
  Status = mUfs.Interconnect->AcquirePath (
             mUfs.Interconnect, mUfs.Target->Paths[1].Provider,
             mUfs.Target->Paths[1].Source, mUfs.Target->Paths[1].Destination, &mUfs.CfgPath);
  if (Status != EFI_SUCCESS) {
    goto Rollback;
  }
  DEBUG ((DEBUG_INIT, "Crane UFS: ICC paths acquired; stage bandwidth votes\n"));
  /* Linux ufs_qcom_icc_init(): MODE_MAX in kB/s. */
  Stage = "ICC memory vote";
  Status = mUfs.Interconnect->SetBandwidth (mUfs.Interconnect, mUfs.MemPath,
             mUfs.Target->Paths[0].Average, mUfs.Target->Paths[0].Peak);
  if (Status != EFI_SUCCESS) {
    goto Rollback;
  }
  Stage = "ICC config vote";
  Status = mUfs.Interconnect->SetBandwidth (mUfs.Interconnect, mUfs.CfgPath,
             mUfs.Target->Paths[1].Average, mUfs.Target->Paths[1].Peak);
  if (Status != EFI_SUCCESS) {
    goto Rollback;
  }
  mUfs.InterconnectReady = TRUE;

  /* Qualified CX boot floor must be acknowledged before GDSC/300MHz votes. */
  Stage = "RPMh boot floor";
  Status = mUfs.Rpmh->EnsureBootFloor (mUfs.Rpmh);
  if (Status != EFI_SUCCESS) goto Rollback;

  DEBUG ((DEBUG_INIT, "Crane UFS: ICC votes done; stage GDSC\n"));
  Stage = "GDSC";
  Status = CrAcquireGdsc (mUfs.Clock, CR_UFS_CONTROLLER, CR_UFS_GDSC_ID,
                           &mUfs.GdscEnabled);
  if (Status != EFI_SUCCESS) {
    goto Rollback;
  }
  DEBUG ((DEBUG_INIT, "Crane UFS: GDSC enabled; stage rails\n"));
  Stage = "rails";
  if (mUfs.Target->RetainedEnableSupply) {
    Stage = "retained supplier";
    Status = mUfs.Rpmh->EnsureRetainedVregEnabled(mUfs.Rpmh,mUfs.Target->RetainedEnableSupply);
    if (Status != EFI_SUCCESS) goto Rollback;
    Stage = "rails";
  }
  for (Index = 0; Index < mUfs.Target->RailCount; Index++) {
    if (mUfs.Target->RetainedRailMask & (1U << Index)) {
      Status = mUfs.Rpmh->EnsureRetainedVreg (mUfs.Rpmh,
          mUfs.Target->Rails[Index].Name, mUfs.Target->Rails[Index].VoltageMv,
          mUfs.Target->Rails[Index].Mode);
      if (Status != EFI_SUCCESS) goto Rollback;
    }
    Status = mUfs.Rpmh->AcquireVreg (mUfs.Rpmh, mUfs.Target->Rails[Index].Name,
                                    mUfs.Target->Rails[Index].VoltageMv, mUfs.Target->Rails[Index].Mode,
                                    &mUfs.RailRequested[Index]);
    if (Status != EFI_SUCCESS) {
      DEBUG ((DEBUG_ERROR, "Crane UFS: rail %a failed: %r\n", mUfs.Target->Rails[Index].Name, Status));
      goto Rollback;
    }
  }
  DEBUG ((DEBUG_INIT, "Crane UFS: rails ready; stage non-lane clocks\n"));
  Stage = "non-lane clocks";
  while (mUfs.ClockCount < mUfs.Target->PrePhyClockCount) {
    /* Count a retained failed acquisition, but never withdraw a foreign
       vote when validation failed before this caller acquired a reference. */
    BOOLEAN Owned;
    Index = mUfs.ClockCount;
    Status = CrUfsSetClock (mUfs.Clock, mUfs.Target->Clocks[Index].Name,
                            mUfs.Target->Clocks[Index].RateHz, &Owned);
    if (Owned) ++mUfs.ClockCount;
    if (Status != EFI_SUCCESS) {
      goto Rollback;
    }
  }

  DEBUG ((DEBUG_INIT, "Crane UFS: non-lane clocks ready; stage core reset\n"));
  /* GCC BCR resets the HCI core. The PHY reset provider in Linux is a
     DIFFERENT reset: HCI CFG1.PHY_SOFT_RESET. Never calibrate under BCR. */
  Stage = "core reset";
  mUfs.CoreResetCompleted = FALSE;
  mUfs.CoreResetAsserted = TRUE;
  Status = mUfs.Clock->SetReset (mUfs.Clock, CR_UFS_CONTROLLER, mUfs.Target->Reset, TRUE);
  if (Status != EFI_SUCCESS) {
    goto Rollback;
  }
  MicroSecondDelay (200); /* >= four 32.768 kHz sleep clock cycles. */
  Status = mUfs.Clock->SetReset (mUfs.Clock, CR_UFS_CONTROLLER, mUfs.Target->Reset, FALSE);
  if (Status != EFI_SUCCESS) {
    goto Rollback;
  }
  mUfs.CoreResetAsserted = FALSE;
  MicroSecondDelay (1000);
  mUfs.CoreResetCompleted = TRUE;

  DEBUG ((DEBUG_INIT, "Crane UFS: core reset done; stage PHY calibration\n"));
  /* Read the silicon gear limit only after clocks and core reset complete.
     This SM8450 PHY table supports at most G4 even if PARAM0 reports more. */
  Stage = "HCI timer profile";
  HwVersion = MmioRead32 (CR_UFS_HCI_BASE + 0xe4);
  Major = (HwVersion >> 28) & 0xf;
  DEBUG ((DEBUG_INIT, "Crane UFS: HW_VERSION=0x%x, QMP v%u profile\n",
          HwVersion, mUfs.Target->PhyGeneration));
  if (HwVersion == 0 || HwVersion == MAX_UINT32 ||
      !(mUfs.Target->AllowedHciMajors & (1U << Major))) {
    Status = EFI_UNSUPPORTED;
    goto Rollback;
  }
  Stage = "PHY calibration";
  mUfs.MaxHsGear = (MmioRead32 (CR_UFS_HCI_BASE + CR_UFS_PARAM0) >> 4) & 7;
  if (mUfs.MaxHsGear == 0) { Status = EFI_UNSUPPORTED; goto Rollback; }
  if (mUfs.MaxHsGear > mUfs.Target->MaxHsGear) mUfs.MaxHsGear = mUfs.Target->MaxHsGear;
  for (Index = 0; Index < mUfs.Target->RxInterfaceCount; ++Index)
    MmioAnd32 (CR_UFS_PHY_BASE + mUfs.Target->RxInterfaceOffsets[Index], ~BIT5);
  MmioOr32 (CR_UFS_PHY_BASE + CR_UFS_PHY_PWR_CTRL, BIT0);
  mUfs.PhyPowered = TRUE;
  MmioOr32 (CR_UFS_HCI_BASE + CR_UFS_CFG1, CR_UFS_PHY_SOFT_RESET);
  (VOID)MmioRead32 (CR_UFS_HCI_BASE + CR_UFS_CFG1);
  MicroSecondDelay (1000);
  CrUfsProgramPhy (&mUfs.Target->Base);
  if (mUfs.MaxHsGear == 4) CrUfsProgramPhy (&mUfs.Target->Gear4);
  CrUfsProgramPhy (&mUfs.Target->RateB);
  MmioAnd32 (CR_UFS_HCI_BASE + CR_UFS_CFG1, ~CR_UFS_PHY_SOFT_RESET);
  (VOID)MmioRead32 (CR_UFS_HCI_BASE + CR_UFS_CFG1);
  MicroSecondDelay (1000);
  MmioAnd32 (CR_UFS_PHY_BASE + CR_UFS_PHY_SW_RESET, ~BIT0);
  MmioOr32 (CR_UFS_PHY_BASE + CR_UFS_PHY_START, BIT0);
  for (Index = 0; Index < 100; Index++) {
    if ((MmioRead32 (CR_UFS_PHY_BASE + CR_UFS_PHY_READY) & BIT0) != 0) {
      break;
    }
    MicroSecondDelay (100);
  }
  if (Index == 100) {
    Status = EFI_TIMEOUT;
    DEBUG ((DEBUG_ERROR, "Crane UFS: QMP PHY READY timeout\n"));
    goto Rollback;
  }
  DEBUG ((DEBUG_INIT, "Crane UFS: PHY ready; stage lane clocks\n"));
  MmioOr32 (CR_UFS_HCI_BASE + CR_UFS_CFG1, CR_UFS_QUNIPRO_SEL);
  Stage = "lane clocks";
  while (mUfs.ClockCount < mUfs.Target->ClockCount) {
    BOOLEAN Owned;
    Index = mUfs.ClockCount;
    Status = CrUfsSetClock (mUfs.Clock, mUfs.Target->Clocks[Index].Name,
                            mUfs.Target->Clocks[Index].RateHz, &Owned);
    if (Owned) ++mUfs.ClockCount;
    if (Status != EFI_SUCCESS) {
      goto Rollback;
    }
  }
  DEBUG ((DEBUG_INIT, "Crane UFS: lane clocks ready; stage SMMU attach\n"));
  Stage = "SMMU attach";
  while (mUfs.SmmuAttached < mUfs.Target->SidCount) {
    UINT16 Sid = mUfs.Target->Sids[mUfs.SmmuAttached];
    if (mUfs.Target->AcquireInheritedSmmu) {
      if (mUfs.Smmu->Revision < EFI_SMMU_CR_STOPPED_UFS_REVISION ||
          mUfs.Smmu->AcquireUfsStream == NULL) { Status = EFI_UNSUPPORTED; goto Rollback; }
      Status = mUfs.Smmu->AcquireUfsStream (mUfs.Smmu, CR_UFS_HCI_BASE, Sid,
                                          CrUfsVerifyStoppedOwner, &mUfs);
    } else {
      Status = mUfs.Smmu->Attach (mUfs.Smmu, 0, 0xFFFF, Sid);
    }
    if (Status != EFI_SUCCESS) {
      DEBUG ((DEBUG_ERROR, "Crane UFS: SID 0x%x attach failed: %r\n", Sid, Status));
      goto Rollback;
    }
    ++mUfs.SmmuAttached;
  }
  if (mUfs.Target->AcquireInheritedSmmu) {
    Status = CrUfsVerifyStoppedOwner (&mUfs);
    if (Status != EFI_SUCCESS) goto Rollback;
    /* Requalify every retained native domain, including E0 after an earlier
       4E0 failure. Idempotent acquisition reads registers and stopped state;
       it never reallocates/remaps a valid retained descriptor pool. */
    for (Index = 0; Index < mUfs.SmmuAttached; Index++) {
      Status = mUfs.Smmu->AcquireUfsStream (mUfs.Smmu, CR_UFS_HCI_BASE,
          mUfs.Target->Sids[Index], CrUfsVerifyStoppedOwner, &mUfs);
      if (Status != EFI_SUCCESS) goto Rollback;
    }
  }
  DEBUG ((DEBUG_INIT, "Crane UFS: SMMU attached; stage reset-high\n"));
  Stage = "reset-high";
  Status = mUfs.Gpio->SetIoValue (mUfs.Gpio, CR_UFS_DEVICE_RESET_GPIO, 1);
  if (Status == EFI_SUCCESS) Status = CrUfsCheckReset (1, FALSE);
  if (Status != EFI_SUCCESS) {
    goto Rollback;
  }
  MicroSecondDelay (10);
  mUfs.ResourcesReady = TRUE;
  DEBUG ((DEBUG_INIT, "Crane UFS: GDSC, rails, clocks, QMP PHY and %u SMMU streams ready\n", mUfs.SmmuAttached));
  goto Done;

Rollback:
  DEBUG ((DEBUG_ERROR, "Crane UFS: stage %a failed: %r\n", Stage, Status));
  CleanupStatus = CrUfsReleaseResources ();
  if (CleanupStatus != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "Crane UFS: initialization %r; cleanup pending %r\n", Status, CleanupStatus));
  }
Done:
  mUfs.Initializing = FALSE;
  return CrUfsFinalStatus (Status);
}

/* UIC commands normally use the MU pass-through driver's interface. A caller
   may omit CallbackData; retain the same polled UFSHCI command contract in
   that case. No UTP/SCSI command handling is duplicated here. */
STATIC EFI_STATUS CrUfsUic (
  IN EDKII_UFS_HC_DRIVER_INTERFACE *Driver OPTIONAL,
  IN UINT32 Opcode,
  IN UINT16 Attribute,
  IN OUT UINT32 *Value
  )
{
  EDKII_UIC_COMMAND Command;
  EFI_STATUS Status;
  UINTN Retry;

  Command.Opcode = Opcode;
  Command.Arg1 = (UINT32)Attribute << 16;
  Command.Arg2 = 0;
  Command.Arg3 = *Value;
  if ((Driver != NULL) && (Driver->UfsExecUicCommand != NULL)) {
    Status = Driver->UfsExecUicCommand (Driver, &Command);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
  } else {
    /* HCS.UCRDY, IS.UCCS and the UIC argument registers, UFSHCI 2.0. */
    for (Retry = 0; Retry < 100000; Retry++) {
      if ((MmioRead32 (CR_UFS_HCI_BASE + 0x30) & BIT3) != 0) {
        break;
      }
      MicroSecondDelay (10);
    }
    if (Retry == 100000) {
      return EFI_TIMEOUT;
    }
    MmioWrite32 (CR_UFS_HCI_BASE + 0x20, BIT10);
    MmioWrite32 (CR_UFS_HCI_BASE + 0x94, Command.Arg1);
    MmioWrite32 (CR_UFS_HCI_BASE + 0x98, Command.Arg2);
    MmioWrite32 (CR_UFS_HCI_BASE + 0x9c, Command.Arg3);
    MemoryFence ();
    MmioWrite32 (CR_UFS_HCI_BASE + 0x90, Command.Opcode);
    for (Retry = 0; Retry < 100000; Retry++) {
      if ((MmioRead32 (CR_UFS_HCI_BASE + 0x20) & BIT10) != 0) {
        break;
      }
      MicroSecondDelay (10);
    }
    if (Retry == 100000) {
      return EFI_TIMEOUT;
    }
    Command.Arg2 = MmioRead32 (CR_UFS_HCI_BASE + 0x98);
    Command.Arg3 = MmioRead32 (CR_UFS_HCI_BASE + 0x9c);
    MmioWrite32 (CR_UFS_HCI_BASE + 0x20, BIT10);
  }
  if ((Command.Arg2 & 0xff) != 0) {
    return EFI_DEVICE_ERROR;
  }
  *Value = Command.Arg3;
  return EFI_SUCCESS;
}

/* Linux ufshcd_get_max_pwr_mode()/ufshcd_dme_change_power_mode(), with
   Android Waipio ufs_qcom_pwr_change_notify() refclk and adaptation setup.
   Every UIC operation uses MU's owner, including asynchronous PA_PWRMODE. */
STATIC EFI_STATUS CrUfsNegotiateHs (EDKII_UFS_HC_DRIVER_INTERFACE *Driver)
{
  STATIC CONST UINT16 Caps[] = { 0x1587, 0x1587, 0x1581, 0x1561 };
  STATIC CONST struct { UINT16 Attribute; UINT32 Value; } Timeouts[] = {
    { 0x15b0, 8191 }, { 0x15b1, 65535 }, { 0x15b2, 32767 },
    { 0x15b3, 8191 }, { 0x15b4, 65535 }, { 0x15b5, 32767 },
    { 0xd041, 8191 }, { 0xd042, 65535 }, { 0xd043, 32767 },
  };
  UINT32 Cap[4], Gear, Value;
  UINTN Index;
  EFI_STATUS Status;
  struct { UINT16 Attribute; UINT32 Value; } Config[] = {
    { 0x15d4, 0 }, /* PA_TXHSADAPTTYPE */
    { 0x1583, 0 }, { 0x1580, 0 }, { 0x1584, 1 }, /* RX gear, lanes, termination */
    { 0x1568, 0 }, { 0x1560, 0 }, { 0x1569, 1 }, /* TX gear, lanes, termination */
    { 0x156a, 2 }, /* PA_HSSERIES: Rate B */
  };
  if (Driver == NULL || Driver->UfsExecUicCommand == NULL) return EFI_UNSUPPORTED;
  for (Index = 0; Index < ARRAY_SIZE (Caps); Index++) {
    Cap[Index] = 0;
    /* Peer MAXRXHSGEAR constrains host TX; local MAXRXHSGEAR constrains RX. */
    Status = CrUfsUic (Driver, Index == 1 ? 3 : 1, Caps[Index], &Cap[Index]);
    if (Status != EFI_SUCCESS) return CrUfsFinalStatus (Status);
    if (Cap[Index] == 0) return EFI_UNSUPPORTED;
  }
  /* Linux's ufshcd_validate_link_params() rejects an asymmetric or
     unsupported connected-lane report before negotiating power.  The
     SM8450 DT advertises a two-lane UFS link; do not silently clamp an
     invalid peer report to one direction or accept a third lane that the
     QMP PHY and its lane clocks do not implement. */
  if ((Cap[2] != Cap[3]) || (Cap[2] > CR_UFS_MAX_LANES) ||
      (Cap[3] > CR_UFS_MAX_LANES)) {
    DEBUG ((DEBUG_ERROR, "Crane UFS: invalid connected lanes RX=%u TX=%u\n",
            Cap[2], Cap[3]));
    return EFI_UNSUPPORTED;
  }
  Gear = MIN (mUfs.MaxHsGear, MIN (Cap[0], Cap[1]));
  if (Gear == 0) return EFI_UNSUPPORTED;
  Config[0].Value = Gear == 4 ? 1 : 3; /* INITIAL_ADAPT / NO_ADAPT */
  Config[1].Value = Config[4].Value = Gear;
  Config[2].Value = Cap[2];
  Config[5].Value = Cap[3];
  /* Device reset is released and PWM link is up; gate the physical refclk
     before HS, flush the posted write, then allow Android's 50 us settle. */
  MmioOr32 (CR_UFS_HCI_BASE + CR_UFS_CFG1, CR_UFS_DEV_REFCLK_EN);
  if (!(MmioRead32 (CR_UFS_HCI_BASE + CR_UFS_CFG1) & CR_UFS_DEV_REFCLK_EN))
    return EFI_DEVICE_ERROR;
  MicroSecondDelay (50);
  for (Index = 0; Index < ARRAY_SIZE (Config); Index++) {
    Value = Config[Index].Value;
    Status = CrUfsUic (Driver, 2, Config[Index].Attribute, &Value);
    if (Status != EFI_SUCCESS) return CrUfsFinalStatus (Status);
  }
  for (Index = 0; Index < ARRAY_SIZE (Timeouts); Index++) {
    Value = Timeouts[Index].Value;
    Status = CrUfsUic (Driver, 2, Timeouts[Index].Attribute, &Value);
    if (Status != EFI_SUCCESS) return CrUfsFinalStatus (Status);
  }
  /* FAST_MODE in both directions. MU waits UCCS + UPMS + HCS.PWR_LOCAL;
     a command acknowledgement alone must never publish an HS success. */
  Value = 0x11;
  Status = CrUfsUic (Driver, 2, 0x1571, &Value);
  if (Status == EFI_SUCCESS) {
    DEBUG ((DEBUG_INIT, "Crane UFS: HS-G%u Rate B RX lanes=%u TX lanes=%u\n",
            Gear, Config[2].Value, Config[5].Value));
  }
  return CrUfsFinalStatus (Status);
}

STATIC EFI_STATUS EFIAPI CrUfsPlatformCallback (
  IN EFI_HANDLE ControllerHandle,
  IN EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase,
  IN OUT VOID *CallbackData
  )
{
  EFI_STATUS Status;
  UINT32 Value;
  UINTN Retry;
  EDKII_UFS_HC_DRIVER_INTERFACE *Driver = CallbackData;

  if (mUfs.ExitStarted) return EFI_ACCESS_DENIED;

  if ((ControllerHandle == NULL) || ((UINT32)Phase > EdkiiUfsHcPostLinkStartup)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mUfs.FatalError != 0) return EFI_DEVICE_ERROR;
  if ((Phase == EdkiiUfsHcPreHce) && mUfs.HceAttempted) {
    /* Linux's ufs_qcom_power_up_sequence recalibrates the PHY after a
       failed DME_LINKSTARTUP; MU calls PreHce again for each retry. */
    Status = CrUfsReleaseResources ();
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
  }
  Status = CrUfsInitializeResources ();
  if (Status != EFI_SUCCESS) {
    return CrUfsFinalStatus (Status);
  }
  if (Phase == EdkiiUfsHcPreHce) {
    mUfs.HceAttempted = TRUE;
  }
  DEBUG ((DEBUG_INFO, "Crane UFS platform phase %u\n", Phase));
  if (Phase == EdkiiUfsHcPostHce) {
    /* ufs_qcom_check_hibern8(): M-PHY TX_FSM_STATE, lane 0. */
    for (Retry = 0; Retry < 1000; Retry++) {
      Value = 0;
      Status = CrUfsUic (Driver, 1, 0x41, &Value); /* DME_GET */
      if (Status != EFI_SUCCESS) {
        return CrUfsFinalStatus (Status);
      }
      if (Value == 1) {
        return EFI_SUCCESS;
      }
      MicroSecondDelay (100);
    }
    DEBUG ((DEBUG_ERROR, "Crane UFS: TX lane did not enter HIBERN8 (0x%x)\n", Value));
    return EFI_TIMEOUT;
  }
  if (Phase == EdkiiUfsHcPreLinkStartup) {
    /* Linux ufs-qcom: UTP/QUniPro timers match the DT maximum boot rate,
       retained through HS-G4. SM8450 is HCI v4; QUniPro does not use the
       legacy gear-specific symbol timers. */
    DEBUG ((DEBUG_INIT, "Crane UFS: AXI/ICE/UniPro %u Hz, timers %u/us %u/40ns\n",
            CR_UFS_CORE_RATE_HZ, CR_UFS_CYCLES_US, CR_UFS_CYCLES_40NS));
    MmioWrite32 (CR_UFS_HCI_BASE + 0xc0, CR_UFS_CYCLES_US);
    (VOID)MmioRead32 (CR_UFS_HCI_BASE + 0xc0);
    Value = 0;
    Status = CrUfsUic (Driver, 1, 0xd002, &Value);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    Value = (Value & ~(0x0fff0000U | BIT8)) | (CR_UFS_CYCLES_US << 16);
    Status = CrUfsUic (Driver, 2, 0xd002, &Value); /* DME_SET */
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    Value = 0;
    Status = CrUfsUic (Driver, 1, 0x9007, &Value);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    /* PA_VS_CORE_CLK_40NS_CYCLES spans bits [6:0], including bit 6. */
    Value = (Value & ~0x7fU) | CR_UFS_CYCLES_40NS;
    Status = CrUfsUic (Driver, 2, 0x9007, &Value);
    if (Status != EFI_SUCCESS) {
      return CrUfsFinalStatus (Status);
    }
    /* Android ufs-qcom: skip ICE checks for non-SCSI UPIUs and disable
       PA_LOCAL_TX_LCC_ENABLE before link startup. */
    if (mUfs.Target->SkipIceForNonScsi) {
      MmioOr32 (CR_UFS_HCI_BASE + CR_UFS_CFG1, BIT31);
      (VOID)MmioRead32 (CR_UFS_HCI_BASE + CR_UFS_CFG1);
    }
    if (mUfs.Target->LineCfgPolicy == CrUfsLineCfgAndroidConditional) {
      MmioAnd32 (CR_UFS_PHY_BASE + CR_UFS_PHY_LINECFG_DISABLE,
                 ~CR_UFS_PHY_RX_LINECFG_DISABLE);
      if (MmioRead32 (CR_UFS_PHY_BASE + CR_UFS_PHY_LINECFG_DISABLE) &
          CR_UFS_PHY_RX_LINECFG_DISABLE) return EFI_DEVICE_ERROR;
    }
    if (mUfs.Target->LineCfgPolicy == CrUfsLineCfgAndroidConditional) {
      Value = MmioRead32 (CR_UFS_HCI_BASE + 0x08);
      if (Value == 0 || Value == MAX_UINT32) return EFI_UNSUPPORTED;
      /* Android skips PA_LOCAL_TX_LCC_ENABLE for local UniPro1.41. */
      if (Value == 0x10000 || Value == 0x10100) return EFI_SUCCESS;
    }
    Value = 0;
    return CrUfsUic (Driver, 2, 0x155e, &Value);
  }
  if (Phase == EdkiiUfsHcPostLinkStartup) {
    if (mUfs.Target->LineCfgPolicy == CrUfsLineCfgWaipioFullWrite) {
      /* Waipio BSP requires the whole register, not a read/modify/write. */
      MmioWrite32 (CR_UFS_PHY_BASE + CR_UFS_PHY_LINECFG_DISABLE,
                   CR_UFS_PHY_RX_LINECFG_DISABLE);
      if (MmioRead32 (CR_UFS_PHY_BASE + CR_UFS_PHY_LINECFG_DISABLE) !=
          CR_UFS_PHY_RX_LINECFG_DISABLE) return EFI_DEVICE_ERROR;
    } else {
      /* Android ufshcd_get_local_unipro_ver(): HCI VERSION1.0/1.1 maps to
         UniPro1.41; other HCI versions map to1.6. This is the standard HCI
         VERSION register, independent of Qualcomm HW_VERSION above. */
      Value = MmioRead32 (CR_UFS_HCI_BASE + 0x08);
      if (Value == 0 || Value == MAX_UINT32) return EFI_UNSUPPORTED;
      if (Value == 0x10000 || Value == 0x10100) {
        MmioOr32 (CR_UFS_PHY_BASE + CR_UFS_PHY_LINECFG_DISABLE,
                  CR_UFS_PHY_RX_LINECFG_DISABLE);
        if (!(MmioRead32 (CR_UFS_PHY_BASE + CR_UFS_PHY_LINECFG_DISABLE) &
              CR_UFS_PHY_RX_LINECFG_DISABLE)) return EFI_DEVICE_ERROR;
      }
    }
    return CrUfsNegotiateHs (Driver);
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UfsHcCrEntryPoint (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  ZeroMem (&mUfs, sizeof (mUfs));
  Status = CrUfsResolveTarget ();
  if (Status != EFI_SUCCESS) return CrUfsFinalStatus (Status);
  DEBUG ((DEBUG_WARN, "Crane UFS: target-qualified coherent DMA\n"));
  mUfs.MemPath = INTERCONNECT_INVALID_PATH;
  mUfs.CfgPath = INTERCONNECT_INVALID_PATH;
  mUfs.Host.GetUfsHcMmioBar = CrUfsGetMmioBar;
  mUfs.Host.AllocateBuffer  = CrUfsAllocateBuffer;
  mUfs.Host.FreeBuffer      = CrUfsFreeBuffer;
  mUfs.Host.Map             = CrUfsMap;
  mUfs.Host.Unmap           = CrUfsUnmap;
  mUfs.Host.Flush           = CrUfsFlush;
  mUfs.Host.Read            = CrUfsRead;
  mUfs.Host.Write           = CrUfsWrite;

  mUfs.Platform.Version           = EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION;
  mUfs.Platform.OverrideHcInfo   = CrUfsOverrideHcInfo;
  mUfs.Platform.Callback          = CrUfsPlatformCallback;
  mUfs.Platform.RefClkFreq        = CR_UFS_REFCLK_19P2;
  mUfs.Platform.SkipHceReenable   = FALSE;
  mUfs.Platform.SkipLinkStartup   = FALSE;

  mUfs.DevicePath.Vendor.Header.Type    = HARDWARE_DEVICE_PATH;
  mUfs.DevicePath.Vendor.Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength (&mUfs.DevicePath.Vendor.Header, sizeof (VENDOR_DEVICE_PATH));
  CopyGuid (&mUfs.DevicePath.Vendor.Guid, &gEdkiiUfsHostControllerProtocolGuid);
  mUfs.DevicePath.End.Type    = END_DEVICE_PATH_TYPE;
  mUfs.DevicePath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength (&mUfs.DevicePath.End, sizeof (EFI_DEVICE_PATH_PROTOCOL));

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid, &mUfs.DevicePath,
                  &gEdkiiUfsHostControllerProtocolGuid, &mUfs.Host,
                  &gEdkiiUfsHcPlatformProtocolGuid, &mUfs.Platform,
                  NULL
                  );
  if (Status == EFI_SUCCESS) {
    mUfs.Handle = Handle;
    DEBUG ((DEBUG_INFO, "Crane UFS host protocol installed at HCI 0x%lx\n", CR_UFS_HCI_BASE));
  }
  return CrUfsFinalStatus (Status);
}
