/**
  Standalone Qualcomm SMEM protocol producer for Crane.

  The wire-format reader is shared with the PEI adapter in SmemLib. This
  driver only publishes the legacy UEFI protocol shape required by existing
  platform consumers; it does not load or call the Qualcomm SmemDxe binary.

  Copyright (c) 2025-2026 Project Aloha Authors.
  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/smem.h>
#include <Protocol/EFISmem.h>
#include <Protocol/EFISmemCrProtocol.h>

STATIC SmemContext mSmem;
STATIC EFI_SMEM_PROTOCOL mSmemProtocol;
STATIC EFI_SMEM_CR_PROTOCOL mScopedProtocol;
STATIC volatile UINT32 mAllocationBusy;

/* SM8450's qcom,tcsr-mutex resource is 0x01f40000/0x40000, with one
 * 32-bit lock register every 0x1000 bytes. Linux binds SMEM to lock 3 and
 * qcom_hwspinlock uses Apps processor owner ID 1. */
#define CRANE_TCSR_MUTEX_BASE       0x01F40000U
#define CRANE_TCSR_MUTEX_STRIDE     0x00001000U
#define CRANE_SMEM_MUTEX_INDEX      3U
#define CRANE_SMEM_MUTEX_OWNER      1U
#define CRANE_SMEM_MUTEX_ADDRESS    \
  (CRANE_TCSR_MUTEX_BASE + (CRANE_SMEM_MUTEX_INDEX * CRANE_TCSR_MUTEX_STRIDE))
#define CRANE_SMEM_MUTEX_TIMEOUT_US 1000000U
#define CRANE_SMEM_MUTEX_POLL_US    1000U

STATIC SmemHardwareMutex mHardwareLock = {
  CRANE_SMEM_MUTEX_ADDRESS, sizeof (UINT32), CRANE_SMEM_MUTEX_OWNER,
  CRANE_SMEM_MUTEX_TIMEOUT_US, CRANE_SMEM_MUTEX_POLL_US, FALSE
};

STATIC
EFI_STATUS
CraneSmemAcquireLock (
  VOID
  )
{
  EFI_STATUS Status;
  if (mHardwareLock.Held) {
    /* A previous allocation may have committed before unlock failed. Retry
     * that release under the local guard before any new ownership claim. */
    Status = (EFI_STATUS)SmemHardwareMutexRelease (&mHardwareLock);
    if (EFI_ERROR (Status)) return Status;
  }
  Status = (EFI_STATUS)SmemHardwareMutexAcquire (&mHardwareLock);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
          "Crane SMEM: TCSR mutex %u acquire status=%r retained=%u\n",
          CRANE_SMEM_MUTEX_INDEX, Status, mHardwareLock.Held));
  return Status;
}

STATIC
EFI_STATUS
CraneSmemReleaseLock (
  VOID
  )
{
  EFI_STATUS Status = (EFI_STATUS)SmemHardwareMutexRelease (&mHardwareLock);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
          "Crane SMEM: TCSR mutex %u release status=%r retained=%u\n",
          CRANE_SMEM_MUTEX_INDEX, Status, mHardwareLock.Held));
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
CraneSmemAllocateForHost (
  IN UINT32 Host,
  IN int ItemIndex,
  IN UINT32 Size,
  OUT VOID **Address,
  IN BOOLEAN GetOrAllocate
  )
{
  EFI_STATUS Status;
  EFI_STATUS ReleaseStatus;
  BOOLEAN InterruptsEnabled;

  if (Address != NULL) {
    *Address = NULL;
  }
  if (Address == NULL || (Host != SMEM_HOST_ANY && Host >= SMEM_HOST_COUNT) ||
      ItemIndex < 0 || (UINT32)ItemIndex >= mSmem.ItemCount ||
      Size > MAX_UINT32 - 7U) {
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((DEBUG_ERROR,
          "Crane SMEM: allocate request item=%d size=0x%x version=%u "
          "items=%u global-off=0x%x global-size=0x%x\n",
          ItemIndex, Size, mSmem.Version, mSmem.ItemCount,
          mSmem.Global.Offset, mSmem.Global.Size));

  /* All APPS callers share owner ID 1. Serialize locally as well as against
   * remote hosts, and prevent interrupt-side reentry as Linux irqsave does.
   * Never spin on a nested local call; it cannot release its caller's lock. */
  InterruptsEnabled = SaveAndDisableInterrupts ();
  if (InterlockedCompareExchange32 (&mAllocationBusy, 0, 1) != 0) {
    SetInterruptState (InterruptsEnabled);
    return EFI_ACCESS_DENIED;
  }
  Status = CraneSmemAcquireLock ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "Crane SMEM: allocate item=%d lock status=%r\n",
            ItemIndex, Status));
    goto Done;
  }
  /* Preserve Linux qcom_smem_alloc() semantics: an existing item is an
   * already-started allocation, even when the requested size differs.  The
   * legacy UEFI protocol has a subtly different contract, however: its
   * callers use AllocFunc as an idempotent "get or allocate" operation and
   * immediately consume the returned pointer.  DALSys does this for items
   * that boot firmware may have published before DXE (SM8450 item 404 is one
   * such item).  Preserve the Linux distinction in SmemLib, then translate a
   * duplicate into success only when the published item is large enough for
   * this UEFI request. */
  Status = (EFI_STATUS)SmemAllocateItem (&mSmem, Host,
                                        (UINT32)ItemIndex, Size, Address);
  if (Status == EFI_ACCESS_DENIED && GetOrAllocate) {
    UINT32 ExistingSize;
    VOID   *ExistingAddress;
    EFI_STATUS ExistingStatus;

    ExistingSize    = 0;
    ExistingAddress = NULL;
    ExistingStatus  = (EFI_STATUS)SmemGetItem (&mSmem, Host,
                                               (UINT32)ItemIndex,
                                               &ExistingSize,
                                               &ExistingAddress);
    DEBUG ((DEBUG_ERROR,
            "Crane SMEM: duplicate item=%d existing-status=%r "
            "existing-size=0x%x requested-size=0x%x address=%p\n",
            ItemIndex, ExistingStatus, ExistingSize, Size, ExistingAddress));
    if (!EFI_ERROR (ExistingStatus) && ExistingAddress != NULL &&
        ExistingSize >= Size) {
      *Address = ExistingAddress;
      Status   = EFI_SUCCESS;
    } else if (!EFI_ERROR (ExistingStatus) && ExistingSize < Size) {
      /* Never hand a caller a smaller shared object than it requested. */
      Status = EFI_BAD_BUFFER_SIZE;
    } else {
      Status = ExistingStatus;
    }
  }
  if (Status == EFI_ACCESS_DENIED && !GetOrAllocate) Status = EFI_ALREADY_STARTED;
  ReleaseStatus = CraneSmemReleaseLock ();
  if (EFI_ERROR (ReleaseStatus)) Status = ReleaseStatus;

Done:
  if (EFI_ERROR (Status)) *Address = NULL;
  InterlockedCompareExchange32 (&mAllocationBusy, 1, 0);
  SetInterruptState (InterruptsEnabled);
  DEBUG ((DEBUG_ERROR,
          "Crane SMEM: allocate item=%d size=0x%x status=%r address=%p\n",
          ItemIndex, Size, Status, *Address));
  return Status;
}

STATIC EFI_STATUS EFIAPI
CraneSmemAllocate (IN int ItemIndex, IN UINT32 Size, OUT VOID **Address)
{
  return CraneSmemAllocateForHost (SMEM_HOST_ANY, ItemIndex, Size, Address, TRUE);
}

STATIC EFI_STATUS EFIAPI
CraneSmemScopedAllocate (
  IN EFI_SMEM_CR_PROTOCOL *This, IN UINT32 Host, IN UINT32 Item,
  IN UINT32 Size, OUT VOID **Address)
{
  if (This != &mScopedProtocol || Item > MAX_INT32) {
    if (Address != NULL) *Address = NULL;
    return EFI_INVALID_PARAMETER;
  }
  return CraneSmemAllocateForHost (Host, (int)Item, Size, Address, FALSE);
}

STATIC EFI_STATUS EFIAPI
CraneSmemScopedGet (
  IN EFI_SMEM_CR_PROTOCOL *This, IN UINT32 Host, IN UINT32 Item,
  OUT UINT32 *Size, OUT VOID **Address)
{
  if (Size != NULL) *Size = 0;
  if (Address != NULL) *Address = NULL;
  if (This != &mScopedProtocol || Size == NULL || Address == NULL)
    return EFI_INVALID_PARAMETER;
  return (EFI_STATUS)SmemGetItem (&mSmem, Host, Item, Size, Address);
}

STATIC
EFI_STATUS
EFIAPI
CraneSmemGet (
  IN int ItemIndex,
  IN OUT UINT32 *Size,
  OUT VOID **Address
  )
{
  CR_STATUS Status;

  if (Address != NULL) *Address = NULL;
  if (Size != NULL) {
    *Size = 0;
  }
  if (Address == NULL || ItemIndex < 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = SmemGetItem (&mSmem, SMEM_HOST_ANY, (UINT32)ItemIndex, Size, Address);
  DEBUG ((DEBUG_ERROR,
          "Crane SMEM: get item=%d status=%r size=0x%x address=%p\n",
          ItemIndex, (EFI_STATUS)Status, Size != NULL ? *Size : 0,
          Address != NULL ? *Address : NULL));
  return (EFI_STATUS)Status;
}

STATIC
EFI_STATUS
EFIAPI
CraneSmemUnsupported (
  IN VOID *Parameters
  )
{
  DEBUG ((DEBUG_ERROR, "Crane SMEM: legacy function unsupported params=%p\n",
          Parameters));
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SmemCrDxeEntryPoint (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR_EX Region;
  EFI_HANDLE Handle;
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  if (mHardwareLock.Held || mAllocationBusy != 0) return EFI_ACCESS_DENIED;
  ZeroMem (&Region, sizeof (Region));
  ZeroMem (&mSmem, sizeof (mSmem));
  ZeroMem (&mSmemProtocol, sizeof (mSmemProtocol));
  mAllocationBusy = 0;

  Status = LocateMemoryMapAreaByName ("SMEM", &Region);
  DEBUG ((DEBUG_ERROR,
          "Crane SMEM: locate region status=%r address=0x%llx length=0x%llx\n",
          Status, (UINT64)Region.Address, (UINT64)Region.Length));
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Region.Address == 0 || Region.Length == 0 ||
      Region.Address > MAX_UINTN || Region.Length > MAX_UINTN ||
      Region.Length - 1 > MAX_UINTN - Region.Address) {
    DEBUG ((DEBUG_ERROR, "Crane SMEM: invalid region span\n"));
    return EFI_INVALID_PARAMETER;
  }

  Status = (EFI_STATUS)SmemInitialize (
              &mSmem,
              (VOID *)(UINTN)Region.Address,
              (UINTN)Region.Length,
              Region.Address);
  DEBUG ((DEBUG_ERROR,
          "Crane SMEM: parser status=%r version=%u items=%u\n",
          Status, mSmem.Version, mSmem.ItemCount));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mSmemProtocol.Revision = EFI_SMEM_PROTOCOL_REVISION;
  mSmemProtocol.AllocFunc = CraneSmemAllocate;
  mSmemProtocol.GetFunc = CraneSmemGet;
  mSmemProtocol.Func2 = CraneSmemUnsupported;
  mSmemProtocol.Func3 = CraneSmemUnsupported;
  mScopedProtocol.Revision = EFI_SMEM_CR_PROTOCOL_REVISION;
  mScopedProtocol.GetItem = CraneSmemScopedGet;
  mScopedProtocol.AllocateItem = CraneSmemScopedAllocate;
  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle, &gEfiSMEMProtocolGuid, &mSmemProtocol,
                  &gEfiSmemCrProtocolGuid, &mScopedProtocol, NULL);
  DEBUG ((DEBUG_ERROR, "Crane SMEM: install protocol status=%r handle=%p\n",
          Status, Handle));
  return Status;
}
