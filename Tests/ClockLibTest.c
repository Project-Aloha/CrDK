/** @file
 *  Host tests for Linux-compatible Qualcomm branch and RCG2 sequencing.
 *  SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include <Uefi.h>
#include "../Library/ClockLib/clock.c"

#define TEST_BASE         0x00100000U
#define TEST_RCG_COMMAND  (TEST_BASE + 0x100U)
#define TEST_BRANCH_GATE  (TEST_BASE + 0x200U)
#define TEST_BRANCH_HALT  (TEST_BASE + 0x204U)
#define TEST_GATE_MASK    BIT(5)
#define MAX_REGISTERS     32U
#define MAX_WRITES        64U

typedef struct {
  UINTN  Address;
  UINT32 Value;
} TEST_REGISTER;

STATIC TEST_REGISTER mRegisters[MAX_REGISTERS];
STATIC TEST_REGISTER mWrites[MAX_WRITES];
STATIC UINTN         mRegisterCount;
STATIC UINTN         mWriteCount;
STATIC UINTN         mDelayUs;
STATIC UINTN         mHaltReads;
STATIC BOOLEAN       mAutoRoot;
STATIC BOOLEAN       mAutoUpdate;
STATIC BOOLEAN       mAutoBranch;
STATIC BOOLEAN       mInvertBranch;

STATIC TEST_REGISTER *
FindRegister(UINTN Address, BOOLEAN Create)
{
  UINTN Index;

  for (Index = 0; Index < mRegisterCount; ++Index) {
    if (mRegisters[Index].Address == Address) {
      return &mRegisters[Index];
    }
  }
  assert(Create && (mRegisterCount < MAX_REGISTERS));
  mRegisters[mRegisterCount].Address = Address;
  mRegisters[mRegisterCount].Value = 0;
  return &mRegisters[mRegisterCount++];
}

STATIC VOID
SetRegister(UINTN Address, UINT32 Value)
{
  FindRegister(Address, TRUE)->Value = Value;
}

STATIC UINT32
GetRegister(UINTN Address)
{
  return FindRegister(Address, TRUE)->Value;
}

UINT32 EFIAPI
MmioRead32(IN UINTN Address)
{
  if (Address == TEST_BRANCH_HALT) {
    ++mHaltReads;
  }
  return GetRegister(Address);
}

UINT32 EFIAPI
MmioWrite32(IN UINTN Address, IN UINT32 Value)
{
  UINT32 Previous;

  assert(mWriteCount < MAX_WRITES);
  mWrites[mWriteCount].Address = Address;
  mWrites[mWriteCount++].Value = Value;
  Previous = GetRegister(Address);
  if (Address == TEST_RCG_COMMAND) {
    /* ROOT_OFF is status-only.  Model the hardware response to ROOT_EN. */
    Value = (Value & ~CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_OFF_MSK) |
            (Previous & CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_OFF_MSK);
    if (mAutoRoot) {
      if ((Value & CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) != 0) {
        Value &= ~CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_OFF_MSK;
      } else {
        Value |= CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_OFF_MSK;
      }
    }
    if (mAutoUpdate) {
      Value &= ~CLOCK_NODE_RCG_CMD_REGISTER_CMD_UPDATE_MSK;
    }
  }
  SetRegister(Address, Value);
  if ((Address == TEST_BRANCH_GATE) && mAutoBranch) {
    BOOLEAN Enabled;
    UINT32  Halt;

    Enabled = ((Value & TEST_GATE_MASK) != 0);
    Halt = GetRegister(TEST_BRANCH_HALT);
    Halt &= ~CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK;
    if (mInvertBranch ? Enabled : !Enabled) {
      Halt |= CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK;
    }
    SetRegister(TEST_BRANCH_HALT, Halt);
  }
  return Value;
}

UINTN EFIAPI
MicroSecondDelay(IN UINTN MicroSeconds)
{
  mDelayUs += MicroSeconds;
  return MicroSeconds;
}

VOID EFIAPI
MemoryFence(VOID)
{
}

VOID EFIAPI
DebugPrint(IN UINTN ErrorLevel, IN CONST CHAR8 *Format, ...)
{
  (VOID)ErrorLevel;
  (VOID)Format;
}

SPIN_LOCK *EFIAPI
InitializeSpinLock(OUT SPIN_LOCK *SpinLock)
{
  *SpinLock = 1;
  return SpinLock;
}

SPIN_LOCK *EFIAPI
AcquireSpinLock(IN OUT SPIN_LOCK *SpinLock)
{
  assert(*SpinLock == 1);
  *SpinLock = 2;
  return SpinLock;
}

SPIN_LOCK *EFIAPI
ReleaseSpinLock(IN OUT SPIN_LOCK *SpinLock)
{
  assert(*SpinLock == 2);
  *SpinLock = 1;
  return SpinLock;
}

STATIC ClockController mController = {
  .Address = TEST_BASE,
};
STATIC ClockDriverContext mContext = {
  .ClockControllerCount = 1,
  .ClockControllers = &mController,
};
STATIC ClockRcgFreqTable mFrequency =
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(0, 2, 19200000U, 1, 0, 0);
STATIC ClockNode mRcg = {
  .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .Name = "test_rcg",
  .ParentController = &mController,
  .CmdRegister = TEST_RCG_COMMAND - TEST_BASE,
  .MNDWidth = 0,
  .HIDWidth = 5,
  .HwClockCtrl = TRUE,
  .FreqCount = 1,
  .FreqTable = &mFrequency,
};
STATIC ClockNode mBranch = {
  .Type = CLOCK_NODE_TYPE_BRANCH_2,
  .Name = "test_branch",
  .ParentController = &mController,
  .EnableRegister = TEST_BRANCH_GATE - TEST_BASE,
  .EnableMsk = TEST_GATE_MASK,
  .HaltRegister = TEST_BRANCH_HALT - TEST_BASE,
  .HaltCheckFlag = CLOCK_NODE_BRANCH_HALT,
};

STATIC VOID
ResetMock(VOID)
{
  memset(mRegisters, 0, sizeof(mRegisters));
  memset(mWrites, 0, sizeof(mWrites));
  mRegisterCount = 0;
  mWriteCount = 0;
  mDelayUs = 0;
  mHaltReads = 0;
  mAutoRoot = TRUE;
  mAutoUpdate = TRUE;
  mAutoBranch = TRUE;
  mInvertBranch = FALSE;
  mBranch.ParentCount = 0;
  mBranch.Parents = NULL;
  mBranch.ReferenceCount = 0;
  mBranch.RestoreControlEnabled = FALSE;
  mBranch.ActiveRateHz = 0;
  mBranch.HaltCheckFlag = CLOCK_NODE_BRANCH_HALT;
  mRcg.ReferenceCount = 0;
  mRcg.RestoreControlEnabled = FALSE;
  mRcg.ActiveRateHz = 0;
  SetRegister(
      TEST_RCG_COMMAND, CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_OFF_MSK);
  SetRegister(
      TEST_BRANCH_HALT, CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK);
}

STATIC VOID
TestRcgControl(VOID)
{
  ClockNode Invalid;

  ResetMock();
  assert(ClockEnable(&mContext, &mRcg, 19200000U, TRUE) == CR_SUCCESS);
  assert((GetRegister(TEST_RCG_COMMAND) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) != 0);
  assert((GetRegister(TEST_RCG_COMMAND) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_OFF_MSK) == 0);
  assert(ClockRcg2CheckEnable(&mContext, &mRcg));
  assert(ClockEnable(&mContext, &mRcg, 19200000U, FALSE) == CR_SUCCESS);
  assert((GetRegister(TEST_RCG_COMMAND) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) == 0);
  assert(!ClockRcg2CheckEnable(&mContext, &mRcg));

  ResetMock();
  SetRegister(
      TEST_RCG_COMMAND, CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK);
  assert(ClockEnable(&mContext, &mRcg, 19200000U, TRUE) == CR_SUCCESS);
  assert(ClockEnable(&mContext, &mRcg, 19200000U, FALSE) == CR_SUCCESS);
  assert((GetRegister(TEST_RCG_COMMAND) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) != 0);

  ResetMock();
  mAutoRoot = FALSE;
  assert(ClockEnable(&mContext, &mRcg, 19200000U, TRUE) == CR_TIMEOUT);
  assert(mDelayUs == CLOCK_RCG2_UPDATE_TIMEOUT_US);
  assert((GetRegister(TEST_RCG_COMMAND) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) == 0);

  ResetMock();
  mAutoUpdate = FALSE;
  assert(ClockEnable(&mContext, &mRcg, 19200000U, TRUE) == CR_TIMEOUT);
  assert(mDelayUs == CLOCK_RCG2_UPDATE_TIMEOUT_US);
  assert((GetRegister(TEST_RCG_COMMAND) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) == 0);

  Invalid = mRcg;
  Invalid.ParentController = NULL;
  assert(!ClockRcg2CheckEnable(&mContext, &Invalid));
  assert(ClockRcg2UpdateConfig(&mContext, &Invalid) == CR_INVALID_PARAMETER);
}

STATIC VOID
TestBranchStatus(VOID)
{
  ResetMock();
  assert(ClockEnable(&mContext, &mBranch, 0, TRUE) == CR_SUCCESS);
  assert((GetRegister(TEST_BRANCH_GATE) & TEST_GATE_MASK) != 0);
  assert((GetRegister(TEST_BRANCH_HALT) &
          CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK) == 0);
  assert(mHaltReads != 0);
  assert(ClockEnable(&mContext, &mBranch, 0, FALSE) == CR_SUCCESS);
  assert((GetRegister(TEST_BRANCH_GATE) & TEST_GATE_MASK) == 0);
  assert((GetRegister(TEST_BRANCH_HALT) &
          CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK) != 0);

  ResetMock();
  mInvertBranch = TRUE;
  mBranch.HaltCheckFlag = CLOCK_NODE_BRANCH_HALT_ENABLE;
  assert(ClockEnable(&mContext, &mBranch, 0, TRUE) == CR_SUCCESS);
  assert((GetRegister(TEST_BRANCH_HALT) &
          CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK) != 0);
  assert(mHaltReads != 0);

  ResetMock();
  mAutoBranch = FALSE;
  assert(ClockEnable(&mContext, &mBranch, 0, TRUE) == CR_TIMEOUT);
  assert(mDelayUs == CLOCK_NODE_HALT_WAIT_TIMEOUT_US);
  assert(mHaltReads == CLOCK_NODE_HALT_WAIT_TIMEOUT_US);

  ResetMock();
  mBranch.HaltCheckFlag = CLOCK_NODE_BRANCH_HALT_SKIP;
  assert(ClockEnable(&mContext, &mBranch, 0, TRUE) == CR_SUCCESS);
  assert(mHaltReads == 0);

  ResetMock();
  SetRegister(TEST_BRANCH_GATE, TEST_GATE_MASK);
  SetRegister(TEST_BRANCH_HALT, 0);
  assert(ClockEnable(&mContext, &mBranch, 0, TRUE) == CR_SUCCESS);
  assert(ClockEnable(&mContext, &mBranch, 0, FALSE) == CR_SUCCESS);
  assert((GetRegister(TEST_BRANCH_GATE) & TEST_GATE_MASK) != 0);

  ResetMock();
  assert(ClockEnable(&mContext, &mBranch, 0, FALSE) == CR_SUCCESS);
  assert(mWriteCount == 0);
}

STATIC VOID
TestDependencyOrderAndErrors(VOID)
{
  ClockNode *Parents[2];
  ClockNode  InvalidParent = {
    .Type = CLOCK_NODE_TYPE_GDSC,
    .Name = "invalid_parent",
  };

  ResetMock();
  Parents[0] = &mRcg;
  mBranch.ParentCount = 1;
  mBranch.Parents = Parents;
  assert(ClockEnable(&mContext, &mBranch, 19200000U, TRUE) == CR_SUCCESS);
  assert(mWrites[0].Address == TEST_RCG_COMMAND);
  assert(mWrites[mWriteCount - 1].Address == TEST_BRANCH_GATE);

  mWriteCount = 0;
  assert(ClockEnable(&mContext, &mBranch, 19200000U, FALSE) == CR_SUCCESS);
  assert(mWrites[0].Address == TEST_BRANCH_GATE);
  assert(mWrites[mWriteCount - 1].Address == TEST_RCG_COMMAND);

  ResetMock();
  Parents[0] = &InvalidParent;
  Parents[1] = &mRcg;
  mBranch.ParentCount = 2;
  mBranch.Parents = Parents;
  assert(ClockEnable(&mContext, &mBranch, 19200000U, TRUE) == CR_UNSUPPORTED);
  assert(mWriteCount == 0);

  ResetMock();
  Parents[0] = &InvalidParent;
  mBranch.ParentCount = 1;
  mBranch.Parents = Parents;
  mBranch.ReferenceCount = 1;
  InvalidParent.ReferenceCount = 1;
  assert(ClockEnable(&mContext, &mBranch, 19200000U, FALSE) == CR_UNSUPPORTED);
  assert(mWrites[0].Address == TEST_BRANCH_GATE);
  assert((GetRegister(TEST_BRANCH_GATE) & TEST_GATE_MASK) == 0);

  ResetMock();
  Parents[0] = &mRcg;
  mBranch.ParentCount = 1;
  mBranch.Parents = Parents;
  mAutoRoot = FALSE;
  assert(ClockEnable(&mContext, &mBranch, 19200000U, TRUE) == CR_TIMEOUT);
  for (UINTN Index = 0; Index < mWriteCount; ++Index) {
    assert(mWrites[Index].Address != TEST_BRANCH_GATE);
  }
}

STATIC VOID
TestSharedParentReference(VOID)
{
  ClockNode *Parents[1] = {&mRcg};
  ClockNode First;
  ClockNode Second;

  ResetMock();
  First = mBranch;
  First.HaltCheckFlag = CLOCK_NODE_BRANCH_HALT_SKIP;
  First.ParentCount = 1;
  First.Parents = Parents;
  Second = First;
  Second.Name = "second_branch";
  Second.EnableRegister = 0x208;
  Second.HaltRegister = 0x20c;

  assert(ClockEnable(&mContext, &First, 19200000U, TRUE) == CR_SUCCESS);
  assert(ClockEnable(&mContext, &Second, 100000000U, TRUE) == CR_BUSY);
  assert(mRcg.ReferenceCount == 1);
  assert(ClockEnable(&mContext, &Second, 19200000U, TRUE) == CR_SUCCESS);
  assert(mRcg.ReferenceCount == 2);
  assert(ClockEnable(&mContext, &First, 19200000U, FALSE) == CR_SUCCESS);
  assert(mRcg.ReferenceCount == 1);
  assert((GetRegister(TEST_RCG_COMMAND) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) != 0);
  assert(ClockEnable(&mContext, &Second, 19200000U, FALSE) == CR_SUCCESS);
  assert(mRcg.ReferenceCount == 0);
  assert((GetRegister(TEST_RCG_COMMAND) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) == 0);
}

int
main(VOID)
{
  CrLockInit(&mContext.Lock);
  mContext.LockInitialized = TRUE;
  TestRcgControl();
  TestBranchStatus();
  TestDependencyOrderAndErrors();
  TestSharedParentReference();
  puts("Clock: RCG/branch state, timeout, ordering and ownership passed");
  return 0;
}
