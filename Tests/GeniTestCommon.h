/** @file Linux-derived GENI transport behavior test support. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/CrDalLib.h>
#include <Library/DxeServicesTableLib.h>
#include "../Target/Sm8450/CrQupTarget.c"

static EFI_BOOT_SERVICES BootServices;
EFI_BOOT_SERVICES *gBS = &BootServices;
EFI_GUID gEfiQupCrProtocolGuid;
EFI_GUID gEfiClockCrProtocolGuid;
EFI_GUID gEfiInterconnectCrProtocolGuid;
EFI_GUID gEfiGpioCrProtocolGuid;
static CONST CR_TARGET_QUP_ICC_PATH TestIccPaths[] = {
  {"qcom,sm8450-qup1", 0x5000, 0x5001, 1},
  {"qcom,sm8450-qup1", 0x5002, 0x5003, 1},
  {"qcom,sm8450-qup1", 0x5004, 0x5005, 1000},
};
static CONST CR_TARGET_QUP_ICC_PATH TestIccPaths0[] = {
  {"qcom,sm8450-qup0", 0x5100, 0x5101, 1},
  {"qcom,sm8450-qup0", 0x5102, 0x5103, 1},
  {"qcom,sm8450-qup0", 0x5104, 0x5105, 1000},
};
static CR_TARGET_QUP_WRAPPER TestWrappers[] = {
  { .Id = 1, .SerialEngineCount = 2,.SerialEngineSize=0x4000, .SerialEngineBase = {0, 0xa84000},
    .IommuStreamId = 0x43, .SerialEngineInterconnect = {NULL, TestIccPaths} },
  { .Id = 0, .SerialEngineCount = 6,.SerialEngineSize=0x4000, .SerialEngineBase = {0, 0, 0, 0, 0, 0x994000},
    .IommuStreamId = 0x5a3,
    .SerialEngineInterconnect = {NULL, NULL, NULL, NULL, NULL, TestIccPaths0} },
};
#define TestWrapper TestWrappers[0]
static CR_TARGET_QUP_CONTEXT TestContext = {.Qup = TestWrappers, .QupCount = 2,
 .I2c=mSm8450I2c,.I2cCount=ARRAY_SIZE(mSm8450I2c)};
static EFI_STATUS EFIAPI TestMmioDescriptor(EFI_PHYSICAL_ADDRESS At,
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) {
  for(UINTN I=0;I<ARRAY_SIZE(TestWrappers);++I)
    for(UINTN E=0;E<TestWrappers[I].SerialEngineCount;++E){
      UINT64 B=TestWrappers[I].SerialEngineBase[E],N=TestWrappers[I].SerialEngineSize;
      if(B&&At>=B&&At<B+N){memset(D,0,sizeof(*D));D->BaseAddress=B;D->Length=N;
        D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;D->Capabilities=D->Attributes=EFI_MEMORY_UC;
        return EFI_SUCCESS;}
    }
  return EFI_NOT_FOUND;
}
static EFI_DXE_SERVICES DxeServices={.GetMemorySpaceDescriptor=TestMmioDescriptor};
EFI_DXE_SERVICES *gDS=&DxeServices;
CONST CR_TARGET_QUP_CONTEXT *CrDalGetQupContext (VOID) { return &TestContext; }
VOID *EFIAPI AllocateZeroPool (UINTN Size) { return calloc (1, Size); }
VOID EFIAPI FreePool (VOID *Buffer) { free (Buffer); }
VOID *EFIAPI CopyMem (VOID *D, CONST VOID *S, UINTN N) { return memcpy (D, S, N); }
VOID *EFIAPI ZeroMem (VOID *D, UINTN N) { return memset (D, 0, N); }
UINT32 EFIAPI InterlockedCompareExchange32 (volatile UINT32 *P, UINT32 C, UINT32 E) {
  UINT32 Old = *P;
  if (Old == C) *P = E;
  return Old;
}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
static void (*TestWriteBackHook)(VOID *Start, UINTN Length);
static void (*TestInvalidateHook)(VOID *Start, UINTN Length);
VOID *EFIAPI WriteBackDataCacheRange (VOID *Start, UINTN Length) {
  if (TestWriteBackHook != NULL) TestWriteBackHook (Start, Length);
  return Start;
}
VOID *EFIAPI InvalidateDataCacheRange (VOID *Start, UINTN Length) {
  if (TestInvalidateHook != NULL) TestInvalidateHook (Start, Length);
  return Start;
}

#include "../Driver/QUPCrDxe/QUPCrDxe.c"

static UINT32 IccAcquiredMask, IccVotedMask, IccAcquireCalls, IccVoteCalls, IccReleaseCalls;
static UINT32 IccFailAcquire, IccFailVote, IccFailRelease;
static EFI_STATUS EFIAPI TestIccAcquire (EFI_INTERCONNECT_CR_PROTOCOL *This,
    CONST CHAR8 *Provider, UINT32 Source, UINT32 Destination, INTERCONNECT_PATH_HANDLE *Path) {
  (VOID)This;
  BOOLEAN Qup0 = strcmp (Provider, "qcom,sm8450-qup0") == 0;
  UINT32 Base = Qup0 ? 0x5100 : 0x5000;
  assert (mSeOwners[Qup0 ? 0 : 1][Qup0 ? 5 : 1].Busy);
  assert (Qup0 || strcmp (Provider, "qcom,sm8450-qup1") == 0);
  assert (Source >= Base && Source <= Base + 4 && (Source & 1) == 0);
  assert (Destination == Source + 1);
  if (++IccAcquireCalls == IccFailAcquire) return EFI_OUT_OF_RESOURCES;
  *Path = (Source - Base) / 2 + 1 + (Qup0 ? 3 : 0);
  assert ((IccAcquiredMask & (1U << (*Path - 1))) == 0);
  IccAcquiredMask |= 1U << (*Path - 1);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestIccVote (EFI_INTERCONNECT_CR_PROTOCOL *This,
    INTERCONNECT_PATH_HANDLE Path, UINT64 Average, UINT64 Peak) {
  (VOID)This;
  assert (Path >= 1 && Path <= 6 && (IccAcquiredMask & (1U << (Path - 1))));
  assert (Average == Peak && Average == TestIccPaths[(Path - 1) % 3].Bandwidth);
  if (++IccVoteCalls == IccFailVote) return EFI_DEVICE_ERROR;
  IccVotedMask |= 1U << (Path - 1);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestIccRelease (EFI_INTERCONNECT_CR_PROTOCOL *This,
    INTERCONNECT_PATH_HANDLE Path) {
  (VOID)This;
  assert (Path >= 1 && Path <= 6 && (IccAcquiredMask & (1U << (Path - 1))));
  assert (mSeOwners[Path > 3 ? 0 : 1][Path > 3 ? 5 : 1].Busy);
  if (++IccReleaseCalls == IccFailRelease) return EFI_DEVICE_ERROR;
  IccAcquiredMask &= ~(1U << (Path - 1));
  IccVotedMask &= ~(1U << (Path - 1));
  return EFI_SUCCESS;
}
static EFI_INTERCONNECT_CR_PROTOCOL TestIcb = {
  .Revision = EFI_INTERCONNECT_CR_PROTOCOL_REVISION,
  .AcquirePath = TestIccAcquire, .SetBandwidth = TestIccVote, .ReleasePath = TestIccRelease,
};
static void TestIccReset (void) {
  IccAcquiredMask = IccVotedMask = IccAcquireCalls = IccVoteCalls = IccReleaseCalls = 0;
  IccFailAcquire = IccFailVote = IccFailRelease = 0;
  mQupIcb = &TestIcb;
  ZeroMem (mSeOwners, sizeof (mSeOwners));
}

static void TestOwnership (void) {
  UINT8 FifoOwner, GpiOwner;
  mQupContext = &TestContext;
  TestIccReset ();
  /* Either acquire or vote can fail on any of the three paths; no SE lease
     or path may leak when rollback succeeds. */
  for (UINT32 Fault = 1; Fault <= 3; ++Fault) {
    IccFailAcquire = Fault;
    assert (QupAcquireSe (&mQupProtocol, 1, 1, &FifoOwner, QupCrSeFifo) == EFI_OUT_OF_RESOURCES);
    assert (!IccAcquiredMask && !IccVotedMask && !mSeOwners[1][1].Busy);
    TestIccReset ();
    IccFailVote = Fault;
    assert (QupAcquireSe (&mQupProtocol, 1, 1, &GpiOwner, QupCrSeGpi) == EFI_DEVICE_ERROR);
    assert (!IccAcquiredMask && !IccVotedMask && !mSeOwners[1][1].Busy);
    TestIccReset ();
  }
  /* A rollback failure must retain the unreleased path and fence this SE. */
  IccFailVote = 2;
  IccFailRelease = 1;
  assert (QupAcquireSe (&mQupProtocol, 1, 1, &FifoOwner, QupCrSeFifo) == EFI_DEVICE_ERROR);
  assert (IccAcquiredMask == 2 && mSeOwners[1][1].Paths[1] == 2);
  assert (mSeOwners[1][1].Poisoned);
  assert (QupAcquireSe (&mQupProtocol, 1, 1, &GpiOwner, QupCrSeGpi) == EFI_DEVICE_ERROR);
  TestIccReset ();
  assert (QupAcquireSe (&mQupProtocol, 1, 1, &FifoOwner, QupCrSeFifo) == EFI_SUCCESS);
  assert (IccVotedMask == 7);
  assert (QupAcquireSe (&mQupProtocol, 1, 1, &GpiOwner, QupCrSeGpi) == EFI_ALREADY_STARTED);
  assert (QupReleaseSe (&mQupProtocol, 1, 1, &GpiOwner, TRUE) == EFI_ACCESS_DENIED);
  assert (QupReleaseSe (&mQupProtocol, 1, 1, &FifoOwner, TRUE) == EFI_SUCCESS);
  assert (!IccAcquiredMask && !IccVotedMask);
  assert (QupAcquireSe (&mQupProtocol, 1, 1, &GpiOwner, QupCrSeGpi) == EFI_SUCCESS);
  assert (QupAcquireSe (&mQupProtocol, 1, 1, &FifoOwner, QupCrSeFifo) == EFI_ALREADY_STARTED);
  assert (QupReleaseSe (&mQupProtocol, 1, 1, &GpiOwner, FALSE) == EFI_DEVICE_ERROR);
  assert (IccAcquiredMask == 7 && IccVotedMask == 7);
  assert (QupReleaseSe (&mQupProtocol, 1, 1, &GpiOwner, TRUE) == EFI_DEVICE_ERROR);
  assert (QupAcquireSe (&mQupProtocol, 1, 1, &FifoOwner, QupCrSeFifo) == EFI_DEVICE_ERROR);
  TestIccReset ();
  assert (QupAcquireSe (&mQupProtocol, 1, 1, &FifoOwner, QupCrSeFifo) == EFI_SUCCESS);
  IccFailRelease = 1;
  assert (QupReleaseSe (&mQupProtocol, 1, 1, &FifoOwner, TRUE) == EFI_DEVICE_ERROR);
  assert (IccAcquiredMask == 4 && IccVotedMask == 4 && mSeOwners[1][1].Paths[2] == 3);
  assert (mSeOwners[1][1].Poisoned);
  TestIccReset ();
  puts ("QUP: native ICC votes, transfer leases, rollback and failed-quiescence retention passed");
}
