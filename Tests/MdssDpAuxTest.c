/* Independent DP sink/FIFO, checked callback faults and native parity.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/MdssDpCrLib.h>
#ifdef DP_NATIVE_PARITY
#include "../Driver/MDSSDpCrDxe/DpLink.h"
#include <Library/IoLib.h>
#include <Library/BaseMemoryLib.h>
#endif

#define BIT(N) ((UINT32)1U << (N))
#define STATUS_BITS (BIT(3)|BIT(6)|BIT(9)|BIT(12)|BIT(15)|BIT(18)|BIT(21)|BIT(24)|BIT(27))
typedef struct {
  UINT32 Address, Control, Bytes;
  UINT8 Packet[128];
} TRANSACTION;
typedef struct {
  UINT32 Reg[2][128];
  UINT8 Dpcd[0x3000], Edid[512], Fifo[128];
  UINTN FifoBytes, RxIndex, Calls, FaultAt, Writes, Elapsed, PhyClears;
  UINTN Transactions, Resets;
  TRANSACTION Log[512];
  UINT32 Interrupt, FaultTransactions;
  UINT32 DropWindow, DropOffset, DropValue;
  UINT32 BadIndex;
  CR_STATUS FaultStatus;
  BOOLEAN Lost, Drop, PhyClearFailure;
  UINT8 Segment, Offset;
} MODEL;
static MODEL M;
static UINT32 Stable, Foreign;
static UINTN FailureCases;
#ifndef _KERNEL_MODE
VOID *EFIAPI CopyMem(VOID *Out, CONST VOID *In, UINTN Bytes) { return memcpy(Out, In, Bytes); }
VOID *EFIAPI SetMem(VOID *Out, UINTN Bytes, UINT8 Value) { return memset(Out, Value, Bytes); }
#endif

static CR_STATUS Boundary(VOID)
{
  ++M.Calls;
  return M.Calls == M.FaultAt ? M.FaultStatus : CR_SUCCESS;
}

static VOID Execute(UINT32 Control)
{
  UINT32 Address = ((M.Fifo[0] & 15U) << 16) | ((UINT32)M.Fifo[1] << 8) | M.Fifo[2];
  UINTN Bytes = M.Fifo[3] + 1U;
  BOOLEAN Read = (M.Fifo[0] & 16) != 0;
  assert(M.FifoBytes == 4 + (Read ? 0 : Bytes));
  assert(Bytes <= 124 && M.Transactions < 512);
  TRANSACTION *T = &M.Log[M.Transactions++];
  T->Address = Address;
  T->Control = Control;
  T->Bytes = (UINT32)Bytes;
  memcpy(T->Packet, M.Fifo, M.FifoBytes);
  assert(Control == (BIT(9) | ((Control & BIT(8)) ? BIT(8) | BIT(10) | (Read ? 0 : BIT(11)) : 0)));
  UINT32 Interrupt = M.FaultTransactions ? M.Interrupt : BIT(3);
  M.Reg[0][0x20 / 4] |= Interrupt;
  if (M.FaultTransactions) { --M.FaultTransactions; return; }
  if (Control & BIT(8)) {
    assert(Address == 0x30 || Address == 0x50);
    if (!Read) {
      if (Bytes == 1) {
        if (Address == 0x30) M.Segment = M.Fifo[4]; else M.Offset = M.Fifo[4];
      }
    }
  } else {
    assert(Address + Bytes <= sizeof(M.Dpcd));
    if (!Read) memcpy(M.Dpcd + Address, M.Fifo + 4, Bytes);
  }
}

static UINT32 Load(UINT32 Window, UINT32 Offset)
{
  assert(Window < 2 && Offset < 0x200 && !(Offset & 3));
  if (Window == 1 && Offset == 0x34 && (M.Reg[1][0x34 / 4] & 1)) {
    if (M.RxIndex++ == 0) return 0; /* header */
    UINT32 Index = (UINT32)M.RxIndex - 2;
    UINT32 Address = ((M.Fifo[0] & 15) << 16) | ((UINT32)M.Fifo[1] << 8) | M.Fifo[2];
    UINT8 Byte;
    if (M.Reg[1][0x38 / 4] & BIT(8)) {
      UINTN Position = (UINTN)M.Segment * 256 + M.Offset + Index;
      assert(Position < sizeof(M.Edid));
      Byte = M.Edid[Position];
    } else {
      assert(Address + Index < sizeof(M.Dpcd));
      Byte = M.Dpcd[Address + Index];
    }
    return ((Index == M.BadIndex ? Index + 1 : Index) << 16) | ((UINT32)Byte << 8);
  }
  return M.Reg[Window][Offset / 4];
}

static VOID Store(UINT32 Window, UINT32 Offset, UINT32 Value)
{
  assert(Window < 2 && Offset < 0x200 && !(Offset & 3));
  ++M.Writes;
  if (M.Drop && Window == M.DropWindow && Offset == M.DropOffset && Value == M.DropValue) return;
  if (Window == 0 && Offset == 0x20) {
    UINT32 Pending = M.Reg[0][0x20 / 4] & (STATUS_BITS | BIT(0));
    Pending &= ~((Value >> 1) & (STATUS_BITS | BIT(0)));
    M.Reg[0][0x20 / 4] = Pending | (Value & ((STATUS_BITS << 2) | BIT(2)));
    return;
  }
  M.Reg[Window][Offset / 4] = Value;
  if (Window != 1) return;
  if (Offset == 0x30 && (Value & BIT(1))) {
    ++M.Resets;
    /* Controller reset retires FIFO/GO, but this fixture intentionally leaves
       the packed AHB completion latch for its separate W1C operation. */
    M.Reg[1][0x38 / 4] &= ~BIT(9);
  }
  if (Offset == 0x34) {
    if (Value & BIT(31)) M.FifoBytes = M.RxIndex = 0;
    if (!(Value & BIT(0))) {
      assert(M.FifoBytes < sizeof(M.Fifo));
      M.Fifo[M.FifoBytes++] = (UINT8)(Value >> 8);
    }
  }
  if (Offset == 0x38 && (Value & BIT(9))) Execute(Value);
}

static CR_STATUS Check(VOID *Context, CONST VOID *Owner)
{
  assert(Context == &M && Owner == &Stable);
  CR_STATUS S = Boundary();
  return S != CR_SUCCESS ? S : M.Lost ? CR_NOT_READY : CR_SUCCESS;
}
static CR_STATUS Read32(VOID *Context, CR_MDSS_DP_WINDOW Window, UINT32 Offset, UINT32 *Value)
{
  assert(Context == &M);
  CR_STATUS S = Boundary();
  *Value = Load(Window, Offset); /* tests failed callbacks with output effects */
  return S;
}
static CR_STATUS Write32(VOID *Context, CR_MDSS_DP_WINDOW Window, UINT32 Offset, UINT32 Value)
{
  assert(Context == &M);
  CR_STATUS S = Boundary();
  Store(Window, Offset, Value); /* committed mutation even on nonfinal result */
  return S;
}
static CR_STATUS Fence(VOID *Context)
{ assert(Context == &M); return Boundary(); }
static CR_STATUS DelayUs(VOID *Context, UINT32 Us)
{ assert(Context == &M); M.Elapsed += Us; return Boundary(); }
static CR_STATUS ClearPhy(VOID *Context, CONST VOID *Owner)
{
  assert(Context == &M && Owner == &Stable);
  ++M.PhyClears;
  CR_STATUS S = Boundary();
  return S != CR_SUCCESS ? S : M.PhyClearFailure ? CR_NOT_READY : CR_SUCCESS;
}

static CR_MDSS_DP_AUX_IO Io(VOID)
{
  CR_MDSS_DP_AUX_IO I = {0};
  I.Context = &M; I.Soc = 0x8450;
  I.View[0] = (CR_MDSS_DP_VIEW){CR_MDSS_DP_AHB_PHYSICAL, 0x200};
  I.View[1] = (CR_MDSS_DP_VIEW){CR_MDSS_DP_AUX_PHYSICAL, 0x200};
  I.Ready = Check; I.Read32 = Read32; I.Write32 = Write32;
  I.Fence = Fence; I.DelayUs = DelayUs; I.ClearPhyInterrupts = ClearPhy;
  return I;
}

static VOID ResetModel(VOID)
{
  memset(&M, 0, sizeof(M));
  M.BadIndex = CR_MAX_UINT32;
  M.FaultStatus = CR_DEVICE_ERROR;
  M.Reg[0][0x20 / 4] = BIT(0) | BIT(2); /* preserve foreign HPD pending/mask */
  for (UINTN I = 0; I < sizeof(M.Dpcd); ++I) M.Dpcd[I] = (UINT8)(I * 7 + 3);
  for (UINTN Block = 0; Block < 4; ++Block) {
    UINT8 Sum = 0;
    for (UINTN I = 0; I < 127; ++I) {
      UINT8 V = (UINT8)(I * 3 + Block);
      M.Edid[Block * 128 + I] = V; Sum = (UINT8)(Sum + V);
    }
    M.Edid[Block * 128 + 127] = (UINT8)(0 - Sum);
  }
}

static VOID Start(CR_MDSS_DP_AUX *A)
{
  ResetModel(); memset(A, 0, sizeof(*A));
  CR_MDSS_DP_AUX_IO I = Io();
  assert(CrMdssDpAuxInit(A, &I, &Stable) == CR_SUCCESS);
  assert(CrMdssDpAuxEnable(A, &Stable) == CR_SUCCESS);
  assert(A->Enabled && A->Touched && !A->CleanupPending);
  M.Calls = 0;
}

static VOID Close(CR_MDSS_DP_AUX *A)
{
  M.FaultAt = 0; M.Drop = M.Lost = M.PhyClearFailure = FALSE;
  assert(CrMdssDpAuxStop(A, &Stable) == CR_SUCCESS);
  assert(!A->Enabled && !A->Touched && !A->CleanupPending && A->Stopped);
  assert((M.Reg[0][0x20 / 4] & ((STATUS_BITS << 2) | BIT(0) | BIT(2))) == (BIT(0) | BIT(2)));
  assert(CrMdssDpAuxUninit(A, &Stable) == CR_SUCCESS && !A->Initialized);
}

static VOID Positive(VOID)
{
  CR_MDSS_DP_AUX A; UINT8 B[128];
  Start(&A);
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0x1234, B, 16) == CR_SUCCESS);
  assert(!memcmp(B, M.Dpcd + 0x1234, 16));
  memset(B, 0x40, 124);
  assert(CrMdssDpAuxTransfer(&A, &Stable, FALSE, FALSE, 0x50, B, 124) == CR_SUCCESS);
  assert(M.FifoBytes == 128 && !memcmp(M.Log[M.Transactions-1].Packet + 4, B, 124));
  assert(CrMdssDpAuxTransfer(&A, &Stable, FALSE, TRUE, 0x50, B, 124) == CR_SUCCESS);
  assert(!memcmp(B, M.Edid, 124));
  assert(M.Log[0].Packet[0] == 0x10 && M.Log[0].Packet[1] == 0x12 && M.Log[0].Packet[2] == 0x34 && M.Log[0].Packet[3] == 15);
  memset(B, 0x51, 16);
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, FALSE, 0x1234, B, 16) == CR_SUCCESS);
  assert(!memcmp(B, M.Dpcd + 0x1234, 16));
  for (UINT8 Block = 0; Block < 4; ++Block) {
    M.Segment = 0;
    assert(CrMdssDpAuxReadEdid(&A, &Stable, Block, B, sizeof(B)) == CR_SUCCESS);
    assert(!memcmp(B, M.Edid + Block * 128, sizeof(B)));
  }
  UINTN Before = M.Transactions;
  M.Interrupt = BIT(21); M.FaultTransactions = 2;
  assert(CrMdssDpAuxTransfer(&A, &Stable, FALSE, TRUE, 0x50, B, 16) == CR_SUCCESS);
  assert(M.Transactions == Before + 3 && A.Attempts == 3);
  M.Dpcd[0x201] = 0x12;
  assert(CrMdssDpAuxServiceIrq(&A, &Stable) == CR_SUCCESS && M.Dpcd[0x260] == 2);
  assert(CrMdssDpAuxUninit(&A, &Stable) == CR_BUSY);
  Close(&A);
}

static VOID Failures(VOID)
{
  CR_MDSS_DP_AUX A; UINT8 B[128], Original[128];
  const UINT32 Errors[] = {BIT(6),BIT(9),BIT(12),BIT(15),BIT(18),BIT(24),BIT(27),BIT(3)|BIT(21),BIT(21),0};
  for (UINTN E = 0; E < sizeof(Errors) / sizeof(Errors[0]); ++E) {
    Start(&A); memset(B, 0x59, sizeof(B)); memcpy(Original, B, sizeof(B));
    M.Interrupt = Errors[E]; M.FaultTransactions = 7;
    UINTN First = M.Elapsed, Resets = M.Resets;
    CR_STATUS S = CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0x20, B, 16);
    assert(S != CR_SUCCESS && !memcmp(B, Original, sizeof(B)) && !A.CleanupPending);
    if (Errors[E] == BIT(9) || Errors[E] == 0) {
      assert(S == CR_TIMEOUT && M.Transactions == 7 && M.Resets == Resets + 7);
      assert(M.Elapsed - First <= 7 * 26000U);
    }
    Close(&A);
  }
  Start(&A); memset(B, 0xa7, sizeof(B)); memcpy(Original, B, sizeof(B));
  M.BadIndex = 7;
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0x20, B, 16) == CR_COMPROMISED_DATA);
  assert(!memcmp(B, Original, sizeof(B)) && !A.CleanupPending);
  M.BadIndex = CR_MAX_UINT32; ++M.Edid[43];
  assert(CrMdssDpAuxReadEdid(&A, &Stable, 0, B, sizeof(B)) == CR_COMPROMISED_DATA);
  assert(!memcmp(B, Original, sizeof(B))); Close(&A);
}

static VOID Preflight(VOID)
{
  CR_MDSS_DP_AUX A = {0}; CR_MDSS_DP_AUX_IO I = Io(), Bad;
  ResetModel();
  Bad = I; Bad.Soc = 0x8250; assert(CrMdssDpAuxInit(&A, &Bad, &Stable) == CR_UNSUPPORTED && !A.Initialized);
  Bad = I; Bad.View[1].Physical += 4; assert(CrMdssDpAuxInit(&A, &Bad, &Stable) == CR_UNSUPPORTED);
  Bad = I; Bad.View[0].Bytes -= 4; assert(CrMdssDpAuxInit(&A, &Bad, &Stable) == CR_BAD_BUFFER_SIZE);
  Bad = I; Bad.ClearPhyInterrupts = NULL; assert(CrMdssDpAuxInit(&A, &Bad, &Stable) == CR_INVALID_PARAMETER);
  assert(CrMdssDpAuxInit(&A, &A.Io, &Stable) == CR_INVALID_PARAMETER);
  assert(CrMdssDpAuxInit(&A, &I, &Stable) == CR_SUCCESS);
  assert(CrMdssDpAuxInit(&A, &I, &Stable) == CR_BUSY);
  assert(CrMdssDpAuxUninit(&A, &Foreign) == CR_BUSY);
  assert(CrMdssDpAuxUninit(&A, &Stable) == CR_SUCCESS);
  Start(&A); UINT8 B[128]; memset(B, 0x5a, sizeof(B));
  UINTN Calls = M.Calls;
  assert(CrMdssDpAuxTransfer(&A, &Foreign, TRUE, TRUE, 0, B, 16) == CR_BUSY);
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0, B, 17) == CR_INVALID_PARAMETER);
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0xfffff, B, 2) == CR_INVALID_PARAMETER);
  assert(CrMdssDpAuxTransfer(&A, &Stable, FALSE, FALSE, 0x80, B, 1) == CR_INVALID_PARAMETER);
  assert(CrMdssDpAuxTransfer(&A, &Stable, FALSE, FALSE, 0x50, B, 125) == CR_INVALID_PARAMETER);
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0, B, 0) == CR_INVALID_PARAMETER);
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0, (UINT8 *)&A, 1) == CR_INVALID_PARAMETER);
  assert(CrMdssDpAuxReadEdid(&A, &Stable, 0, B, 127) == CR_INVALID_PARAMETER);
  assert(M.Calls == Calls && B[0] == 0x5a); Close(&A);
}

static CR_STATUS Invoke(UINT32 Phase, CR_MDSS_DP_AUX *A, UINT8 *B)
{
  if (Phase == 0) return CrMdssDpAuxEnable(A, &Stable);
  if (Phase == 1) return CrMdssDpAuxTransfer(A, &Stable, TRUE, TRUE, 0x20, B, 16);
  if (Phase == 2) return CrMdssDpAuxReadEdid(A, &Stable, 2, B, 128);
  return CrMdssDpAuxStop(A, &Stable);
}

static VOID Matrix(VOID)
{
  for (UINT32 Phase = 0; Phase < 4; ++Phase) {
    CR_MDSS_DP_AUX A; UINT8 B[128], Original[128];
    if (Phase) Start(&A); else { ResetModel(); memset(&A, 0, sizeof(A)); CR_MDSS_DP_AUX_IO I = Io(); assert(CrMdssDpAuxInit(&A, &I, &Stable) == CR_SUCCESS); }
    assert(Invoke(Phase, &A, B) == CR_SUCCESS);
    UINTN Calls = M.Calls;
    Close(&A);
    for (UINTN N = 1; N <= Calls; ++N) for (UINT32 Kind = 0; Kind < 3; ++Kind) {
      if (Phase) Start(&A); else { ResetModel(); memset(&A, 0, sizeof(A)); CR_MDSS_DP_AUX_IO I = Io(); assert(CrMdssDpAuxInit(&A, &I, &Stable) == CR_SUCCESS); }
      memset(B, 0xd3, sizeof(B)); memcpy(Original, B, sizeof(B));
      M.FaultAt = N;
      M.FaultStatus = Kind == 0 ? CR_DEVICE_ERROR : Kind == 1 ? (CR_STATUS)1 : (CR_STATUS)0x103;
      CR_STATUS S = Invoke(Phase, &A, B);
      assert(S != CR_SUCCESS);
      if (Phase == 1 || Phase == 2) assert(!memcmp(B, Original, sizeof(B)));
      if (A.Touched || A.Enabled || A.CleanupPending) assert(CrMdssDpAuxUninit(&A, &Stable) == CR_BUSY);
      if (A.CleanupPending) {
        UINTN Writes = M.Writes;
        assert(CrMdssDpAuxEnable(&A, &Stable) == CR_NOT_READY);
        assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0x20, B, 1) == CR_NOT_READY);
        assert(M.Writes == Writes);
      }
      Close(&A); ++FailureCases;
    }
  }
}

static VOID PostedAndLoss(VOID)
{
  CR_MDSS_DP_AUX A; UINT8 B[16];
  const UINT32 Drops[][3] = {{1,0x30,1},{1,0x3c,0xffff},{1,0x40,0xffff}};
  for (UINTN K = 0; K < sizeof(Drops) / sizeof(Drops[0]); ++K) {
    ResetModel(); memset(&A, 0, sizeof(A)); CR_MDSS_DP_AUX_IO I = Io();
    assert(CrMdssDpAuxInit(&A, &I, &Stable) == CR_SUCCESS);
    M.Drop = TRUE; M.DropWindow = Drops[K][0]; M.DropOffset = Drops[K][1]; M.DropValue = Drops[K][2];
    assert(CrMdssDpAuxEnable(&A, &Stable) != CR_SUCCESS && A.CleanupPending); Close(&A);
  }
  Start(&A); M.Drop = TRUE; M.DropWindow = 1; M.DropOffset = 0x38; M.DropValue = 0;
  M.Reg[1][0x38 / 4] = BIT(9);
  assert(CrMdssDpAuxStop(&A, &Stable) == CR_NOT_READY && A.CleanupPending); Close(&A);
  const UINT32 StopDrops[] = {0, 1, 3};
  for (UINTN K = 0; K < sizeof(StopDrops) / sizeof(StopDrops[0]); ++K) {
    Start(&A); M.Drop = TRUE; M.DropWindow = 1; M.DropOffset = 0x30; M.DropValue = StopDrops[K];
    assert(CrMdssDpAuxStop(&A, &Stable) == CR_NOT_READY && A.CleanupPending);
    assert(CrMdssDpAuxUninit(&A, &Stable) == CR_BUSY); Close(&A);
  }
  Start(&A); M.Drop = TRUE; M.DropWindow = 1; M.DropOffset = 0x38; M.DropValue = BIT(9);
  memset(B, 0xab, sizeof(B));
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0x20, B, 16) == CR_TIMEOUT);
  assert(B[0] == 0xab && !M.Transactions && !A.CleanupPending); Close(&A);
  Start(&A); M.Drop = TRUE; M.DropWindow = 0; M.DropOffset = 0x20;
  M.DropValue = BIT(2) | (BIT(3) << 1) | (STATUS_BITS << 2);
  M.Reg[0][0x20 / 4] |= BIT(3);
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0x20, B, 1) == CR_NOT_READY);
  assert(A.CleanupPending && !M.Transactions); Close(&A);
  Start(&A); M.PhyClearFailure = TRUE;
  assert(CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0x20, B, 1) == CR_NOT_READY && A.CleanupPending);
  M.Lost = TRUE; UINTN Writes = M.Writes;
  assert(CrMdssDpAuxStop(&A, &Stable) == CR_NOT_READY && M.Writes == Writes && A.Initialized);
  assert(CrMdssDpAuxUninit(&A, &Stable) == CR_BUSY); Close(&A);
}

#ifdef DP_NATIVE_PARITY
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { (VOID)Level; return TRUE; }
VOID EFIAPI MemoryFence(VOID) {}
UINTN EFIAPI MicroSecondDelay(UINTN Us) { M.Elapsed += Us; return Us; }
VOID *EFIAPI ZeroMem(VOID *Buffer, UINTN Bytes) { return memset(Buffer, 0, Bytes); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN Bytes) { return memcmp(A, B, Bytes); }
UINT32 EFIAPI MmioRead32(UINTN Address)
{
  if (Address >= 0xae90000 && Address < 0xae90200) return Load(0, (UINT32)(Address - 0xae90000));
  assert(Address >= 0xae90200 && Address < 0xae90400);
  return Load(1, (UINT32)(Address - 0xae90200));
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  if (Address >= 0xae90000 && Address < 0xae90200) Store(0, (UINT32)(Address - 0xae90000), Value);
  else {
    assert(Address >= 0xae90200 && Address < 0xae90400);
    Store(1, (UINT32)(Address - 0xae90200), Value);
  }
  return Value;
}
UINT32 EFIAPI MmioOr32(UINTN Address, UINT32 Value) { return MmioWrite32(Address, MmioRead32(Address) | Value); }
UINT32 EFIAPI MmioAnd32(UINTN Address, UINT32 Value) { return MmioWrite32(Address, MmioRead32(Address) & Value); }

static VOID Parity(VOID)
{
  static TRANSACTION Expected[512];
  for (UINT32 Case = 0; Case < 8; ++Case) {
    UINT8 Old[128], New[128]; UINTN Count, Elapsed;
    ResetModel(); M.Reg[0][0x20 / 4] = 0; /* legacy does not preserve HPD mask */
    CrDpAuxEnable();
    if (Case == 2) { M.FaultTransactions = 2; M.Interrupt = BIT(21); }
    if (Case == 3) { M.FaultTransactions = 7; M.Interrupt = BIT(9); }
    if (Case == 4) { M.FaultTransactions = 1; M.Interrupt = BIT(12); }
    memset(Old, 0x99, sizeof(Old));
    EFI_STATUS S;
    if (Case == 0 || Case == 2 || Case == 3 || Case == 4) S = CrDpAuxTransfer(TRUE, TRUE, 0x1234, Old, 16);
    else if (Case == 1) S = CrDpAuxTransfer(TRUE, FALSE, 0x1234, Old, 16);
    else S = CrDpReadEdid((UINT8)(Case - 5), Old);
    Count = M.Transactions; Elapsed = M.Elapsed; memcpy(Expected, M.Log, Count * sizeof(Expected[0]));
    CR_MDSS_DP_AUX A; Start(&A);
    if (Case == 2) { M.FaultTransactions = 2; M.Interrupt = BIT(21); }
    if (Case == 3) { M.FaultTransactions = 7; M.Interrupt = BIT(9); }
    if (Case == 4) { M.FaultTransactions = 1; M.Interrupt = BIT(12); }
    memset(New, 0x99, sizeof(New));
    CR_STATUS R;
    if (Case == 0 || Case == 2 || Case == 3 || Case == 4) R = CrMdssDpAuxTransfer(&A, &Stable, TRUE, TRUE, 0x1234, New, 16);
    else if (Case == 1) R = CrMdssDpAuxTransfer(&A, &Stable, TRUE, FALSE, 0x1234, New, 16);
    else R = CrMdssDpAuxReadEdid(&A, &Stable, (UINT8)(Case - 5), New, sizeof(New));
    assert((S == EFI_SUCCESS) == (R == CR_SUCCESS));
    assert(Count == M.Transactions && !memcmp(Expected, M.Log, Count * sizeof(Expected[0])));
    assert(Elapsed == M.Elapsed && !memcmp(Old, New, sizeof(Old)));
    Close(&A);
  }
  puts("Actual native DpAux packet/EDID/retry/delay parity: eight cases PASS; PHY clear boundary delegated, HPD mask preserved");
}
#endif

int main(VOID)
{
  Positive(); Failures(); Preflight(); Matrix(); PostedAndLoss();
#ifdef DP_NATIVE_PARITY
  Parity();
#endif
  printf("DP AUX actual-source: %zu callback failure/nonfinal phases; FIFO, EDID, timeout retirement, dropped writes, output/lifetime/owner checks PASS\n", (size_t)FailureCases);
  return 0;
}
