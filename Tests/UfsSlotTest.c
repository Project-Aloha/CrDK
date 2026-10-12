/** Exercise real UFS software ownership across submission/completion gaps. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef NULL
#include <Uefi.h>
#include "UfsPassThruHci.c"

EFI_BOOT_SERVICES *gBS;
EDKII_UFS_HC_PLATFORM_PROTOCOL *mUfsHcPlatform;
static EFI_TPL CurrentTpl = TPL_APPLICATION;
static UINT32 Doorbell;
static BOOLEAN ReadFails, Reenter;
static UFS_PASS_THRU_PRIVATE_DATA *ReenterPrivate;
static UINT8 ReenterSlot;

VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) {}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return FALSE; }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID EFIAPI DebugAssert (CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description) { abort (); }

static EFI_TPL EFIAPI Raise (EFI_TPL Tpl)
{
  EFI_TPL Previous = CurrentTpl;
  assert (Tpl >= Previous);
  CurrentTpl = Tpl;
  return Previous;
}

static VOID EFIAPI Restore (EFI_TPL Tpl)
{
  assert (Tpl <= CurrentTpl);
  CurrentTpl = Tpl;
  // Simulate a callback running at the first opportunity after reservation.
  if (Reenter) {
    Reenter = FALSE;
    assert (UfsFindAvailableSlotInTrl (ReenterPrivate, &ReenterSlot) == EFI_SUCCESS);
  }
}

static EFI_STATUS EFIAPI Read (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset, UINTN Count, VOID *Value)
{
  assert (CurrentTpl == TPL_NOTIFY && Offset == UFS_HC_UTRLDBR_OFFSET);
  if (ReadFails) return EFI_DEVICE_ERROR;
  *(UINT32 *)Value = Doorbell;
  return EFI_SUCCESS;
}

int main (void)
{
  EFI_BOOT_SERVICES Boot = { .RaiseTPL = Raise, .RestoreTPL = Restore };
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL Host = { .Read = Read };
  UFS_PASS_THRU_PRIVATE_DATA Owner = { .UfsHostController = &Host };
  UINT8 First = 0xff, Next = 0xff;
  gBS = &Boot;
  Owner.UfsHcInfo.Capabilities = 1; // Two hardware transfer slots.
  ReenterPrivate = &Owner;
  Reenter = TRUE;
  assert (UfsFindAvailableSlotInTrl (&Owner, &First) == EFI_SUCCESS);
  assert (First == 0 && ReenterSlot == 1);
  assert (UfsFindAvailableSlotInTrl (&Owner, &Next) == EFI_NOT_READY);
  // Completion clears hardware ownership but software still owns response/DMA.
  Doorbell = BIT0;
  Doorbell = 0;
  assert (UfsFindAvailableSlotInTrl (&Owner, &Next) == EFI_NOT_READY);
  UfsReleaseSlot (&Owner, First);
  assert (UfsFindAvailableSlotInTrl (&Owner, &Next) == EFI_SUCCESS && Next == First);
  UfsReleaseSlot (&Owner, First);
  UfsReleaseSlot (&Owner, ReenterSlot);
  // A live hardware bit remains unavailable even without a software owner.
  Doorbell = BIT0;
  assert (UfsFindAvailableSlotInTrl (&Owner, &Next) == EFI_SUCCESS && Next == 1);
  UfsReleaseSlot (&Owner, Next);
  Doorbell = 0;
  ReadFails = TRUE;
  assert (UfsFindAvailableSlotInTrl (&Owner, &Next) == EFI_DEVICE_ERROR);
  assert (Owner.ReservedSlots == 0 && CurrentTpl == TPL_APPLICATION);
  ReadFails = FALSE;
  Owner.UfsHcInfo.Capabilities = 31;
  Doorbell = 0x7fffffff;
  assert (UfsFindAvailableSlotInTrl (&Owner, &Next) == EFI_SUCCESS && Next == 31);
  assert (UfsFindAvailableSlotInTrl (&Owner, &Next) == EFI_NOT_READY);
  UfsReleaseSlot (&Owner, 31);
  puts ("UFS slots: callback reservation, delayed response ownership, hardware busy, read errors and slot31 PASS");
  return 0;
}
