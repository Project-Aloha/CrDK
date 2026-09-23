/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include <Uefi.h>
#include <Library/interconnect.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* The host test links the real UEFI-facing library without pulling in the
 * firmware BaseMemory/Debug/Synchronization implementations. */
VOID *EFIAPI CopyMem(OUT VOID *Destination, IN CONST VOID *Source, IN UINTN Length) {
  return memmove(Destination, Source, Length);
}

VOID *EFIAPI SetMem(OUT VOID *Buffer, IN UINTN Length, IN UINT8 Value) {
  return memset(Buffer, Value, Length);
}

INTN EFIAPI CompareMem(IN CONST VOID *Left, IN CONST VOID *Right, IN UINTN Length) {
  return memcmp(Left, Right, Length);
}

INTN EFIAPI AsciiStrCmp(IN CONST CHAR8 *Left, IN CONST CHAR8 *Right) {
  return strcmp(Left, Right);
}

VOID EFIAPI DebugPrint(IN UINTN ErrorLevel, IN CONST CHAR8 *Format, ...) {
  (void)ErrorLevel;
  (void)Format;
}

UINT32 EFIAPI InterlockedCompareExchange32(
    IN OUT volatile UINT32 *Value, IN UINT32 Compare, IN UINT32 Exchange) {
  UINT32 Old = *Value;
  if (Old == Compare) {
    *Value = Exchange;
  }
  return Old;
}

SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *Lock) {
  *Lock = 1;
  return Lock;
}

SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *Lock) {
  assert(*Lock == 1);
  *Lock = 2;
  return Lock;
}

SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *Lock) {
  assert(*Lock == 2);
  *Lock = 1;
  return Lock;
}

typedef struct {
  UINT32 Calls;
  UINT32 Commands;
  BOOLEAN FailNext;
  UINT32 FailCall;
  BOOLEAN VcdMode;
  RpmhTcsCmd Last[INTERCONNECT_MAX_RPMH_COMMANDS];
} MOCK_RPMH;

static CR_STATUS
MockGetAddress(VOID *Context, CONST CHAR8 *Name, UINT32 *Address) {
  (void)Context;
  if (Name == NULL || Address == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (strcmp(Name, "SN7") == 0) *Address = 0x100;
  else if (strcmp(Name, "SH1") == 0) *Address = 0x104;
  else if (strcmp(Name, "SH0") == 0) *Address = 0x108;
  else if (strcmp(Name, "MC0") == 0) *Address = 0x10c;
  else if (strcmp(Name, "CN0") == 0) *Address = 0x110;
  else return CR_NOT_FOUND;
  return CR_SUCCESS;
}

static CR_STATUS
MockGetAux(VOID *Context, CONST CHAR8 *Name, UINT8 *Data, UINT32 *Length) {
  MOCK_RPMH *Mock = (MOCK_RPMH *)Context;
  if (Name == NULL || Data == NULL || Length == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (*Length < 8 || MockGetAddress(NULL, Name, (UINT32[1]){0}) != CR_SUCCESS) {
    return CR_NOT_FOUND;
  }
  memset(Data, 0, 8);
  /* Unit=1, width=1, VCD=0.  This keeps all commands in one RPMh batch and
   * leaves the vote math visible in the public runtime state. */
  Data[0] = 1;
  Data[4] = 1;
  if (Mock != NULL && Mock->VcdMode) {
    if (strcmp(Name, "SH1") == 0) Data[6] = 1;
    else if (strcmp(Name, "SH0") == 0) Data[6] = 2;
    else if (strcmp(Name, "MC0") == 0) Data[6] = 3;
    else if (strcmp(Name, "CN0") == 0) Data[6] = 4;
  }
  *Length = 8;
  return CR_SUCCESS;
}

static CR_STATUS
MockWrite(VOID *Context, RpmhTcsCmd *Commands, UINT32 Count) {
  MOCK_RPMH *Mock = (MOCK_RPMH *)Context;
  if (Mock == NULL || Commands == NULL || Count == 0 ||
      Count > INTERCONNECT_MAX_RPMH_COMMANDS) {
    return CR_INVALID_PARAMETER;
  }
  Mock->Calls++;
  if (Mock->FailNext || (Mock->FailCall != 0 && Mock->Calls == Mock->FailCall)) {
    Mock->FailNext = FALSE;
    Mock->FailCall = 0;
    return CR_DEVICE_ERROR;
  }
  memcpy(Mock->Last, Commands, Count * sizeof(*Commands));
  Mock->Commands = Count;
  return CR_SUCCESS;
}

static UINT16
NodeIndex(CONST InterconnectTargetContext *Target, CONST CHAR8 *Name) {
  UINT16 Index;
  for (Index = 0; Index < Target->NodeCount; Index++) {
    if (strcmp(Target->Nodes[Index].Name, Name) == 0) {
      return Index;
    }
  }
  assert(0 && "target node missing");
  return 0;
}

static BOOLEAN
PathHasNode(CONST InterconnectDeviceContext *Context,
            CONST InterconnectPathRuntime *Path, CONST CHAR8 *Name) {
  UINT16 Index = NodeIndex(Context->Target, Name);
  return (Path->NodeMask[Index / 64] & (1ULL << (Index % 64))) != 0;
}

int
main(void) {
  MOCK_RPMH Mock = {0};
  InterconnectDeviceContext *Context = NULL;
  InterconnectIoOps Io = {
      .GetAddress = MockGetAddress,
      .GetAuxData = MockGetAux,
      .WriteRpmh = MockWrite,
      .CmdDbContext = &Mock,
      .RpmhContext = &Mock,
      .MaxRpmhCommands = INTERCONNECT_MAX_RPMH_COMMANDS,
  };
  INTERCONNECT_PATH_HANDLE MemPath;
  INTERCONNECT_PATH_HANDLE CpuPath;
  UINT64 OldAverage;
  UINT64 OldPeak;
  CONST InterconnectTargetContext *Target;

  assert(InterconnectLibInit(&Context, CrTargetGetInterconnectContext(), &Io) ==
         CR_SUCCESS);
  assert(Context != NULL && Context->Initialized);
  Target = Context->Target;
  assert(Target->ProviderCount == 1);
  assert(strcmp(Target->Providers[0].Compatible, "qcom,sm8450-pcie") == 0);
  /* Only Linux keepalive BCMs are sent during initialization. */
  assert(Mock.Commands == 3);
  assert(Context->Bcm[2].ProgrammedX == 1 && Context->Bcm[2].ProgrammedY == 1);
  assert(Context->Bcm[3].ProgrammedX == 1 && Context->Bcm[3].ProgrammedY == 1);
  assert(Context->Bcm[4].ProgrammedX == 1 && Context->Bcm[4].ProgrammedY == 1);

  assert(InterconnectAcquirePath(Context, "qcom,sm8450-pcie", 0x1000, 0x1100,
                                 &MemPath) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "xm_pcie3_0"));
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "qns_pcie_mem_noc"));
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "qnm_pcie"));
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "qns_llcc"));
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "ebi"));
  assert(!PathHasNode(Context, &Context->Paths[MemPath - 1], "qns_pcie"));
  assert(InterconnectSetBandwidth(Context, MemPath, 100, 200) == CR_SUCCESS);
  assert(Context->Paths[MemPath - 1].AverageBandwidth == 100);
  assert(Context->Bcm[0].VoteX != 0 && Context->Bcm[0].VoteY != 0);

  assert(InterconnectAcquirePath(Context, "qcom,sm8450-pcie", 0x2000, 0x2100,
                                 &CpuPath) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[CpuPath - 1], "chm_apps"));
  assert(PathHasNode(Context, &Context->Paths[CpuPath - 1], "qns_pcie"));
  assert(PathHasNode(Context, &Context->Paths[CpuPath - 1], "qnm_gemnoc_pcie"));
  assert(PathHasNode(Context, &Context->Paths[CpuPath - 1], "xs_pcie_0"));
  assert(!PathHasNode(Context, &Context->Paths[CpuPath - 1], "qhs_pcie0_cfg"));
  assert(InterconnectSetBandwidth(Context, CpuPath, 10, 20) == CR_SUCCESS);
  OldAverage = Context->Paths[MemPath - 1].AverageBandwidth;
  OldPeak = Context->Paths[MemPath - 1].PeakBandwidth;
  Mock.FailNext = TRUE;
  assert(InterconnectSetBandwidth(Context, MemPath, 300000, 400000) != CR_SUCCESS);
  assert(Context->Paths[MemPath - 1].AverageBandwidth == OldAverage);
  assert(Context->Paths[MemPath - 1].PeakBandwidth == OldPeak);

  Mock.FailNext = TRUE;
  assert(InterconnectReleasePath(Context, MemPath) != CR_SUCCESS);
  assert(Context->Paths[MemPath - 1].InUse);
  assert(InterconnectReleasePath(Context, MemPath) == CR_SUCCESS);
  assert(InterconnectReleasePath(Context, CpuPath) == CR_SUCCESS);
  assert(Context->Bcm[0].ProgrammedX == 0 && Context->Bcm[0].ProgrammedY == 0);
  assert(Context->Bcm[1].ProgrammedX == 0 && Context->Bcm[1].ProgrammedY == 0);
  assert(Context->Bcm[2].ProgrammedX == 1 && Context->Bcm[2].ProgrammedY == 1);
  assert(Context->Bcm[3].ProgrammedX == 1 && Context->Bcm[3].ProgrammedY == 1);
  assert(Context->Bcm[4].ProgrammedX == 1 && Context->Bcm[4].ProgrammedY == 1);

  assert(InterconnectLibDeinit(Context) == CR_SUCCESS);
  assert(!Context->Initialized);

  /* Give each BCM a separate VCD and fail the second batch.  The first batch
   * is already visible to RPMh when the second one fails; the library must
   * submit the restored votes before returning the error. */
  Mock.VcdMode = TRUE;
  Mock.FailNext = FALSE;
  Mock.FailCall = 0;
  Io.MaxRpmhCommands = 1;
  assert(InterconnectLibInit(&Context, CrTargetGetInterconnectContext(), &Io) ==
         CR_SUCCESS);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-pcie", 0x1000, 0x1100,
                                 &MemPath) == CR_SUCCESS);
  {
    UINT32 StartCalls = Mock.Calls;
    Mock.FailCall = StartCalls + 2;
    assert(InterconnectSetBandwidth(Context, MemPath, 100, 200) != CR_SUCCESS);
    assert(Context->Paths[MemPath - 1].AverageBandwidth == 0);
    assert(Context->Bcm[0].ProgrammedX == 0 && Context->Bcm[0].ProgrammedY == 0);
    assert(Context->Bcm[1].ProgrammedX == 0 && Context->Bcm[1].ProgrammedY == 0);
    assert(!Context->Bcm[0].Dirty && !Context->Bcm[1].Dirty);
  }
  assert(InterconnectReleasePath(Context, MemPath) == CR_SUCCESS);
  assert(InterconnectLibDeinit(Context) == CR_SUCCESS);
  puts("SM8450 ICB: Linux paths, BCM votes and rollback passed");
  return 0;
}
