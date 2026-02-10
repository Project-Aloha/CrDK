#include <Uefi.h>
#include <oskal/cr_memmap.h>

#include <Pi/PiDxeCis.h>

#include <Library/DxeServicesTableLib.h>

#define ALIGN_TO_4KB(Size) (((Size) + 0xFFF) & ~0xFFF)

CR_STATUS
MapMemRegion(
    IN UINTN BaseAddress, IN UINTN Size, IN UINT64 Attributes,
    IN UINT64 CacheAttr, IN UINT64 MemoryType)
{
  CR_STATUS Status      = CR_SUCCESS;
  UINTN     AlignedSize = ALIGN_TO_4KB(Size);

  if (BaseAddress == 0 || Size == 0) {
    return CR_INVALID_PARAMETER;
  }

  // Add memory space to GCD
  Status = gDS->AddMemorySpace(MemoryType, BaseAddress, AlignedSize, CacheAttr);

  if (CR_ERROR(Status)) {
    log_warn(
        "Failed to add memory space at 0x%lx, Status=0x%X", BaseAddress,
        Status);
    return Status;
  }

  // Set memory attributes to MMIO + Uncacheable
  Status = gDS->SetMemorySpaceAttributes(BaseAddress, AlignedSize, Attributes);

  if (CR_ERROR(Status)) {
    log_err(
        "Failed to set attributes for 0x%lx, Status=0x%X", BaseAddress, Status);
    return Status;
  }

  return CR_SUCCESS;
}

CR_STATUS
UnMapMemRegion(IN UINTN BaseAddress, IN UINTN Size)
{
  CR_STATUS Status      = CR_SUCCESS;
  UINTN     AlignedSize = ALIGN_TO_4KB(Size);

  if (BaseAddress == 0 || Size == 0) {
    return CR_INVALID_PARAMETER;
  }

  // clear attributes
  Status = gDS->SetMemorySpaceAttributes(BaseAddress, AlignedSize, 0);

  if (CR_ERROR(Status)) {
    log_warn(
        "Failed to clear attributes at 0x%lx, Status=0x%X", BaseAddress,
        Status);
  }

  // Remove memory space from GCD to release it for other drivers
  Status = gDS->RemoveMemorySpace(BaseAddress, AlignedSize);
  if (CR_ERROR(Status)) {
    log_warn(
        "Failed to remove memory space at 0x%lx, Status=0x%X", BaseAddress,
        Status);
    return Status;
  }

  return CR_SUCCESS;
}

CR_STATUS
MapDeviceIORegion(IN UINTN BaseAddress, IN UINTN Size)
{
  return MapMemRegion(
      BaseAddress, Size, EFI_MEMORY_UC, EFI_MEMORY_UC,
      EfiGcdMemoryTypeMemoryMappedIo);
}