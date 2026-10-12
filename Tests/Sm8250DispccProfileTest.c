/* Actual immutable profile dump and admission/bounds checks.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/Sm8250DispccProfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL line %u: %s\n", (unsigned)__LINE__, #x); exit(1); \
} } while (0)
#define NUMBER(p, f, v) printf("%s.%s\t%llu\n", p, f, (unsigned long long)(v))
#define STRING(p, f, v) printf("%s.%s\t%s\n", p, f, v)
#define N(p, o, f) NUMBER(p, #f, (o)->f)
#define S(p, o, f) STRING(p, #f, (o)->f)

int main(void)
{
  CONST CR_SM8250_DISPCC_PROFILE *P = NULL, *Again = NULL;
  CONST UINT32 Unsupported[] = {0, 1, 8250, 8245, 0x825001, 0x84500001,
    CR_SM8250_DISPCC_HDK8245_DSI0 + 1, 0xffffffffU};
  UINT32 I, J;
  CHAR8 Key[160];
  CHECK(CrSm8250DispccGetProfile(CR_SM8250_DISPCC_HDK8245_DSI0, NULL) ==
    CR_INVALID_PARAMETER);
  CHECK(CrSm8250DispccGetProfile(CR_SM8250_DISPCC_HDK8245_DSI0, &P) == CR_SUCCESS);
  CHECK(P != NULL);
  for (I = 0; I < sizeof(Unsupported) / sizeof(Unsupported[0]); ++I) {
    Again = P;
    CHECK(CrSm8250DispccGetProfile(Unsupported[I], &Again) == CR_UNSUPPORTED);
    CHECK(Again == NULL);
  }
  CHECK(CrSm8250DispccGetProfile(CR_SM8250_DISPCC_HDK8245_DSI0, &Again) == CR_SUCCESS);
  CHECK(Again == P);
  N("profile", P, Id); N("profile", P, Soc); N("profile", P, Board);
  N("profile", P, DsiControllerMask);
  S("profile", P, AndroidCommit); S("profile", P, MainlineCommit); S("profile", P, FdtSha256);
  NUMBER("profile", "ControllerCount", CR_SM8250_DISPCC_CONTROLLER_COUNT);
  NUMBER("profile", "RcgCount", CR_SM8250_DISPCC_RCG_COUNT);
  NUMBER("profile", "BranchCount", CR_SM8250_DISPCC_BRANCH_COUNT);
  for (I = 0; I < CR_SM8250_DISPCC_CONTROLLER_COUNT; ++I) {
    CONST CR_SM8250_DISPCC_WINDOW *W = &P->Controllers[I];
    CHECK(W->Physical && W->Bytes && W->Physical + W->Bytes > W->Physical);
    snprintf(Key, sizeof(Key), "window.%s", W->Name);
    S(Key, W, Compatible); N(Key, W, Physical); N(Key, W, Bytes);
  }
  for (I = 0; I < CR_SM8250_DISPCC_RCG_COUNT; ++I) {
    CONST CR_SM8250_DISPCC_RCG *R = &P->Rcgs[I];
    CHECK(R->CommandOffset + 20 <= P->Controllers[CrSm8250DispccDisp].Bytes);
    CHECK((R->CommandOffset & 3) == 0 && R->HidWidth > 0 && R->HidWidth <= 8);
    CHECK(R->HidValue < (1U << R->HidWidth));
    CHECK(R->Parent < R->Id); /* This selected subset is topologically ordered. */
    for (J = 0; J < I; ++J) {
      CHECK(R->Id != P->Rcgs[J].Id);
      CHECK(strcmp(R->Name, P->Rcgs[J].Name) != 0);
      CHECK(R->CommandOffset != P->Rcgs[J].CommandOffset);
    }
    snprintf(Key, sizeof(Key), "rcg.%s", R->Name);
    N(Key, R, Id); N(Key, R, Parent); S(Key, R, ParentName); N(Key, R, CommandOffset);
    N(Key, R, HidWidth); N(Key, R, MndWidth); N(Key, R, Selector); N(Key, R, HidValue);
    N(Key, R, M); N(Key, R, N); N(Key, R, FixedHz);
    N(Key, R, AndroidSafeConfig); N(Key, R, AndroidHardwareControl);
  }
  for (I = 0; I < CR_SM8250_DISPCC_BRANCH_COUNT; ++I) {
    CONST CR_SM8250_DISPCC_BRANCH *B = &P->Branches[I];
    CHECK(B->Controller < CR_SM8250_DISPCC_CONTROLLER_COUNT);
    CHECK(B->EnableOffset + 4 <= P->Controllers[B->Controller].Bytes);
    CHECK(B->HaltOffset + 4 <= P->Controllers[B->Controller].Bytes);
    CHECK(B->HwcgOffset + 4 <= P->Controllers[B->Controller].Bytes);
    CHECK(((B->EnableOffset | B->HaltOffset | B->HwcgOffset) & 3) == 0);
    for (J = 0; J < I; ++J) CHECK(strcmp(B->Name, P->Branches[J].Name) != 0);
    snprintf(Key, sizeof(Key), "branch.%s", B->Name);
    N(Key, B, Controller); N(Key, B, Parent); N(Key, B, EnableOffset); N(Key, B, EnableMask);
    N(Key, B, HaltOffset); N(Key, B, HwcgOffset); N(Key, B, HwcgMask);
    N(Key, B, AndroidHalt); N(Key, B, MainlineHalt); N(Key, B, AndroidCritical);
  }
  S("divider", &P->Byte0Divider, Name); N("divider", &P->Byte0Divider, Id);
  N("divider", &P->Byte0Divider, Parent); N("divider", &P->Byte0Divider, Offset);
  N("divider", &P->Byte0Divider, Shift); N("divider", &P->Byte0Divider, Width);
  N("divider", &P->Byte0Divider, Divisor); N("divider", &P->Byte0Divider, ReadOnly);
  S("gdsc", &P->Gdsc, Name); N("gdsc", &P->Gdsc, Offset);
  N("gdsc", &P->Gdsc, FdtHardwareTrigger); N("gdsc", &P->Gdsc, FdtRetainRegisters);
  N("gdsc", &P->Gdsc, MainlineHardwareControl); N("gdsc", &P->Gdsc, MainlineRetainFlipFlops);
  N("gdsc", &P->Gdsc, MainlineEnableRestWait); N("gdsc", &P->Gdsc, MainlineEnableFewWait);
  N("gdsc", &P->Gdsc, MainlineClockDisableWait);
  S("pll1", &P->Pll1, Name); S("pll1", &P->Pll1, ParentName);
  N("pll1", &P->Pll1, Offset); N("pll1", &P->Pll1, DirectOwnerProfile);
  N("pll1", &P->Pll1, ReferenceHz); N("pll1", &P->Pll1, OutputHz);
  S("xo", &P->Xo, Name); S("xo", &P->Xo, ParentName); S("xo", &P->Xo, RpmhResource);
  N("xo", &P->Xo, BoardHz); N("xo", &P->Xo, OutputHz); N("xo", &P->Xo, Divider);
  N("xo", &P->Xo, EnableOffset); N("xo", &P->Xo, OnValue);
  N("xo", &P->Xo, ActiveSet); N("xo", &P->Xo, WakeSet); N("xo", &P->Xo, SleepSet);
  S("rails", &P->Rails, SupplyName); S("rails", &P->Rails, MmcxResource);
  S("rails", &P->Rails, MxResource); S("rails", &P->Rails, CxResource);
  N("rails", &P->Rails, MmcxBoardMinimum); N("rails", &P->Rails, MmcxCalibrationMinimum);
  N("rails", &P->Rails, MmcxOperatingMinimum); N("rails", &P->Rails, CxParentMinimum);
  N("rails", &P->Rails, ActiveSet); N("rails", &P->Rails, SleepSet);
  printf("rails.MxDropoutFromSelectedMmcx\t%d\n", P->Rails.MxDropoutFromSelectedMmcx);
  return 0;
}
