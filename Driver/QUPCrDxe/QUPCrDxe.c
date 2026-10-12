/** @file
 *  QUPv3 target-resource protocol and shared serial-engine arbitration.
 *
 *  This module does not touch QUP registers. Native FIFO and GPI clients use
 *  one shared lease per serial engine before changing its operating mode.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>
#include <Pi/PiDxeCis.h>

#include <Library/BaseLib.h>
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFIQupCrProtocol.h>

STATIC CONST CR_TARGET_QUP_CONTEXT *mQupContext;
STATIC EFI_QUP_CR_PROTOCOL mQupProtocol;
STATIC EFI_INTERCONNECT_CR_PROTOCOL *mQupIcb;
/* Indexed by wrapper ID; entries are validated against target metadata. */
typedef struct {
  volatile UINT32 Busy;
  VOID *Owner;
  EFI_QUP_CR_SE_MODE Mode;
  BOOLEAN Poisoned;
  INTERCONNECT_PATH_HANDLE Paths[CR_TARGET_QUP_ICC_PATHS];
} QUP_SE_OWNER;
STATIC QUP_SE_OWNER mSeOwners[256][CR_TARGET_QUP_MAX_SERIAL_ENGINES];

/* A target address and an ICC vote do not establish a CPU mapping. Qualify
   the entire SE aperture before admitting any native FIFO or GPI client;
   platform memory maps remain responsible for creating that mapping. */
STATIC EFI_STATUS
QupCheckSeMmio (CONST CR_TARGET_QUP_WRAPPER *Wrapper, UINT8 Engine)
{
  UINT64 At = Wrapper->SerialEngineBase[Engine];
  UINT64 Bytes = Wrapper->SerialEngineSize;
  if (!At || !Bytes || (At & EFI_PAGE_MASK) || (Bytes & EFI_PAGE_MASK) ||
      At > MAX_UINTN || Bytes > MAX_UINTN - At) return EFI_COMPROMISED_DATA;
  if (gDS == NULL || gDS->GetMemorySpaceDescriptor == NULL) return EFI_NOT_READY;
  UINT64 End = At + Bytes;
  while (At < End) {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
    EFI_STATUS Status = gDS->GetMemorySpaceDescriptor (At, &D);
    if (Status != EFI_SUCCESS) return EFI_ERROR(Status) ? Status : EFI_NOT_READY;
    if (D.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo || D.BaseAddress > At ||
        !D.Length || D.BaseAddress > MAX_UINT64 - D.Length ||
        D.BaseAddress + D.Length <= At ||
        (D.Attributes & EFI_CACHE_ATTRIBUTE_MASK) != EFI_MEMORY_UC ||
        (D.Attributes & (EFI_MEMORY_RP | EFI_MEMORY_RO | EFI_MEMORY_WP)))
      return EFI_ACCESS_DENIED;
    At = MIN (End, D.BaseAddress + D.Length);
  }
  return EFI_SUCCESS;
}

/* A failed release preserves the handle and the lease. No other owner may
   start a transfer using a partially released or uncertain resource set. */
STATIC EFI_STATUS
QupReleasePaths (IN OUT QUP_SE_OWNER *Slot)
{
  UINTN Index;
  EFI_STATUS Status;
  EFI_STATUS Result = EFI_SUCCESS;

  for (Index = CR_TARGET_QUP_ICC_PATHS; Index > 0; --Index) {
    if (Slot->Paths[Index - 1] == INTERCONNECT_INVALID_PATH) continue;
    Status = mQupIcb->ReleasePath (mQupIcb, Slot->Paths[Index - 1]);
    if (EFI_ERROR (Status)) {
      Result = Status;
    } else {
      Slot->Paths[Index - 1] = INTERCONNECT_INVALID_PATH;
    }
  }
  return Result;
}

STATIC EFI_STATUS
EFIAPI
QupGetWrapper (
  IN  EFI_QUP_CR_PROTOCOL          *This,
  IN  UINT8                         WrapperId,
  OUT CONST CR_TARGET_QUP_WRAPPER **Wrapper
  )
{
  UINTN Index;

  (VOID)This;
  if ((Wrapper == NULL) || (mQupContext == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Wrapper = NULL;
  for (Index = 0; Index < mQupContext->QupCount; ++Index) {
    if (mQupContext->Qup[Index].Id == WrapperId) {
      *Wrapper = &mQupContext->Qup[Index];
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS
EFIAPI
QupGetContext (
  IN  EFI_QUP_CR_PROTOCOL       *This,
  OUT CONST CR_TARGET_QUP_CONTEXT **Context
  )
{
  (VOID)This;
  if ((Context == NULL) || (mQupContext == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *Context = mQupContext;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
QupAcquireSe (IN EFI_QUP_CR_PROTOCOL *This, IN UINT8 WrapperId,
              IN UINT8 Engine, IN VOID *Owner, IN EFI_QUP_CR_SE_MODE Mode)
{
  CONST CR_TARGET_QUP_WRAPPER *Wrapper;
  CONST CR_TARGET_QUP_ICC_PATH *Paths;
  QUP_SE_OWNER *Slot;
  EFI_STATUS Status;
  EFI_STATUS Cleanup;
  UINTN Index;
  if ((This != &mQupProtocol) || (Owner == NULL) ||
      ((Mode != QupCrSeFifo) && (Mode != QupCrSeGpi))) return EFI_INVALID_PARAMETER;
  if (EFI_ERROR (QupGetWrapper (This, WrapperId, &Wrapper)) ||
      (Engine >= CR_TARGET_QUP_MAX_SERIAL_ENGINES) ||
      (Engine >= Wrapper->SerialEngineCount) ||
      (Wrapper->SerialEngineBase[Engine] == 0)) return EFI_NOT_FOUND;
  Paths = Wrapper->SerialEngineInterconnect[Engine];
  if (Paths == NULL) return EFI_UNSUPPORTED;
  if (mQupIcb == NULL) return EFI_NOT_READY;
  Status = QupCheckSeMmio (Wrapper, Engine);
  if (Status != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "Crane QUP: wrapper=%u SE=%u CPU MMIO unavailable base=%Lx size=%x: %r\n",
            WrapperId, Engine, Wrapper->SerialEngineBase[Engine],
            Wrapper->SerialEngineSize, Status));
    return Status;
  }
  Slot = &mSeOwners[WrapperId][Engine];
  if (InterlockedCompareExchange32 (&Slot->Busy, 0, 1) != 0) {
    return Slot->Poisoned ? EFI_DEVICE_ERROR : EFI_ALREADY_STARTED;
  }
  Slot->Owner = Owner;
  Slot->Mode = Mode;
  /* Vote before the consumer enables clocks or accesses GENI/GPI MMIO.
     Both FIFO and GPI use these paths, including after a mode transition. */
  for (Index = 0; Index < CR_TARGET_QUP_ICC_PATHS; ++Index) {
    Status = mQupIcb->AcquirePath (mQupIcb, Paths[Index].Provider,
                                  Paths[Index].Source, Paths[Index].Destination,
                                  &Slot->Paths[Index]);
    if (!EFI_ERROR (Status)) {
      Status = mQupIcb->SetBandwidth (mQupIcb, Slot->Paths[Index],
                                     Paths[Index].Bandwidth, Paths[Index].Bandwidth);
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "QUP: wrapper=%u SE=%u ICC path=%u acquire/vote failed: %r\n",
              WrapperId, Engine, (UINT32)Index, Status));
      Cleanup = QupReleasePaths (Slot);
      if (EFI_ERROR (Cleanup)) {
        Slot->Poisoned = TRUE;
        DEBUG ((DEBUG_ERROR, "QUP: ICC rollback failed: %r; SE fenced\n", Cleanup));
      } else {
        Slot->Owner = NULL;
        InterlockedCompareExchange32 (&Slot->Busy, 1, 0);
      }
      return Status;
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
QupReleaseSe (IN EFI_QUP_CR_PROTOCOL *This, IN UINT8 WrapperId,
              IN UINT8 Engine, IN VOID *Owner, IN BOOLEAN Quiesced)
{
  QUP_SE_OWNER *Slot;
  EFI_STATUS Status;
  if ((This != &mQupProtocol) || (Owner == NULL) ||
      (Engine >= CR_TARGET_QUP_MAX_SERIAL_ENGINES)) return EFI_INVALID_PARAMETER;
  Slot = &mSeOwners[WrapperId][Engine];
  if ((Slot->Busy == 0) || (Slot->Owner != Owner)) return EFI_ACCESS_DENIED;
  if (Slot->Poisoned || !Quiesced) {
    Slot->Poisoned = TRUE;
    return EFI_DEVICE_ERROR;
  }
  Status = QupReleasePaths (Slot);
  if (EFI_ERROR (Status)) {
    Slot->Poisoned = TRUE;
    DEBUG ((DEBUG_ERROR, "QUP: wrapper=%u SE=%u ICC release failed: %r; SE fenced\n",
            WrapperId, Engine, Status));
    return Status;
  }
  Slot->Owner = NULL;
  InterlockedCompareExchange32 (&Slot->Busy, 1, 0);
  return EFI_SUCCESS;
}

STATIC EFI_QUP_CR_PROTOCOL mQupProtocol = {
  .Revision   = EFI_QUP_CR_PROTOCOL_REVISION,
  .GetWrapper = QupGetWrapper,
  .GetContext = QupGetContext,
  .AcquireSe = QupAcquireSe,
  .ReleaseSe = QupReleaseSe,
};

EFI_STATUS
EFIAPI
QupCrEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)SystemTable;
  mQupContext = CrDalGetQupContext ();
  if ((mQupContext == NULL) || (mQupContext->Qup == NULL) ||
      (mQupContext->QupCount == 0)) {
    return EFI_NOT_FOUND;
  }

  /* A successful lease includes native ICC votes. Consumers then configure
     clocks, pins and the SE while holding that lease. */
  {
    EFI_CLOCK_CR_PROTOCOL       *Clock;

    Status = gBS->LocateProtocol (
        &gEfiClockCrProtocolGuid, NULL, (VOID **)&Clock);
    if (EFI_ERROR (Status) || (Clock == NULL)) {
      return EFI_NOT_READY;
    }
    Status = gBS->LocateProtocol (
        &gEfiInterconnectCrProtocolGuid, NULL, (VOID **)&mQupIcb);
    if (EFI_ERROR (Status) || (mQupIcb == NULL) ||
        (mQupIcb->Revision < EFI_INTERCONNECT_CR_PROTOCOL_REVISION) ||
        (mQupIcb->AcquirePath == NULL) || (mQupIcb->SetBandwidth == NULL) ||
        (mQupIcb->ReleasePath == NULL)) {
      mQupIcb = NULL;
      return EFI_NOT_READY;
    }
  }

  return gBS->InstallMultipleProtocolInterfaces (
      &ImageHandle, &gEfiQupCrProtocolGuid, &mQupProtocol, NULL);
}
