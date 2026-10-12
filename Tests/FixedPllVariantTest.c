/* LUCID MODE versus LUCID EVO USER FSM voting. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Library/FixedPllVoteLib.h>

#define BASE 0x100000U
static UINT32 Registers[0x53000 / 4];
static UINTN Writes, Delays;
static BOOLEAN DropVote;
static CR_FIXED_PLL_DESCRIPTION D;
UINT32 EFIAPI MmioRead32(UINTN Address) {
  assert(Address >= BASE && Address - BASE < sizeof Registers && !(Address & 3));
  return Registers[(Address - BASE) / 4];
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value) {
  /* No test case authorizes analogue or output configuration writes. */
  assert(Address == BASE + D.VoteRegister);
  ++Writes;
  Registers[(Address - BASE) / 4] = DropVote ? Value & ~D.VoteMask : Value;
  return Value;
}
UINTN EFIAPI MicroSecondDelay(UINTN Us) { Delays += Us; return Us; }
VOID EFIAPI MemoryFence(VOID) {}

static CR_FIXED_PLL_VOTE Setup(BOOLEAN Lucid) {
  memset(Registers, 0, sizeof Registers);
  Writes = Delays = 0; DropVote = FALSE;
  D = (CR_FIXED_PLL_DESCRIPTION){
    .Base = BASE, .Size = sizeof Registers,
    .VoteRegister = Lucid ? 0x52018 : 0x5201c, .VoteMask = BIT(0),
    .ModeRegister = 0, .UserRegister = Lucid ? 0xc : 0x18,
    .LRegister = Lucid ? 4 : 0x10, .AlphaRegister = Lucid ? 0x40 : 0x14,
    .FsmMask = Lucid ? BIT(20) : BIT(25),
    .LockMask = Lucid ? BIT(30) : BIT(31),
    .CalibrationPendingMask = Lucid ? 0 : BIT(8),
    .PostDividerMask = 15U << (Lucid ? 8 : 10),
    .PostDividerValue = 1U << (Lucid ? 8 : 10),
    .ExpectedL = 31, .ExpectedAlpha = 0x4000,
    .ParentHz = 19200000, .FrequencyHz = 600000000,
  };
  /* EVO deliberately omits any opt-in: all existing descriptions do so. */
  if (Lucid) D.FsmInModeRegister = TRUE;
  Registers[D.ModeRegister / 4] = D.LockMask | (Lucid ? D.FsmMask : 0);
  Registers[D.UserRegister / 4] = D.PostDividerValue | (Lucid ? 0 : D.FsmMask);
  Registers[D.LRegister / 4] = D.ExpectedL;
  Registers[D.AlphaRegister / 4] = D.ExpectedAlpha;
  CR_FIXED_PLL_VOTE P = {0};
  assert(CrFixedPllInit(&P, &D) == CR_SUCCESS);
  return P;
}
static void Variant(BOOLEAN Lucid) {
  CR_FIXED_PLL_VOTE P = Setup(Lucid);
  Registers[D.VoteRegister / 4] = BIT(9);
  assert(CrFixedPllAcquire(&P) == CR_SUCCESS && P.Owned && P.Ready);
  assert(Registers[D.VoteRegister / 4] == (BIT(9) | BIT(0)));
  assert(CrFixedPllCheck(&P) == CR_SUCCESS);
  assert(CrFixedPllRelease(&P) == CR_SUCCESS && !P.Owned);
  assert(Registers[D.VoteRegister / 4] == BIT(9) && Writes == 2);

  P = Setup(Lucid); Registers[D.VoteRegister / 4] = BIT(9) | BIT(0);
  assert(CrFixedPllAcquire(&P) == CR_SUCCESS && P.RestoreVote);
  assert(CrFixedPllRelease(&P) == CR_SUCCESS);
  assert(Registers[D.VoteRegister / 4] == (BIT(9) | BIT(0)));

  /* The expected bit in the wrong register cannot authorize a vote. */
  P = Setup(Lucid);
  UINT32 Right = Lucid ? D.ModeRegister : D.UserRegister;
  UINT32 Wrong = Lucid ? D.UserRegister : D.ModeRegister;
  Registers[Right / 4] &= ~D.FsmMask; Registers[Wrong / 4] |= D.FsmMask;
  assert(CrFixedPllAcquire(&P) == CR_UNSUPPORTED && Writes == 0 && !P.Owned);

  P = Setup(Lucid); Registers[D.LRegister / 4]++;
  assert(CrFixedPllAcquire(&P) == CR_NOT_READY && Writes == 0 && !P.Owned);
  P = Setup(Lucid); Registers[D.UserRegister / 4] ^= D.PostDividerValue;
  assert(CrFixedPllAcquire(&P) == CR_NOT_READY && Writes == 0 && !P.Owned);

  /* LUCID LOCK alone is insufficient; EVO ACTIVE alone is insufficient. */
  P = Setup(Lucid);
  Registers[D.ModeRegister / 4] &= ~D.LockMask;
  Registers[D.ModeRegister / 4] |= Lucid ? BIT(31) : BIT(30);
  assert(CrFixedPllAcquire(&P) == CR_TIMEOUT && P.Owned && !P.Ready && Delays == 1500);
  assert(CrFixedPllRelease(&P) == CR_SUCCESS && !P.Owned);
  assert(!(Registers[D.VoteRegister / 4] & D.VoteMask));

  P = Setup(Lucid); DropVote = TRUE;
  assert(CrFixedPllAcquire(&P) == CR_DEVICE_ERROR && P.Owned && !P.Ready);
  DropVote = FALSE;
  assert(CrFixedPllRelease(&P) == CR_SUCCESS && !P.Owned);
  P = Setup(Lucid);
  assert(CrFixedPllAcquire(&P) == CR_SUCCESS);
  Registers[Right / 4] &= ~D.FsmMask;
  assert(CrFixedPllCheck(&P) == CR_UNSUPPORTED);
  assert(CrFixedPllRelease(&P) == CR_SUCCESS);
}
static void MainOutput(void) {
  (void)Setup(TRUE);
  /* SM8150's SDCC2 uses GPLL0 MAIN, never the EVEN postdivider. */
  D.VoteRegister = 0x52000;
  D.PostDividerMask = D.PostDividerValue = 0;
  CR_FIXED_PLL_VOTE P = {0};
  assert(CrFixedPllInit(&P, &D) == CR_SUCCESS);
  Registers[D.VoteRegister / 4] = BIT(9);
  Registers[D.UserRegister / 4] = 0xffff;
  assert(CrFixedPllAcquire(&P) == CR_SUCCESS);
  Registers[D.UserRegister / 4] = 0;
  assert(CrFixedPllCheck(&P) == CR_SUCCESS);
  assert(CrFixedPllRelease(&P) == CR_SUCCESS);
  assert(Registers[D.VoteRegister / 4] == BIT(9));
  assert(Writes == 2 && Registers[D.UserRegister / 4] == 0);
  /* A value without a mask still describes an invalid dependency. */
  P = (CR_FIXED_PLL_VOTE){0}; D.PostDividerValue = 1;
  assert(CrFixedPllInit(&P, &D) == CR_INVALID_PARAMETER);
}
int main(void) {
  Variant(FALSE); Variant(TRUE);
  MainOutput();
  CR_FIXED_PLL_VOTE P = Setup(FALSE);
  Registers[D.ModeRegister / 4] |= BIT(8);
  assert(CrFixedPllAcquire(&P) == CR_NOT_READY && Writes == 0);
  P = (CR_FIXED_PLL_VOTE){0}; D.FsmInModeRegister = 2;
  assert(CrFixedPllInit(&P, &D) == CR_INVALID_PARAMETER);
  puts("Fixed PLL LUCID/EVO/TRION MAIN register, acknowledgement, vote and cleanup tests passed");
  return 0;
}
