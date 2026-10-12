/** @file Host coverage for the SM8450 DISPCC PLL parent hook. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#undef NULL
#include <Uefi.h>

#include "../Library/Sm8450ClockLib/Sm8450Clock.c"
#include "../Target/Sm8450/CrClockTarget.c"

#define MOCK_BASE 0x10000000U

typedef struct {
  UINTN Address;
  UINT32 Value;
} MOCK_REG;

STATIC MOCK_REG mRegs[64];
STATIC UINTN mRegCount;
STATIC UINTN mDropWrite;
STATIC BOOLEAN mAckLatch;
STATIC BOOLEAN mLockOnRun;

STATIC UINTN
PllBaseFor (UINTN Address, UINT32 RegisterOffset)
{
  if (Address == MOCK_BASE + SM8450_DISP_PLL0_OFFSET + RegisterOffset) {
    return MOCK_BASE + SM8450_DISP_PLL0_OFFSET;
  }
  if (Address == MOCK_BASE + SM8450_DISP_PLL1_OFFSET + RegisterOffset) {
    return MOCK_BASE + SM8450_DISP_PLL1_OFFSET;
  }
  return 0;
}

STATIC UINT32 *
MockReg (UINTN Address)
{
  UINTN Index;

  for (Index = 0; Index < mRegCount; ++Index) {
    if (mRegs[Index].Address == Address) {
      return &mRegs[Index].Value;
    }
  }
  assert (mRegCount < ARRAY_SIZE (mRegs));
  mRegs[mRegCount].Address = Address;
  mRegs[mRegCount].Value = 0;
  return &mRegs[mRegCount++].Value;
}

UINT32 EFIAPI
MmioRead32 (IN UINTN Address)
{
  return *MockReg (Address);
}

UINT32 EFIAPI
MmioWrite32 (IN UINTN Address, IN UINT32 Value)
{
  UINT32 *Register;

  if (Address == mDropWrite) return Value;
  Register = MockReg (Address);
  if (PllBaseFor (Address, SM8450_DISP_PLL_MODE) != 0) {
    if ((Value & SM8450_DISP_PLL_LATCH_INPUT) != 0) {
      if (mAckLatch) {
        Value |= SM8450_DISP_PLL_LATCH_ACK;
      }
    } else {
      Value &= ~SM8450_DISP_PLL_LATCH_ACK;
    }
  }
  if ((PllBaseFor (Address, SM8450_DISP_PLL_OPMODE) != 0) &&
      (Value == SM8450_DISP_PLL_RUN) && mLockOnRun) {
    *MockReg (PllBaseFor (Address, SM8450_DISP_PLL_OPMODE) +
              SM8450_DISP_PLL_MODE) |=
      SM8450_DISP_PLL_LOCK_DET;
  }
  *Register = Value;
  return Value;
}

UINTN EFIAPI
MicroSecondDelay (IN UINTN MicroSeconds)
{
  return MicroSeconds;
}

VOID EFIAPI
MemoryFence (VOID)
{
}

VOID EFIAPI
DebugPrint (IN UINTN ErrorLevel, IN CONST CHAR8 *Format, ...)
{
  (VOID)ErrorLevel;
  (VOID)Format;
}

STATIC VOID
ResetMock (VOID)
{
  memset (mRegs, 0, sizeof (mRegs));
  mRegCount = 0;
  mDropWrite = 0;
  mAckLatch = TRUE;
  mLockOnRun = TRUE;
}

STATIC VOID
TestInitialRunAndCalibration (VOID)
{
  UINT32 L;
  UINT32 Alpha;

  ResetMock ();
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_MODE) =
    SM8450_DISP_PLL_PCAL_PENDING;
  assert (Sm8450DispPllEnable (MOCK_BASE, 600000000ULL) == CR_SUCCESS);
  assert ((*MockReg (MOCK_BASE + SM8450_DISP_PLL_L) & 0xFFFFU) == 31U);
  assert (*MockReg (MOCK_BASE + SM8450_DISP_PLL_ALPHA) == 0x4000U);
  assert ((*MockReg (MOCK_BASE + SM8450_DISP_PLL_MODE) &
           SM8450_DISP_PLL_OUTCTRL) != 0);
  assert (*MockReg (MOCK_BASE + SM8450_DISP_PLL_OPMODE) ==
          SM8450_DISP_PLL_RUN);

  Sm8450DispPllRateConfig (257142858ULL, &L, &Alpha);
  assert (L == 13U && Alpha == 0x6493U);
  Sm8450DispPllRateConfig (38399999ULL, &L, &Alpha);
  assert (L == 2U && Alpha == 0U);
}

STATIC VOID
TestRunningRateUpdateAndTimeout (VOID)
{
  UINTN Index;

  ResetMock ();
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_MODE) =
    SM8450_DISP_PLL_OUTCTRL | SM8450_DISP_PLL_LOCK_DET;
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_OPMODE) = SM8450_DISP_PLL_RUN;
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_L) = 31U;
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_ALPHA) = 0x4000U;
  assert (Sm8450DispPllEnable (MOCK_BASE, 450000000ULL) == CR_SUCCESS);
  assert ((*MockReg (MOCK_BASE + SM8450_DISP_PLL_L) & 0xFFFFU) == 23U);
  assert (*MockReg (MOCK_BASE + SM8450_DISP_PLL_ALPHA) == 0x7000U);

  /* A running parent takes the dynamic L/alpha path; static configure
   * registers must remain untouched. */
  for (Index = 0; Index < mRegCount; ++Index) {
    assert (mRegs[Index].Address != MOCK_BASE + SM8450_DISP_PLL_CONFIG);
    assert (mRegs[Index].Address != MOCK_BASE + SM8450_DISP_PLL_CONFIG_U);
  }

  ResetMock ();
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_MODE) = SM8450_DISP_PLL_OUTCTRL;
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_OPMODE) = SM8450_DISP_PLL_RUN;
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_L) = 31U;
  *MockReg (MOCK_BASE + SM8450_DISP_PLL_ALPHA) = 0x4000U;
  mAckLatch = FALSE;
  assert (Sm8450DispPllEnable (MOCK_BASE, 450000000ULL) == CR_TIMEOUT);
}

STATIC VOID
TestPll1MuxDividerContract (VOID)
{
  ClockNode Node;
  ClockRcgFreqTable Frequency;
  UINTN SavedAddress;

  SavedAddress = Sm8450ClockControllers[CC_TYPE_DISP_CC].Address;
  Sm8450ClockControllers[CC_TYPE_DISP_CC].Address = MOCK_BASE;
  memset (&Node, 0, sizeof (Node));
  Node.ParentController = &Sm8450ClockControllers[CC_TYPE_DISP_CC];
  Frequency.Source.Config = 6U; /* DISP PLL1 even output */
  Frequency.PreDiv = 5U;       /* HID divider 3 */
  Frequency.FrequencyHz = 200000000ULL;
  ResetMock ();
  *MockReg (MOCK_BASE + SM8450_DISP_PLL1_OFFSET +
            SM8450_DISP_PLL_MODE) = SM8450_DISP_PLL_PCAL_PENDING;
  assert (Sm8450ClockPrepareSource (
            &Sm8450ClockContext, &Node, &Frequency) == CR_SUCCESS);
  assert ((*MockReg (MOCK_BASE + SM8450_DISP_PLL1_OFFSET +
                     SM8450_DISP_PLL_L) & 0xFFFFU) == 31U);
  assert (*MockReg (MOCK_BASE + SM8450_DISP_PLL1_OFFSET +
                    SM8450_DISP_PLL_ALPHA) == 0x4000U);
  Sm8450ClockControllers[CC_TYPE_DISP_CC].Address = SavedAddress;
}

VOID *EFIAPI SetMem (VOID *Buffer, UINTN Length, UINT8 Value)
{ return memset (Buffer, Value, Length); }
INTN EFIAPI AsciiStrCmp (CONST CHAR8 *Left, CONST CHAR8 *Right)
{ return strcmp (Left, Right); }
SPIN_LOCK *EFIAPI InitializeSpinLock (SPIN_LOCK *Lock)
{ *Lock = 1; return Lock; }
SPIN_LOCK *EFIAPI AcquireSpinLock (SPIN_LOCK *Lock)
{ assert (*Lock == 1); *Lock = 2; return Lock; }
SPIN_LOCK *EFIAPI ReleaseSpinLock (SPIN_LOCK *Lock)
{ assert (*Lock == 2); *Lock = 1; return Lock; }

STATIC VOID TestIndependentGraphsAndMappings (VOID)
{
  UINTN Size = CrSm8450ClockStateSize ();
  VOID *First = calloc (1, Size);
  VOID *Second = calloc (1, Size);
  ClockDriverContext *A = NULL;
  ClockDriverContext *B = NULL;
  CR_CLOCK_MAPPING Mappings[CC_TYPE_MAX];
  UINT64 Physical;
  UINT32 Bytes;
  assert (First != NULL && Second != NULL);
  ResetMock ();
  assert (CrSm8450ClockCreate (First, Size - 1, &A) == CR_INVALID_PARAMETER);
  assert (A == NULL && mRegCount == 0);
  assert (CrSm8450ClockCreate ((UINT8 *)First + 1, Size, &A) == CR_INVALID_PARAMETER);
  assert (CrSm8450ClockCreate (First, Size, &A) == CR_SUCCESS);
  assert (CrSm8450ClockCreate (Second, Size, &B) == CR_SUCCESS);
  assert (mRegCount == 0);
  SM8450_CLOCK_STATE *SA = First;
  SM8450_CLOCK_STATE *SB = Second;
  assert (SA->NodeCount > 50 && SA->NodeCount == SB->NodeCount);
  for (UINTN Index = 0; Index < SA->NodeCount; ++Index) {
    ClockNode *Node = &SA->Nodes[Index];
    assert (Node->ParentController >= SA->Controllers &&
            Node->ParentController < SA->Controllers + CC_TYPE_MAX);
    assert (Node->ReferenceCount == 0 && Node->ActiveRateHz == 0);
    for (UINTN Parent = 0; Parent < Node->ParentCount; ++Parent)
      assert (Node->Parents[Parent] >= SA->Nodes &&
              Node->Parents[Parent] < SA->Nodes + SA->NodeCount);
  }
  const UINT32 SerialIds[]={GCC_QUPV3_WRAP0_S1_CLK,GCC_QUPV3_WRAP0_S3_CLK,
    GCC_QUPV3_WRAP1_S0_CLK,GCC_QUPV3_WRAP1_S2_CLK,GCC_QUPV3_WRAP1_S5_CLK};
  const UINT32 SerialHalts[]={0x27140,0x273a8,0x2800c,0x28274,0x28610};
  const UINT32 SerialCmds[]={0x27148,0x273b0,0x28014,0x2827c,0x28618};
  const UINT32 SerialBits[]={11,13,22,24,27};
  for(UINT32 I=0;I<ARRAY_SIZE(SerialIds);++I){
    ClockNode *N=SA->GccClocks[SerialIds[I]];
    assert(N&&N!=SB->GccClocks[SerialIds[I]]&&N->Type==CLOCK_NODE_TYPE_BRANCH_2);
    assert(N->HaltRegister==SerialHalts[I]&&N->EnableRegister==0x62008&&N->EnableMsk==BIT(SerialBits[I]));
    assert(N->HaltCheckFlag==CLOCK_NODE_BRANCH_VOTED&&N->HwcgRegister==0&&N->ParentCount==1);
    ClockNode *P=N->Parents[0];assert(P->CmdRegister==SerialCmds[I]&&P->MNDWidth==16&&P->HIDWidth==5);
    assert(P->FreqCount==1&&P->FreqTable[0].FrequencyHz==19200000);
  }
  ClockNode *LeafA = SA->GccClocks[GCC_QUPV3_WRAP0_S5_CLK];
  ClockNode *LeafB = SB->GccClocks[GCC_QUPV3_WRAP0_S5_CLK];
  assert (LeafA != LeafB && LeafA != &GccQup0S5Clk);
  assert (LeafA->ParentCount == 1 && LeafB->ParentCount == 1);
  assert (LeafA->Parents[0] == SA->GccClocks[GCC_QUPV3_WRAP0_S5_CLK_SRC]);
  assert (LeafB->Parents[0] == SB->GccClocks[GCC_QUPV3_WRAP0_S5_CLK_SRC]);
  LeafA->ReferenceCount = 4; LeafA->Parents[0]->ReferenceCount = 2;
  assert (LeafB->ReferenceCount == 0 && LeafB->Parents[0]->ReferenceCount == 0);
  assert (GccQup0S5Clk.ReferenceCount == 0 && GccQup0S5ClkSrc.ReferenceCount == 0);
  for (UINTN Index = 0; Index < CC_TYPE_MAX; ++Index) {
    assert (CrSm8450ClockResource ((ClockControllerType)Index, &Physical, &Bytes) == CR_SUCCESS);
    Mappings[Index].Address = 0x20000000U + Index * 0x1000000U;
    Mappings[Index].Size = Bytes;
  }
  assert (CrSm8450ClockResource ((ClockControllerType)-1, &Physical, &Bytes) == CR_INVALID_PARAMETER);
  Mappings[CC_TYPE_DISP_CC].Size--;
  assert (CrSm8450ClockBind (A, Mappings, CC_TYPE_MAX) == CR_BAD_BUFFER_SIZE);
  for (UINTN Index = 0; Index < CC_TYPE_MAX; ++Index)
    assert (A->ClockControllers[Index].Address == 0 && !A->ClockControllers[Index].MemMapped);
  Mappings[CC_TYPE_DISP_CC].Size++;
  Mappings[CC_TYPE_MC_CC].Address = (UINTN)-4;
  assert (CrSm8450ClockBind (A, Mappings, CC_TYPE_MAX) == CR_BAD_BUFFER_SIZE);
  Mappings[CC_TYPE_MC_CC].Address = 0x26000000U;
  assert (CrSm8450ClockBind (A, Mappings, CC_TYPE_MAX) == CR_SUCCESS);
  assert (mRegCount == 0);
  UINT32 SavedSize = A->ClockControllers[CC_TYPE_DISP_CC].Size;
  A->ClockControllers[CC_TYPE_DISP_CC].Size = SM8450_DISP_CC_MISC_CMD_OFFSET + 3;
  assert (CrSm8450ClockInit (A) == CR_BAD_BUFFER_SIZE);
  assert (mRegCount == 0 && A->PrepareSource == NULL);
  A->ClockControllers[CC_TYPE_DISP_CC].Size = SavedSize;
  assert (CrSm8450ClockInit (A) == CR_SUCCESS && A->PrepareSource != NULL);
  CrLockInit (&A->Lock); A->LockInitialized = TRUE;
  UINTN ResetAddress = A->ClockControllers[CC_TYPE_GCC].Address + 0x9d000U;
  *MockReg (ResetAddress) = 0xa5a50004;
  assert (CrSm8450ClockReset (A, "gcc", "gcc_pcie_1_bcr", TRUE) == CR_SUCCESS);
  assert (*MockReg (ResetAddress) == 0xa5a50005);
  assert (CrSm8450ClockReset (A, "gcc", "gcc_pcie_1_bcr", FALSE) == CR_SUCCESS);
  assert (*MockReg (ResetAddress) == 0xa5a50004);
  CONST CHAR8 *ResetNames[] = {"gcc_usb3_dp_phy_prim_bcr", "gcc_usb3_phy_prim_bcr", "gcc_qusb2phy_prim_bcr"};
  UINT32 ResetOffsets[] = {0x60008, 0x60000, 0x22000};
  for (UINTN Index = 0; Index < ARRAY_SIZE (ResetNames); ++Index) {
    UINTN Address = A->ClockControllers[CC_TYPE_GCC].Address + ResetOffsets[Index];
    *MockReg (Address) = 0xa5a50004;
    for (UINTN Step = 0; Step < 2; ++Step) {
      BOOLEAN Assert = Step == 0;
      mDropWrite = Address;
      assert (CrSm8450ClockReset (A, "gcc", ResetNames[Index], Assert) == CR_DEVICE_ERROR);
      mDropWrite = 0;
      assert (CrSm8450ClockReset (A, "gcc", ResetNames[Index], Assert) == CR_SUCCESS);
      assert (*MockReg (Address) == (0xa5a50004U | Assert));
    }
  }
  UINTN Registers = mRegCount;
  A->ClockControllers[CC_TYPE_GCC].Size = 0x9d003;
  assert (CrSm8450ClockReset (A, "gcc", "gcc_pcie_1_bcr", TRUE) == CR_BAD_BUFFER_SIZE);
  assert (*MockReg (ResetAddress) == 0xa5a50004 && mRegCount == Registers);
  assert (CrSm8450ClockReset (A, "gcc", "unknown", TRUE) == CR_UNSUPPORTED);
  free (Second); free (First);
}

int
main (VOID)
{
  assert(mGccClocks[GCC_USB30_PRIM_MASTER_CLK] == &GccUsbMaster);
  assert(GccUsbMasterSrc.CmdRegister == 0x49028 && GccUsbMasterSrc.FreqTable[0].FrequencyHz == 200000000);
  assert(GccUsbPipe.HaltRegister == 0x49064 && GccUsbPipeSrc.MuxRegister == 0x49068);
  assert(GccUsbGdsc.GdscRegister == 0x49004);
  assert(DispDpAuxSrc.CmdRegister == 0x81EC && DispDpLinkSrc.CmdRegister == 0x819C);
  assert(DispDpPixelSrc.CmdRegister == 0x81BC && DispDpPixelSrc.MNDWidth == 16);
  assert(DispDpLinkIntf.EnableRegister == 0x8048);
  /* Public clock IDs must resolve to operational nodes, not just names in
     the generated inventory. These offsets/masks are from gcc-sm8450.c. */
  assert (mGccClocks[GCC_QUPV3_WRAP_0_M_AHB_CLK] == &GccQup0MAhbClk);
  assert (mGccClocks[GCC_QUPV3_WRAP_0_S_AHB_CLK] == &GccQup0SAhbClk);
  assert (mGccClocks[GCC_QUPV3_WRAP0_S5_CLK] == &GccQup0S5Clk);
  assert (mGccClocks[GCC_QUPV3_WRAP0_S5_CLK_SRC] == &GccQup0S5ClkSrc);
  assert (GccQup0MAhbClk.HaltRegister == 0x27004 && GccQup0MAhbClk.EnableMsk == BIT(6));
  assert (GccQup0SAhbClk.HaltRegister == 0x27008 && GccQup0SAhbClk.EnableMsk == BIT(7));
  assert (GccQup0S5Clk.HaltRegister == 0x27610 && GccQup0S5Clk.EnableMsk == BIT(15));
  assert (GccQup0S5Clk.EnableRegister == 0x62008 && GccQup0S5Clk.HwcgRegister == 0);
  assert (GccQup0S5ClkSrc.CmdRegister == 0x27618 && GccQup0S5ClkSrc.MNDWidth == 16);
  assert (GccQup0S5ClkSrc.FreqTable[0].FrequencyHz == 19200000);
  assert (GccQup0S5ClkSrc.FreqTable[0].Source.Config == 0);
  assert (mQupS5BranchParents[0] == &GccQup0S5ClkSrc);
  assert (ARRAY_SIZE (mDispMdpFreq) == 9U);
  assert (mDispMdpFreq[5].FrequencyHz == 200000000ULL);
  assert (mDispMdpFreq[5].Source.Config ==
          SM8450_CLOCK_RCG2_SRC_DISP_PLL0);
  assert (mDispMdpFreq[5].PreDiv == 5U);
  ClockNode *UfsSources[] = { &GccUfsPhyAxiClkSrc, &GccUfsPhyIceCoreClkSrc,
                            &GccUfsPhyUniproClkSrc };
  for (UINTN Index = 0; Index < ARRAY_SIZE (UfsSources); Index++) {
    ClockRcgFreqTable *BootRate = &UfsSources[Index]->FreqTable[2];
    assert (UfsSources[Index]->FreqCount == 3);
    assert (BootRate->FrequencyHz == 300000000ULL && BootRate->PreDiv == 3);
    assert (BootRate->Source.Source == SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN);
    assert (BootRate->Source.Config == 1);
  }
  TestInitialRunAndCalibration ();
  TestRunningRateUpdateAndTimeout ();
  TestPll1MuxDividerContract ();
  TestIndependentGraphsAndMappings ();
  puts ("Clock target: QUP0, DISPCC PLL, independent graphs, atomic mapping bounds and translated reset passed");
  return 0;
}
