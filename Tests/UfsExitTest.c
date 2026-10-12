/* SPDX-License-Identifier: MIT
 * Actual UFS adapter admission/stop/exit, modeled SMMU protocol and HCI.
 */
static unsigned char LegacyPolicy;
#define _PCD_VALUE_PcdUfsLegacyCoherentDmaHandoff LegacyPolicy
#define main OriginalUfsResourceTestMain
#include "UfsHcCrTest.c"
#undef main

STATIC EFI_SMMU_CR_EXIT_PREPARE Prepare;
STATIC EFI_SMMU_CR_EXIT_OBSERVER Complete;
STATIC UINTN PrepareCalls, CompleteCalls, ReleaseCalls;
STATIC EFI_STATUS PrepareStatus, CompleteStatus, ReleaseStatus;

STATIC EFI_STATUS EFIAPI SetPrepare (EFI_SMMU_CR_PROTOCOL *This, VOID *Owner,
    EFI_SMMU_CR_EXIT_PREPARE Callback, VOID *Context)
{
  assert (This == &mSmmu && Owner == &mUfs);
  assert (Context == (Callback ? &mUfs : NULL));
  ++PrepareCalls;
  if (PrepareStatus != EFI_SUCCESS) return PrepareStatus;
  Prepare = Callback;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetComplete (EFI_SMMU_CR_PROTOCOL *This, VOID *Owner,
    EFI_SMMU_CR_EXIT_OBSERVER Callback, VOID *Context)
{
  assert (This == &mSmmu && Owner == &mUfs && Context == &mUfs && Callback);
  ++CompleteCalls;
  if (CompleteStatus != EFI_SUCCESS) return CompleteStatus;
  Complete = Callback;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ReleaseCoherent (EFI_SMMU_CR_PROTOCOL *This)
{
  assert (This == &mSmmu && mUfs.ExitStarted && mUfs.ExitComplete);
  assert (mUfs.ExitStopStatus == EFI_SUCCESS && CrUfsExitStopped ());
  ++ReleaseCalls;
  return ReleaseStatus;
}
STATIC VOID Start (VOID)
{
  Setup (); LegacyPolicy = TRUE;
  Prepare = NULL; Complete = NULL;
  PrepareCalls = CompleteCalls = ReleaseCalls = 0;
  PrepareStatus = CompleteStatus = ReleaseStatus = EFI_SUCCESS;
  mSmmu.Revision = EFI_SMMU_CR_PROTOCOL_REVISION;
  mSmmu.SetExitPrepareObserver = SetPrepare;
  mSmmu.SetExitObserver = SetComplete;
  mSmmu.ReleaseUfsToCoherentPhysicalAtExit = ReleaseCoherent;
}
STATIC VOID Boot (VOID)
{
  assert (CrUfsInitializeResources () == EFI_SUCCESS);
  assert (Prepare == CrUfsPrepareExit && Complete == CrUfsCompleteExit);
  assert (PrepareCalls == 1 && CompleteCalls == 1);
  mMmio[0x24 / 4] = MAX_UINT32;
  mMmio[0x34 / 4] = mMmio[0x60 / 4] = mMmio[0x80 / 4] = 1;
}
int main (void)
{
  UINTN Count = 4096, Base;
  UINT32 Value = 1;
  VOID *Map = (VOID *)1, *Buffer = (VOID *)1;
  EFI_PHYSICAL_ADDRESS Address = 1;
  Start (); Boot ();
  assert (Prepare (&mUfs) == EFI_SUCCESS && CrUfsExitStopped ());
  UINTN Accesses = mAccessCount;
  assert (CrUfsGetMmioBar (&mUfs.Host, &Base) == EFI_ACCESS_DENIED && Base == 0);
  assert (CrUfsWrite (&mUfs.Host, EfiUfsHcWidthUint32, 0x58, 1, &Value) == EFI_ACCESS_DENIED);
  assert (CrUfsRead (&mUfs.Host, EfiUfsHcWidthUint32, 0x58, 1, &Value) == EFI_ACCESS_DENIED);
  assert (CrUfsAllocateBuffer (&mUfs.Host, AllocateAnyPages, EfiBootServicesData, 1, &Buffer, 0) == EFI_ACCESS_DENIED && Buffer == NULL);
  assert (CrUfsMap (&mUfs.Host, EdkiiUfsHcOperationBusMasterRead, (VOID *)1, &Count, &Address, &Map) == EFI_ACCESS_DENIED && Map == NULL && Address == 0);
  assert (CrUfsUnmap (&mUfs.Host, (VOID *)1) == EFI_ACCESS_DENIED);
  assert (CrUfsFreeBuffer (&mUfs.Host, 1, (VOID *)1) == EFI_ACCESS_DENIED);
  assert (CrUfsFlush (&mUfs.Host) == EFI_ACCESS_DENIED);
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPreHce, NULL) == EFI_ACCESS_DENIED);
  assert (mAccessCount == Accesses);
  Complete (&mUfs, EFI_SUCCESS); assert (ReleaseCalls == 1);
  Complete (&mUfs, EFI_SUCCESS); assert (ReleaseCalls == 1);
  assert (Prepare (&mUfs) == EFI_SUCCESS);

  for (UINTN Offset = 0x58; Offset <= 0x78; Offset += 0x20) {
    Start (); Boot (); mMmio[Offset / 4] = 1;
    assert (Prepare (&mUfs) == EFI_TIMEOUT);
    Complete (&mUfs, EFI_SUCCESS); assert (ReleaseCalls == 0);
  }
  CONST UINTN Stopped[] = {0x24, 0x34, 0x58, 0x60, 0x78, 0x80};
  for (UINTN I = 0; I < ARRAY_SIZE (Stopped); ++I) {
    Start (); Boot (); assert (Prepare (&mUfs) == EFI_SUCCESS);
    mMmio[Stopped[I] / 4] = 1;
    Complete (&mUfs, EFI_SUCCESS); assert (ReleaseCalls == 0);
  }
  Start (); Boot (); Complete (&mUfs, EFI_SUCCESS); assert (ReleaseCalls == 0);
  Start (); Boot (); mUfs.Initializing = TRUE;
  Accesses = mAccessCount;
  assert (Prepare (&mUfs) == EFI_NOT_READY && mAccessCount == Accesses);
  Complete (&mUfs, EFI_SUCCESS); assert (ReleaseCalls == 0);

  Start (); LegacyPolicy = FALSE;
  assert (CrUfsInitializeResources () == EFI_SUCCESS && !PrepareCalls && !CompleteCalls);
  Start (); mSmmu.Revision = EFI_SMMU_CR_EXIT_USB_PHYSICAL_REVISION;
  assert (CrUfsInitializeResources () == EFI_UNSUPPORTED && !mAccessCount);
  Start (); PrepareStatus = EFI_WARN_UNKNOWN_GLYPH;
  assert (CrUfsInitializeResources () == EFI_DEVICE_ERROR && !mAccessCount && !CompleteCalls);
  Start (); CompleteStatus = EFI_OUT_OF_RESOURCES;
  assert (CrUfsInitializeResources () == EFI_OUT_OF_RESOURCES && !mAccessCount && !Prepare && PrepareCalls == 2);
  Start (); Boot (); ReleaseStatus = EFI_DEVICE_ERROR;
  assert (Prepare (&mUfs) == EFI_SUCCESS);
  Complete (&mUfs, EFI_SUCCESS); Complete (&mUfs, EFI_SUCCESS); assert (ReleaseCalls == 1);
  puts ("UFS exit: admission, stop, stale HCI, registration and terminal failure checks passed");
  return 0;
}
