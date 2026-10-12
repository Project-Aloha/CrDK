/* Actual SM8250 command units with an independent target register model.
 * OS callbacks are modeled; no hardware/IRQ/DMA ownership is manufactured.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Library/MdssDsiCmdCrLib/Internal.h"
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P, UINTN N, UINT8 V) { return memset(P, V, N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A, B, N); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memmove(D, S, N); }
#endif
void AndroidDscTrace(const CR_MDSS_DSC_CONFIG *, UINT32 *, UINT32 *, UINT32 *);

typedef struct { UINT32 Window, Offset, Value; } WRITE;
typedef struct {
  UINT32 Dpu[0x83208 / 4], Vbif[0x2008 / 4];
  WRITE Trace[2048];
  UINT32 Calls, Fail, Writes, Dropped, DropWindow, DropOffset, DropNth, Match;
  UINT32 AckReads, AckTarget, ResetReads, PendingClear, Publishes;
  UINT32 ElapsedUs, FrameUntil, AutoRefreshReleases, SyncWrites;
  CR_STATUS Failure;
  BOOLEAN CommitError, Lost, StuckAck, UpperAck, StuckReset, StuckLine;
  BOOLEAN AutoRefreshActive, RepairExpected;
  CR_MDSS_CMD *Owner;
} MODEL;
static MODEL M;
static UINT8 FrameByte;

static CR_STATUS Step(MODEL *H)
{
  ++H->Calls;
  return H->Fail == H->Calls ? H->Failure : CR_SUCCESS;
}
static CR_STATUS Ready(VOID *C, CR_MDSS_CMD_WINDOW W)
{
  MODEL *H = C;
  assert(W < CrMdssCmdWindows);
  return H->Lost ? CR_NOT_READY : Step(H);
}
static UINT32 *Reg(MODEL *H, CR_MDSS_CMD_WINDOW W, UINT32 O)
{
  assert(!(O & 3));
  if (W == CrMdssCmdDpu) { assert(O < 0x83208); return &H->Dpu[O / 4]; }
  assert(W == CrMdssCmdVbif && O < 0x2008);
  return &H->Vbif[O / 4];
}
static CR_STATUS Read(VOID *C, CR_MDSS_CMD_WINDOW W, UINT32 O, UINT32 *V)
{
  MODEL *H = C;
  CR_STATUS S = Step(H);
  assert(!H->Lost);
  if (S != CR_SUCCESS) return S;
  /* ACK edges are independently delayed and only progress with the source
   * clock forced. High-only is deliberately NOT an SM8250 ACK. */
  if (W == CrMdssCmdVbif && O == 0x204 && H->AckReads &&
      (H->Dpu[0x2ac / 4] & 0x100) && !H->StuckAck) {
    if (!--H->AckReads) {
      H->Vbif[0x204 / 4] &= ~(2U | (2U << 16));
      H->Vbif[0x204 / 4] |= H->UpperAck ? H->AckTarget << 16 : H->AckTarget;
    }
  }
  if (W == CrMdssCmdDpu && O == 0x1030 && H->ResetReads && !H->StuckReset) {
    if (!--H->ResetReads) { H->Dpu[O / 4] = 0; H->Dpu[0x1018 / 4] = 0; }
  }
  if (W == CrMdssCmdDpu && O == 0x6aab0 && H->StuckLine) { *V = 1; return CR_SUCCESS; }
  if (W == CrMdssCmdDpu && O == 0x6aab0 && H->FrameUntil) {
    *V = H->ElapsedUs < H->FrameUntil ? 512 : 0; return CR_SUCCESS;
  }
  if (W == CrMdssCmdDpu && O == 0x34c) {
    assert(H->Dpu[0x418/4] == 0x405 && H->Dpu[0x348/4] == 0x7001);
    *V = H->AutoRefreshActive ? 0x80 : 0; return CR_SUCCESS;
  }
  *V = *Reg(H, W, O);
  return S;
}
static CR_STATUS Write(VOID *C, CR_MDSS_CMD_WINDOW W, UINT32 O, UINT32 V)
{
  MODEL *H = C;
  CR_STATUS S = Step(H);
  assert(!H->Lost);
  if (S != CR_SUCCESS && !H->CommitError) return S;
  /* Neither pinned Android nor mainline defines SSPP+0x04. Keep this
   * unqualified location outside the model's accepted write aperture. */
  assert(W != CrMdssCmdDpu || O != 0x24004);
  if (W == CrMdssCmdDpu && O == 0x24038) assert(H->Owner->IrqOwned);
  assert(H->Writes < 2048);
  H->Trace[H->Writes++] = (WRITE){W, O, V};
  if (W == H->DropWindow && O == H->DropOffset && ++H->Match == H->DropNth) {
    ++H->Dropped;
    return S;
  }
  if (W == CrMdssCmdDpu && O == 0x18) H->Dpu[0x14 / 4] &= ~V;
  else if (W == CrMdssCmdDpu && O == 0x6d808) H->Dpu[0x6d804 / 4] &= ~V;
  else {
    if (W == CrMdssCmdDpu && ((O == 0x6aa84 && !V) || (O == 0x1030 && (V & 1))))
      assert(!H->AutoRefreshActive && H->ElapsedUs >= H->FrameUntil);
    if (W == CrMdssCmdDpu && O == 0x6aa90 && H->RepairExpected) {
      assert(H->ElapsedUs >= 25000); ++H->SyncWrites;
      if (H->AutoRefreshReleases && !--H->AutoRefreshReleases) H->AutoRefreshActive = FALSE;
    }
    *Reg(H, W, O) = V;
    if (W == CrMdssCmdDpu && O == 0x1030 && (V & 1)) H->ResetReads = 3;
    if (W == CrMdssCmdVbif && O == 0x200) { H->AckReads = 3; H->AckTarget = V & 2; }
  }
  return S;
}
static CR_STATUS Fence(VOID *C) { return Step(C); }
static CR_STATUS Delay(VOID *C, UINT32 Us) {
  MODEL *H = C; assert(Us == 1 || Us == 2000 || Us == 25000);
  CR_STATUS S = Step(C); if (S == CR_SUCCESS || H->CommitError) H->ElapsedUs += Us;
  return S;
}
static CR_STATUS Publish(VOID *C, VOID *P, UINTN Bytes)
{
  MODEL *H = C;
  assert(P == &FrameByte && Bytes == 4320U * 2340U);
  ++H->Publishes;
  return Step(H);
}
static void Setup(CR_MDSS_CMD *D)
{
  memset(&M, 0, sizeof(M));
  M.Owner = D;
  M.DropOffset = 0xffffffffU;
  M.Failure = CR_DEVICE_ERROR;
  /* Nonzero foreign bits verify exact masked changes, not whole-bank clears. */
  M.Dpu[0x2ac / 4] = 0x10;
  M.Dpu[0x10 / 4] = 1U << 3;
  M.Dpu[0x14 / 4] = (1U << 3) | (1U << 8);
  M.Dpu[0x6d800 / 4] = 1;
  M.Dpu[0x6d804 / 4] = 1 | 4;
  M.Vbif[0x200 / 4] = 4;
  M.Vbif[0x204 / 4] = 4;
  memset(D, 0, sizeof(*D));
  CR_MDSS_CMD_CONFIG C;
  CrMdssCmdHdk8245Config(&C);
  CrMdssCmdR66451Profile(&D->Profile);
  D->Profile.Config = C;
  assert(CrMdssCmdBuildPlanProfile(&D->Profile, &D->Plan) == CR_SUCCESS);
  D->Initialized = TRUE;
  D->Io.Context = &M;
  D->Io.Ready = Ready; D->Io.Read32 = Read; D->Io.Write32 = Write;
  D->Io.Fence = Fence; D->Io.DelayUs = Delay;
  D->Io.Bytes[CrMdssCmdDpu] = 0x83208;
  D->Io.Bytes[CrMdssCmdVbif] = 0x2008;
  D->Frame = (CR_MDSS_CMD_MEMORY){&M, &FrameByte, 4320U * 2340U, 0x13000000, Ready, Publish};
  D->HostTouched = D->PhyTouched = D->PanelOwned = D->ExternalOwned = TRUE;
}
static void Prepare(CR_MDSS_CMD *D)
{
  Setup(D);
  assert(CmdDscStart(D) == CR_SUCCESS);
  D->IrqOwned = TRUE;
  M.Calls = M.Writes = 0;
}
static void Started(CR_MDSS_CMD *D)
{
  Prepare(D);
  assert(CmdDpuStart(D) == CR_NOT_READY && D->Starting && D->FrameInFlight);
  M.Dpu[0x1018 / 4] = 0;
  M.Dpu[0x14 / 4] |= 1U << 8;
  UINT32 E = 0xff;
  assert(CmdDpuIrq(D, &E) == CR_SUCCESS && E == CR_MDSS_CMD_FRAME_DONE);
  assert(D->FirstFrameDone && CmdDpuStart(D) == CR_SUCCESS);
}
static void Off(CR_MDSS_CMD *D)
{
  BOOLEAN Touched = D->DpuTouched || D->DpuForceTouched || D->DpuHaltTouched;
  M.Fail = 0; M.DropOffset = 0xffffffffU; M.Lost = FALSE;
  M.StuckAck = M.UpperAck = M.StuckReset = M.StuckLine = FALSE;
  M.AutoRefreshActive = FALSE; M.FrameUntil = 0;
  D->IrqOwned = FALSE;
  assert(CmdDpuStop(D) == CR_SUCCESS);
  assert(!D->DpuTouched && !D->DpuForceTouched && !D->DpuHaltTouched);
  assert(!D->DpuDebugTouched);
  assert(!D->FrameInFlight && !D->AutoRefresh);
  if (Touched) assert((M.Vbif[0x204 / 4] & 2) && M.Vbif[0x200 / 4] == 6);
  assert(M.Dpu[0x2ac / 4] == 0x10);
  assert(CmdDscStop(D) == CR_SUCCESS && !D->DscTouched);
}

static void QueueRefresh(void)
{
  M.FrameUntil = M.ElapsedUs + 6000; M.AutoRefreshActive = M.RepairExpected = TRUE;
  M.AutoRefreshReleases = 3;
  M.Dpu[0x418/4] = 0x12; M.Dpu[0x348/4] = 0x23;
  M.Dpu[0x6aa90/4] = 0xaabb0001;
}

static void QueuedFailures(void)
{
  CR_MDSS_CMD D; Started(&D); QueueRefresh(); D.IrqOwned = FALSE; M.Calls = 0;
  assert(CmdDpuStop(&D) == CR_SUCCESS && M.SyncWrites == 4);
  UINT32 Calls = M.Calls;
  assert(M.Dpu[0x418/4] == 0x12 && M.Dpu[0x348/4] == 0x23 && !D.DpuDebugTouched);
  for (UINT32 Positive = 0; Positive < 2; ++Positive)
    for (UINT32 Posted = 0; Posted < 2; ++Posted)
      for (UINT32 F = 1; F <= Calls; ++F) {
        Started(&D); QueueRefresh(); D.IrqOwned = FALSE; M.Calls = 0; M.Fail = F;
        M.Failure = Positive ? 1 : CR_DEVICE_ERROR; M.CommitError = Posted;
        assert(CmdDpuStop(&D) != CR_SUCCESS && D.DpuTouched && D.CleanupPending);
        Off(&D);
        assert(M.Dpu[0x418/4] == 0x12 && M.Dpu[0x348/4] == 0x23);
      }
  Started(&D); QueueRefresh(); M.AutoRefreshReleases = 0; D.IrqOwned = FALSE;
  assert(CmdDpuStop(&D) == CR_TIMEOUT && D.DpuDebugTouched && D.FrameInFlight); Off(&D);
  Started(&D); QueueRefresh(); M.Dpu[0x6aaa0/4] |= 1U << 16; D.IrqOwned = FALSE;
  assert(CmdDpuStop(&D) == CR_UNSUPPORTED && D.DpuDebugTouched); Off(&D);
  const UINT32 Drops[] = {0x418, 0x348, 0x6aa90};
  for (UINT32 I = 0; I < sizeof(Drops)/sizeof(Drops[0]); ++I) {
    Started(&D); QueueRefresh(); D.IrqOwned = FALSE;
    M.DropWindow = CrMdssCmdDpu; M.DropOffset = Drops[I]; M.DropNth = 1;
    assert(CmdDpuStop(&D) == CR_DEVICE_ERROR && D.DpuDebugTouched); Off(&D);
  }
  printf("queued autorefresh stop %u callback positions x4, selector/count drops and retained retry PASS\n", Calls);
}

static void DscOracle(void)
{
  CR_MDSS_CMD D;
  UINT32 Offsets[128], Values[128], Count = 0;
  Setup(&D);
  AndroidDscTrace(&D.Plan.Dsc, Offsets, Values, &Count);
  assert(Count == 74);
  assert(CmdDscStart(&D) == CR_SUCCESS && M.Writes == Count + 3);
  for (UINT32 I = 0; I < Count; ++I) {
    assert(M.Trace[I].Window == CrMdssCmdDpu);
    if (M.Trace[I].Offset != 0x80000 + Offsets[I] || M.Trace[I].Value != Values[I]) {
      fprintf(stderr,"DSC trace %u actual %x:%x oracle %x:%x\n", I,
          M.Trace[I].Offset, M.Trace[I].Value, Offsets[I], Values[I]);
      abort();
    }
  }
  /* Literal target layout catches accidentally selecting the DPU8 DSC map. */
  assert(M.Dpu[0x80004 / 4] == 0x3880ca);
  assert(M.Dpu[0x80008 / 4] == 0x04380924);
  assert(M.Dpu[0x8000c / 4] == 0x021c0014);
  assert(M.Dpu[0x80010 / 4] == 0x021c0000);
  assert(M.Dpu[0x81800 / 4] == 0 && M.Dpu[0x700a0 / 4] == 1);
  UINT32 Calls = M.Calls;
  assert(CmdDscStart(&D) == CR_NOT_READY);
  assert(M.Calls == Calls + 1);
  assert(CmdDscStop(&D) == CR_SUCCESS);
  assert(M.Dpu[0x81800 / 4] == 0xf && !M.Dpu[0x700a0 / 4]);
  printf("legacy DSC exact Android register trace: %u writes PASS\n", Count);
}

static void Nominal(void)
{
  CR_MDSS_CMD D;
  Prepare(&D);
  assert(CmdDpuStart(&D) == CR_NOT_READY);
  assert(!D.FirstFrameDone && D.FrameInFlight && D.AutoRefresh);
  /* Literal normalized register/route inventory independent of unit macros. */
  assert(M.Dpu[0x1014 / 4] == 0x20000);
  assert(M.Dpu[0x1000 / 4] == 0x40000);
  assert(M.Dpu[0x10e8 / 4] == 1 && M.Dpu[0x10f4 / 4] == 2);
  assert(M.Dpu[0x24030 / 4] == 0x236ff && M.Dpu[0x24034 / 4] == 0x03020001);
  assert(M.Dpu[0x24014 / 4] == 0x13000000 && M.Dpu[0x24024 / 4] == 4320);
  assert(M.Dpu[0x44004 / 4] == 0x09240438);
  assert(M.Dpu[0x6aa88 / 4] == 0x80087); /* initial internal TE */
  assert(M.Dpu[0x6aa8c / 4] == 0xfff0);
  assert(M.Dpu[0x6aa90 / 4] == 2345 && M.Dpu[0x6aa94 / 4] == 2340);
  assert(M.Dpu[0x6aa9c / 4] == 0x40004 && M.Dpu[0x6aaa4 / 4] == 2341);
  assert(M.Dpu[0x6aab4 / 4] == 0x80000001 && !M.Dpu[0x6a800 / 4]);
  assert(M.Dpu[0x10 / 4] == ((1U << 26) | (1U << 8) | 8));
  assert(M.Dpu[0x6d800 / 4] == 5);
  assert(M.Dpu[0x14 / 4] == 8 && M.Dpu[0x6d804 / 4] == 1);
  assert(M.Dpu[0x24064 / 4] == 0xfff0 && M.Dpu[0x24134 / 4] == 9);
  for (UINT32 I = 0; I < M.Writes; ++I) assert(M.Trace[I].Offset != 0x101c);
  UINT32 Writes = M.Writes, E = 0xee;
  assert(CmdDpuStart(&D) == CR_NOT_READY && M.Writes == Writes);
  assert(CmdDpuIrq(&D, &E) == CR_SUCCESS && !E && !D.FirstFrameDone);
  assert(CmdDpuIrq(&D, (UINT32 *)&D.FrameSerial) == CR_INVALID_PARAMETER);
  M.Dpu[0x1018 / 4] = 0;
  M.Dpu[0x14 / 4] |= 1U << 8;
  M.Dpu[0x6d804 / 4] |= 4;
  assert(CmdDpuIrq(&D, &E) == CR_SUCCESS && E == 3);
  assert(D.FirstFrameDone && D.FrameSerial == 1 && D.TeSerial == 1);
  assert(M.Dpu[0x6aa88 / 4] == 0x180087);
  assert(CmdDpuStart(&D) == CR_SUCCESS);
  assert(CmdDscStop(&D) == CR_NOT_READY);
  assert(CmdDpuStop(&D) == CR_NOT_READY); /* physical IRQ owner still live */
  Off(&D);
  assert(M.Dpu[0x10 / 4] == 8 && M.Dpu[0x6d800 / 4] == 1);
  UINT32 Before = M.Calls;
  assert(CmdDpuStop(&D) == CR_SUCCESS && CmdDscStop(&D) == CR_SUCCESS && M.Calls == Before);
  puts("command first-frame/TE, foreign IRQ bits and exact off fence PASS");
}

static void Failures(void)
{
  CR_MDSS_CMD D;
  Setup(&D); assert(CmdDscStart(&D) == CR_SUCCESS); UINT32 DscCalls = M.Calls;
  Prepare(&D); assert(CmdDpuStart(&D) == CR_NOT_READY); UINT32 StartCalls = M.Calls;
  Started(&D); D.IrqOwned = FALSE; M.Calls = 0;
  assert(CmdDpuStop(&D) == CR_SUCCESS); UINT32 StopCalls = M.Calls;
  for (UINT32 Positive = 0; Positive < 2; ++Positive) {
    for (UINT32 Phase = 1; Phase <= DscCalls; ++Phase) {
      Setup(&D); M.Fail = Phase; M.CommitError = TRUE;
      M.Failure = Positive ? (CR_STATUS)1 : CR_DEVICE_ERROR;
      assert(CmdDscStart(&D) != CR_SUCCESS);
      M.Fail = 0;
      assert(CmdDscStop(&D) == CR_SUCCESS && !D.DscTouched);
    }
    for (UINT32 Phase = 1; Phase <= StartCalls; ++Phase) {
      Prepare(&D); M.Fail = Phase; M.CommitError = TRUE;
      M.Failure = Positive ? (CR_STATUS)1 : CR_DEVICE_ERROR;
      CR_STATUS S = CmdDpuStart(&D);
      assert(S != CR_SUCCESS && (!D.DpuTouched || D.CleanupPending));
      Off(&D);
    }
    for (UINT32 Phase = 1; Phase <= StopCalls; ++Phase) {
      Started(&D); D.IrqOwned = FALSE; M.Calls = 0; M.Fail = Phase; M.CommitError = TRUE;
      M.Failure = Positive ? (CR_STATUS)1 : CR_DEVICE_ERROR;
      assert(CmdDpuStop(&D) != CR_SUCCESS && D.DpuTouched && D.CleanupPending);
      assert(D.FrameInFlight && D.AutoRefresh);
      Off(&D);
    }
  }
  printf("callback failures: DSC %u/start %u/stop %u x error+positive PASS\n",
      DscCalls, StartCalls, StopCalls);
}

static void IrqFailures(void)
{
  CR_MDSS_CMD D;
  Prepare(&D); assert(CmdDpuStart(&D) == CR_NOT_READY);
  M.Dpu[0x1018 / 4] = 0; M.Dpu[0x14 / 4] |= 1U << 8;
  M.Dpu[0x6d804 / 4] |= 4; M.Calls = 0;
  UINT32 E = 0xcc;
  assert(CmdDpuIrq(&D, &E) == CR_SUCCESS);
  UINT32 Calls = M.Calls;
  for (UINT32 Positive = 0; Positive < 2; ++Positive) {
    for (UINT32 Phase = 1; Phase <= Calls; ++Phase) {
      Prepare(&D); assert(CmdDpuStart(&D) == CR_NOT_READY);
      M.Dpu[0x1018 / 4] = 0; M.Dpu[0x14 / 4] |= 1U << 8;
      M.Dpu[0x6d804 / 4] |= 4; M.Calls = 0; M.Fail = Phase;
      M.Failure = Positive ? (CR_STATUS)1 : CR_DEVICE_ERROR;
      M.CommitError = TRUE; E = 0xcc;
      assert(CmdDpuIrq(&D, &E) != CR_SUCCESS && E == 0xcc);
      assert(D.Faulted && D.CleanupPending && !D.FirstFrameDone);
      assert(!D.FrameSerial && !D.TeSerial);
      Off(&D);
    }
  }
  Started(&D); M.Dpu[0x6d804 / 4] |= 4; M.DropWindow = CrMdssCmdDpu;
  M.DropOffset = 0x6d808; M.DropNth = 1; E = 0xcc;
  assert(CmdDpuIrq(&D, &E) == CR_DEVICE_ERROR && E == 0xcc); Off(&D);
  Prepare(&D); assert(CmdDpuStart(&D) == CR_NOT_READY);
  M.Dpu[0x1018 / 4] = 0; M.Dpu[0x14 / 4] |= 1U << 8;
  M.DropWindow = CrMdssCmdDpu; M.DropOffset = 0x6aa88; M.DropNth = 1; E = 0xcc;
  assert(CmdDpuIrq(&D, &E) == CR_DEVICE_ERROR && !D.FirstFrameDone && E == 0xcc); Off(&D);
  printf("first-frame IRQ %u callback phases x error+positive, TE W1C/connect failures PASS\n", Calls);
}

static void Adversarial(void)
{
  CR_MDSS_CMD D;
  UINT32 E;
  Prepare(&D); D.Frame.DeviceAddress = 0x100000000ULL;
  assert(CmdDpuStart(&D) == CR_BAD_BUFFER_SIZE && !M.Writes);
  Prepare(&D); D.Plan.Config.Width = 1081;
  assert(CmdDpuStart(&D) == CR_INVALID_PARAMETER && !M.Writes);
  Prepare(&D); M.UpperAck = TRUE;
  assert(CmdDpuStart(&D) == CR_TIMEOUT && D.DpuForceTouched && D.DpuTouched);
  Off(&D);
  Started(&D); M.StuckAck = TRUE; M.Vbif[0x204 / 4] |= 2; D.IrqOwned = FALSE;
  assert(CmdDpuStop(&D) == CR_TIMEOUT && D.FrameInFlight && D.DpuForceTouched);
  Off(&D);
  Started(&D); D.IrqOwned = FALSE; M.StuckReset = TRUE;
  assert(CmdDpuStop(&D) == CR_TIMEOUT && D.DpuTouched); Off(&D);
  Started(&D); D.IrqOwned = FALSE; M.StuckLine = TRUE;
  assert(CmdDpuStop(&D) == CR_TIMEOUT && D.DpuTouched); Off(&D);
  Started(&D); M.Lost = TRUE; D.IrqOwned = FALSE; UINT32 Writes = M.Writes;
  assert(CmdDpuStop(&D) == CR_NOT_READY && M.Writes == Writes && D.DpuTouched); Off(&D);
  Prepare(&D); assert(CmdDpuStart(&D) == CR_NOT_READY);
  E = 0xab; M.Dpu[0x14 / 4] |= 1U << 8; /* flush not yet completed */
  assert(CmdDpuIrq(&D, &E) == CR_NOT_READY && E == 0xab && !D.FirstFrameDone);
  Off(&D);
  Started(&D); M.Dpu[0x14 / 4] |= (1U << 26) | (1U << 8); E = 0xab;
  assert(CmdDpuIrq(&D, &E) == CR_DEVICE_ERROR && E == 4 && D.Faulted); Off(&D);
  Started(&D); M.Dpu[0x14 / 4] |= 1U << 8; M.DropWindow = CrMdssCmdDpu;
  M.DropOffset = 0x18; M.DropNth = 1; E = 0xab;
  assert(CmdDpuIrq(&D, &E) == CR_DEVICE_ERROR && E == 0xab && D.Faulted); Off(&D);
  Started(&D); M.Dpu[0x14 / 4] |= 1U << 8; D.FrameSerial = CR_MAX_UINT64; E = 0xab;
  assert(CmdDpuIrq(&D, &E) == CR_BAD_BUFFER_SIZE && E == 0xab); Off(&D);
  const UINT32 Drops[] = {0x2ac,0x1014,0x10f4,0x6aa88,0x6aab4,0x24030,0x24014};
  for (UINT32 I = 0; I < sizeof(Drops)/sizeof(Drops[0]); ++I) {
    Prepare(&D); M.DropWindow = CrMdssCmdDpu; M.DropOffset = Drops[I]; M.DropNth = 1;
    if (Drops[I] == 0x6aab4) M.DropNth = 2;
    /* Begin from different state so an ineffective write cannot qualify. */
    if (Drops[I] != 0x2ac && Drops[I] != 0x6aab4) M.Dpu[Drops[I]/4] = 0xa5a5a5a5;
    assert(CmdDpuStart(&D) == CR_DEVICE_ERROR && D.DpuTouched);
    Off(&D);
  }
  puts("dropped controls, delayed/low-only ACK, reset/transfer timeout, loss and IRQ faults PASS");
}
int main(void) { DscOracle(); Nominal(); Failures(); IrqFailures(); Adversarial(); QueuedFailures(); return 0; }
