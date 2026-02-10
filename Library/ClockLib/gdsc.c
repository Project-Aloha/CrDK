#include <clock_internal.h>

#define CLOCK_GDSC_REG_PWR_ON_MSK BIT(31)
#define CLOCK_GDSC_REG_EN_REST_WAIT_MSK GEN_MSK(23, 20)
#define CLOCK_GDSC_REG_EN_FEW_WAIT_MSK GEN_MSK(19, 16)
#define CLOCK_GDSC_REG_CLK_DIS_WAIT_MSK GEN_MSK(15, 12)
#define CLOCK_GDSC_REG_SW_OVERRIDE_MSK BIT(2)
#define CLOCK_GDSC_REG_HW_CTL_MSK BIT(1)
#define CLOCK_GDSC_REG_SW_COLLAPSE_MSK BIT(0)
#define CLOCK_GDSC_REG_GMEM_CLAMP_IO_MSK BIT(0)
#define CLOCK_GDSC_REG_GMEM_RESET_MSK BIT(4)

#define CLOCK_GDSC_CFG_REG_PWR_UP_COMPL_MSK BIT(16)
#define CLOCK_GDSC_CFG_REG_PWR_DOWN_COMPL_MSK BIT(15)
#define CLOCK_GDSC_CFG_REG_RETAIN_FF_EN_MSK BIT(11)

#define CLOCK_GDSC_CFG_REG_OFFSET 4

#define CLOCK_GDSC_FLAG_VOTABLE BIT(0)
#define CLOCK_GDSC_FLAG_CLAMP_IO BIT(1)
#define CLOCK_GDSC_FLAG_SW_RESET BIT(2)
#define CLOCK_GDSC_FLAG_AON_RESET BIT(3)
#define CLOCK_GDSC_FLAG_RETAIN_FF_EN BIT(4)
#define CLOCK_GDSC_FLAG_HW_CTL BIT(5)
#define CLOCK_GDSC_FLAG_ALWAYS_ON BIT(6)
#define CLOCK_GDSC_FLAG_NOT_RET_PERIPH BIT(7)
#define CLOCK_GDSC_FLAG_HW_CTL_TRIGGER BIT(8)
#define CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG BIT(9)

#define CLOCK_GDSC_CXC_RETAIN_MEM_MSK BIT(14)
#define CLOCK_GDSC_CXC_RETAIN_PERIPH_MSK BIT(13)

CR_STATUS
ClockGdscToggleEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN BOOLEAN Enable, IN BOOLEAN Wait)
{

  // Ignore regulator enable for sm8450 gcc

  // Set collapse
  {
    UINT32 CollapseSetRegister = TargetClockNode->CollapseCtlMsk
                                     ? TargetClockNode->CollapseCtlRegister
                                     : TargetClockNode->GdscRegister;

    UINT32 CollapseSetMsk = TargetClockNode->CollapseCtlMsk
                                ? TargetClockNode->CollapseCtlMsk
                                : CLOCK_GDSC_REG_SW_COLLAPSE_MSK;
    // Set collapse bit to disable, clr to enable
    CrMmioUpdateBits32(
        TargetClockNode->ParentController->Address + CollapseSetRegister,
        CollapseSetMsk, Enable ? 0 : CollapseSetMsk);
  }

#define CLOCK_GDSC_HW_CTL_WAIT_TIMEOUT_US 500 // 500us in reference code
#define CLOCK_GDSC_POLL_TIMEOUT_US 2000       // 2ms in reference code

  if ((TargetClockNode->GdscFlags & CLOCK_GDSC_FLAG_VOTABLE) && (!Enable) &&
      !Wait) {
    cr_sleep(CLOCK_GDSC_POLL_TIMEOUT_US);
    return CR_SUCCESS;
  }

  if (TargetClockNode->GdscHwCtrlRegister)
    cr_sleep(1); // 1us delay for gdsc stable

  // Poll gdsc status
  {
    UINT32 PollRegister =
        (TargetClockNode->GdscFlags & CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG)
            ? (TargetClockNode->GdscRegister + CLOCK_GDSC_CFG_REG_OFFSET)
            : (TargetClockNode->GdscHwCtrlRegister
                   ? TargetClockNode->GdscHwCtrlRegister
                   : TargetClockNode->GdscRegister);

    for (UINT32 i = 0; i < CLOCK_GDSC_POLL_TIMEOUT_US; i++) {
      UINT32 GdscRegVal = CrMmioRead32(
          TargetClockNode->ParentController->Address + PollRegister);

      if (TargetClockNode->GdscFlags & CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG) {
        if (Enable) {
          if (TO_BOOL(GdscRegVal & CLOCK_GDSC_CFG_REG_PWR_UP_COMPL_MSK))
            break;
        }
        else {
          if (TO_BOOL(GdscRegVal & CLOCK_GDSC_CFG_REG_PWR_DOWN_COMPL_MSK))
            break;
        }
        return CR_SUCCESS;
      }

      if (Enable == TO_BOOL(GdscRegVal & CLOCK_GDSC_REG_PWR_ON_MSK)) {
        log_info(
            "GDSC " CR_LOG_CHAR8_STR_FMT " is now " CR_LOG_CHAR8_STR_FMT ".", TargetClockNode->Name,
            Enable ? "enabled" : "disabled");
        return CR_SUCCESS;
      }

      MicroSecondDelay(1);
    }
  }
  log_err(
      "Timeout waiting for GDSC " CR_LOG_CHAR8_STR_FMT " to be " CR_LOG_CHAR8_STR_FMT, TargetClockNode->Name,
      Enable ? "enabled" : "disabled");

  // Ignore regulator disable for sm8450 gcc
  return CR_TIMEOUT;
}

STATIC
inline VOID ClockGdsc(
    IN ClockDriverContext *ClockContext, IN ClockNode *Gdsc, IN BOOLEAN Enable)
{
  CrMmioUpdateBits32(
      Gdsc->ParentController->Address + Gdsc->GdscRegister,
      CLOCK_GDSC_REG_HW_CTL_MSK, Enable ? CLOCK_GDSC_REG_HW_CTL_MSK : 0);
}

CR_STATUS
ClockGdscEnable(IN ClockDriverContext *Context, IN ClockNode *Gdsc)
{
  // Print Name
  log_info("Enabling GDSC: " CR_LOG_CHAR8_STR_FMT, Gdsc->Name);

  // // Set enable
  // CrMmioUpdateBits32(
  //     Gdsc->ParentController->Address + Gdsc->GdscRegister,
  //     CLOCK_GDSC_REG_SW_COLLAPSE_MSK, 0);

  // // Check if already enabled
  // for (int i = 0; i < 2000; i++) {
  //   // Poll gdsc status
  //   if (CrMmioRead32(Gdsc->ParentController->Address + Gdsc->GdscRegister) &
  //       BIT(31)) {
  //     log_info("GDSC " CR_LOG_CHAR8_STR_FMT " is already enabled.", Gdsc->Name);
  //     return CR_SUCCESS;
  //   }
  // }
  // return CR_TIMEOUT;

  // Ignore for sm8450 gcc
  if (Gdsc->PwrStsFlag == CLOCK_NODE_GDSC_PWR_STS_ON) {
    // TODO Deassert reset and return
  }

  // Ignore for sm8450 gcc
  if (Gdsc->GdscFlags & CLOCK_GDSC_FLAG_SW_RESET) {
    // TODO assert reset
    cr_sleep(1); // 1us delay
    // TODO deassert reset
  }

  if (Gdsc->GdscFlags & CLOCK_GDSC_FLAG_CLAMP_IO) {
    if (Gdsc->GdscFlags & CLOCK_GDSC_FLAG_AON_RESET) {
      // Assert reset aon
      // perhaps referenced code has a bug here, it write 1 while updating
      // regmap.
      CrMmioUpdateBits32(
          Gdsc->ParentController->Address + Gdsc->ClampIoCtlRegister,
          CLOCK_GDSC_REG_GMEM_RESET_MSK, CLOCK_GDSC_REG_GMEM_RESET_MSK);
      cr_sleep(1); // 1us delay
      CrMmioUpdateBits32(
          Gdsc->ParentController->Address + Gdsc->ClampIoCtlRegister,
          CLOCK_GDSC_REG_GMEM_RESET_MSK, 0);
    }

    // deassert clamp io
    CrMmioUpdateBits32(
        Gdsc->ParentController->Address + Gdsc->ClampIoCtlRegister,
        CLOCK_GDSC_REG_GMEM_CLAMP_IO_MSK, 0);
  }

  // toggle logic
  ClockGdscToggleEnable(Context, Gdsc, TRUE, FALSE);

  if (Gdsc->PwrStsFlag & CLOCK_NODE_GDSC_PWR_STS_OFF) {
    // Force mem on
    UINT32 Mask = CLOCK_GDSC_CXC_RETAIN_MEM_MSK;
    if (!(Gdsc->GdscFlags & CLOCK_GDSC_FLAG_NOT_RET_PERIPH))
      Mask |= CLOCK_GDSC_CXC_RETAIN_PERIPH_MSK;

    for (UINT16 i = 0; i < Gdsc->CxcCount; i++) {
      CrMmioUpdateBits32(
          Gdsc->ParentController->Address + Gdsc->CxcRegisters[i], Mask, Mask);
    }
  }

  // Wait for power up complete
  cr_sleep(1); // 1us delay

  if (Gdsc->GdscFlags & CLOCK_GDSC_FLAG_RETAIN_FF_EN) {
    // enable retain ff
    CrMmioUpdateBits32(
        Gdsc->ParentController->Address + Gdsc->GdscRegister,
        CLOCK_GDSC_CFG_REG_RETAIN_FF_EN_MSK,
        CLOCK_GDSC_CFG_REG_RETAIN_FF_EN_MSK);
  }

  if (Gdsc->GdscFlags & CLOCK_GDSC_FLAG_HW_CTL) {
    // gdsc hw ctl
    ClockGdsc(Context, Gdsc, TRUE);
    cr_sleep(1); // 1us delay
  }

  return CR_SUCCESS;
}