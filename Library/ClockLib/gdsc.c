#include "clock_internal.h"

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

STATIC BOOLEAN
ClockGdscRegisterFits (CONST ClockController *Controller, UINT64 Offset)
{
  if (Controller == NULL || Controller->Address == 0) return FALSE;
#ifndef _KERNEL_MODE
  if (Controller->Size == 0) return TRUE; /* legacy native mapped context */
#endif
  return Controller->Size >= sizeof (UINT32) &&
         Offset <= Controller->Size - sizeof (UINT32) &&
         Controller->Address <= (UINTN)-1 - Controller->Size;
}

STATIC BOOLEAN ClockGdscValid (CONST ClockNode *Gdsc)
{
  if (Gdsc == NULL || Gdsc->Type != CLOCK_NODE_TYPE_GDSC ||
      !ClockGdscRegisterFits (Gdsc->ParentController, Gdsc->GdscRegister) ||
      ((Gdsc->GdscFlags & CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG) != 0 &&
       !ClockGdscRegisterFits (Gdsc->ParentController,
                               (UINT64)Gdsc->GdscRegister + CLOCK_GDSC_CFG_REG_OFFSET)) ||
      (Gdsc->CollapseCtlMsk != 0 &&
       !ClockGdscRegisterFits (Gdsc->ParentController, Gdsc->CollapseCtlRegister)) ||
      (Gdsc->GdscHwCtrlRegister != 0 &&
       !ClockGdscRegisterFits (Gdsc->ParentController, Gdsc->GdscHwCtrlRegister)) ||
      ((Gdsc->GdscFlags & CLOCK_GDSC_FLAG_CLAMP_IO) != 0 &&
       !ClockGdscRegisterFits (Gdsc->ParentController, Gdsc->ClampIoCtlRegister)) ||
      Gdsc->CxcCount > 256U || (Gdsc->CxcCount != 0 && Gdsc->CxcRegisters == NULL)) {
    return FALSE;
  }
  for (UINTN Index = 0; Index < Gdsc->CxcCount; ++Index) {
    if (!ClockGdscRegisterFits (Gdsc->ParentController, Gdsc->CxcRegisters[Index]))
      return FALSE;
  }
  return TRUE;
}

CR_STATUS
ClockGdscToggleEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN BOOLEAN Enable, IN BOOLEAN Wait)
{
  UINT32 PollRegister;

  if (ClockContext == NULL || !ClockGdscValid (TargetClockNode) || Enable > 1) {
    return CR_INVALID_PARAMETER;
  }

  /* HW_CTL lets the hardware sequencer own collapse.  Linux clears this
   * ownership before requesting a software collapse; leaving it asserted
   * can keep DISPCC power-down completion stuck on SM8450. */
  if (!Enable && (TargetClockNode->GdscFlags & CLOCK_GDSC_FLAG_HW_CTL)) {
    CrMmioUpdateBits32(
        TargetClockNode->ParentController->Address + TargetClockNode->GdscRegister,
        CLOCK_GDSC_REG_HW_CTL_MSK, 0);
    cr_sleep(1);
  }

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
    /* A votable domain can remain ON due to another master. Qualify our
       software vote itself, including rollback, before interpreting status. */
    if (TargetClockNode->GdscFlags & CLOCK_GDSC_FLAG_VOTABLE) {
      CrMemoryFence ();
      if ((CrMmioRead32 (TargetClockNode->ParentController->Address + CollapseSetRegister) &
           CollapseSetMsk) != (Enable ? 0 : CollapseSetMsk)) return CR_DEVICE_ERROR;
    }
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
    PollRegister =
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
          if (TO_BOOL(GdscRegVal & CLOCK_GDSC_CFG_REG_PWR_UP_COMPL_MSK)) {
            log_info(
                "GDSC " CR_LOG_CHAR8_STR_FMT " is now enabled.",
                TargetClockNode->Name);
            return CR_SUCCESS;
          }
        }
        else {
          if (TO_BOOL(GdscRegVal & CLOCK_GDSC_CFG_REG_PWR_DOWN_COMPL_MSK)) {
            log_info(
                "GDSC " CR_LOG_CHAR8_STR_FMT " is now disabled.",
                TargetClockNode->Name);
            return CR_SUCCESS;
          }
        }
        cr_sleep(1);
        continue;
      }

      if (Enable == TO_BOOL(GdscRegVal & CLOCK_GDSC_REG_PWR_ON_MSK)) {
        log_info(
            "GDSC " CR_LOG_CHAR8_STR_FMT " is now " CR_LOG_CHAR8_STR_FMT ".", TargetClockNode->Name,
            Enable ? "enabled" : "disabled");
        return CR_SUCCESS;
      }

      cr_sleep(1);
    }
  }
  log_err(
      "Timeout waiting for GDSC " CR_LOG_CHAR8_STR_FMT " to be " CR_LOG_CHAR8_STR_FMT
      " control=0x%X status-register=0x%X status=0x%X",
      TargetClockNode->Name, Enable ? "enabled" : "disabled",
      CrMmioRead32(TargetClockNode->ParentController->Address +
                   TargetClockNode->GdscRegister),
      PollRegister,
      CrMmioRead32(TargetClockNode->ParentController->Address + PollRegister));

  // Ignore regulator disable for sm8450 gcc
  return CR_TIMEOUT;
}

STATIC
inline VOID ClockGdsc(
    IN ClockDriverContext *ClockContext, IN ClockNode *Gdsc, IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(ClockContext);
  CrMmioUpdateBits32(
      Gdsc->ParentController->Address + Gdsc->GdscRegister,
      CLOCK_GDSC_REG_HW_CTL_MSK, Enable ? CLOCK_GDSC_REG_HW_CTL_MSK : 0);
}

CR_STATUS
ClockGdscEnable(IN ClockDriverContext *Context, IN ClockNode *Gdsc)
{
  CR_STATUS Status;

  if (Context == NULL || !ClockGdscValid (Gdsc)) {
    return CR_INVALID_PARAMETER;
  }

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
  Status = ClockGdscToggleEnable(Context, Gdsc, TRUE, FALSE);
  if (CR_ERROR(Status)) {
    /* A timed-out domain must not be advertised to PCIe/display consumers
     * as powered.  Do not hand a failed enable to the hardware sequencer. */
    return Status;
  }

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

  log_info(
      "GDSC " CR_LOG_CHAR8_STR_FMT " enabled control-register=0x%X control=0x%X",
      Gdsc->Name, Gdsc->GdscRegister,
      CrMmioRead32(Gdsc->ParentController->Address + Gdsc->GdscRegister));

  return CR_SUCCESS;
}

CR_STATUS
ClockGdscDisable(IN ClockDriverContext *Context, IN ClockNode *Gdsc)
{
  if (Context == NULL || Gdsc == NULL) {
    return CR_INVALID_PARAMETER;
  }
  /* Votable withdrawal observes our collapse vote, not another master's
     aggregate power. Nonvotable domains retain their existing OFF wait. */
  return ClockGdscToggleEnable(Context, Gdsc, FALSE,
      (Gdsc->GdscFlags & CLOCK_GDSC_FLAG_VOTABLE) == 0);
}

CR_STATUS ClockGdscValidate (ClockDriverContext *Context, CONST ClockNode *Gdsc)
{
  return Context != NULL && Context->LockInitialized && ClockGdscValid (Gdsc) ?
    CR_SUCCESS : CR_INVALID_PARAMETER;
}

STATIC CR_STATUS
ClockGdscSetLocked (ClockDriverContext *Context, ClockNode *Gdsc, BOOLEAN Enable)
{
  CR_STATUS Status = CR_SUCCESS;
  UINT32 CollapseMask;
  UINTN CollapseAddress;
  UINTN ControlAddress;

  if (CR_ERROR (ClockGdscValidate (Context, Gdsc)) ||
      Enable > 1) return CR_INVALID_PARAMETER;
  CollapseMask = Gdsc->CollapseCtlMsk != 0 ? Gdsc->CollapseCtlMsk :
                                          CLOCK_GDSC_REG_SW_COLLAPSE_MSK;
  CollapseAddress = Gdsc->ParentController->Address +
      (Gdsc->CollapseCtlMsk != 0 ? Gdsc->CollapseCtlRegister : Gdsc->GdscRegister);
  ControlAddress = Gdsc->ParentController->Address + Gdsc->GdscRegister;
  if (Enable) {
    if (Gdsc->ReleasePending) { Status = CR_NOT_READY; goto Exit; }
    if (Gdsc->ReferenceCount != 0) {
      if (Gdsc->ReferenceCount == CR_MAX_UINT32) Status = CR_OUT_OF_RESOURCES;
      else ++Gdsc->ReferenceCount;
      goto Exit;
    }
    Gdsc->RestoreControlEnabled = (CrMmioRead32 (CollapseAddress) & CollapseMask) == 0;
    Gdsc->RestoreHwControlled = (CrMmioRead32 (ControlAddress) & CLOCK_GDSC_REG_HW_CTL_MSK) != 0;
    Status = ClockGdscEnable (Context, Gdsc);
    if (CR_ERROR (Status)) {
      CR_STATUS Rollback = ClockGdscToggleEnable (
          Context, Gdsc, Gdsc->RestoreControlEnabled,
          Gdsc->RestoreControlEnabled || !(Gdsc->GdscFlags & CLOCK_GDSC_FLAG_VOTABLE));
      if (CR_ERROR (Rollback)) {
        /* Hardware state is uncertain. Keep an owner so teardown retries the
         * restore before releasing the register mapping. */
        Gdsc->ReferenceCount = 1;
        Gdsc->ReleasePending = TRUE;
      } else {
        CrMmioUpdateBits32 (ControlAddress, CLOCK_GDSC_REG_HW_CTL_MSK,
                           Gdsc->RestoreHwControlled ? CLOCK_GDSC_REG_HW_CTL_MSK : 0);
        Gdsc->RestoreControlEnabled = FALSE;
        Gdsc->RestoreHwControlled = FALSE;
      }
      goto Exit;
    }
    Gdsc->ReferenceCount = 1;
  } else if (Gdsc->ReferenceCount > 1) {
    --Gdsc->ReferenceCount;
  } else if (Gdsc->ReferenceCount == 1) {
    if (Gdsc->RestoreControlEnabled) {
      /* Withdrawing our vote preserves an inherited power request. Avoid a
       * status wait here; a consumer's PHY/core reset can change status while
       * the inherited software control still correctly requests power. */
      CrMmioUpdateBits32 (CollapseAddress, CollapseMask, 0);
      if ((Gdsc->GdscFlags & CLOCK_GDSC_FLAG_VOTABLE) &&
          (CrMmioRead32 (CollapseAddress) & CollapseMask)) {
        Gdsc->ReleasePending = TRUE; Status = CR_DEVICE_ERROR; goto Exit;
      }
    } else {
      Status = ClockGdscDisable (Context, Gdsc);
    }
    if (CR_ERROR (Status)) {
      Gdsc->ReleasePending = TRUE;
      goto Exit;
    }
    CrMmioUpdateBits32 (ControlAddress, CLOCK_GDSC_REG_HW_CTL_MSK,
                       Gdsc->RestoreHwControlled ? CLOCK_GDSC_REG_HW_CTL_MSK : 0);
    Gdsc->ReferenceCount = 0;
    Gdsc->RestoreControlEnabled = FALSE;
    Gdsc->RestoreHwControlled = FALSE;
    Gdsc->ReleasePending = FALSE;
  }
Exit:
  CrMemoryFence ();
  return Status;
}

CR_STATUS ClockGdscSet (ClockDriverContext *Context, ClockNode *Gdsc, BOOLEAN Enable)
{
  CR_STATUS Status;
  if (CR_ERROR (ClockGdscValidate (Context, Gdsc)) || Enable > 1)
    return CR_INVALID_PARAMETER;
  CrLockAcquire (&Context->Lock);
  Status = ClockGdscSetLocked (Context, Gdsc, Enable);
  CrLockRelease (&Context->Lock);
  return Status;
}

CR_STATUS ClockGdscAcquire (ClockDriverContext *Context, ClockNode *Gdsc,
                            BOOLEAN *Owned)
{
  CR_STATUS Status;
  UINT32 Before;
  if (Owned == NULL) return CR_INVALID_PARAMETER;
  *Owned = FALSE;
  if (CR_ERROR (ClockGdscValidate (Context, Gdsc))) return CR_INVALID_PARAMETER;
  CrLockAcquire (&Context->Lock);
  Before = Gdsc->ReferenceCount;
  Status = ClockGdscSetLocked (Context, Gdsc, TRUE);
  *Owned = !CR_ERROR (Status) || Gdsc->ReferenceCount > Before;
  CrLockRelease (&Context->Lock);
  return Status;
}
