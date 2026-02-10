// #define CR_OSKAL_TRACE_MMIO 1
#include <clock_internal.h>
#define CLOCK_DEBUGCC_CTL_REG_COUNT_EN_MSK BIT(20)
#define CLOCK_DEBUGCC_CTL_REG_COUNT_CLR_MSK BIT(21)
#define CLOCK_DEBUGCC_CTL_REG_XO_DIV4_TERM_COUNT_MSK GEN_MSK(19, 0)

#define CLOCK_DEBUGCC_STATUS_REG_COUNT_DONE_MAX_WAIT_US 27000 // 27 ms
#define CLOCK_DEBUGCC_STATUS_REG_XO_DIV4_COUNT_DONE_MSK BIT(25)
#define CLOCK_DEBUGCC_STATUS_REG_MEASURE_COUNT_MSK GEN_MSK(24, 0)
#define CLOCK_DEBUGCC_SAMPLE_TICKS_1_MS 0x1000
#define CLOCK_DEBUGCC_SAMPLE_TICKS_27_MS 0x20000
#define CLOCK_DEBUGCC_TCXO_DIV_4_HZ 4800000ULL
#define CLOCK_DEBUGCC_CBCR_EN_MSK BIT(0)

UINT32
DebugccMeasureClockInternal(
    IN UINTN ControllerBaseAddress, IN DebugccDriverContext *Context,
    IN DebugccInfo *Clock, IN UINT64 SampleTicks)
{
  UINT32 MeasurementResult = 0;

  // Ensure measurement is disabled
  CrMmioUpdateBits32(
      ControllerBaseAddress + Context->CtlReg,
      CLOCK_DEBUGCC_CTL_REG_COUNT_EN_MSK, 0);
  CrMmioUpdateBits32(
      ControllerBaseAddress + Context->CtlReg,
      CLOCK_DEBUGCC_CTL_REG_COUNT_CLR_MSK, CLOCK_DEBUGCC_CTL_REG_COUNT_CLR_MSK);

  // Wait for timer to be ready
  cr_sleep(1);

  CrMmioUpdateBits32(
      ControllerBaseAddress + Context->CtlReg,
      CLOCK_DEBUGCC_CTL_REG_COUNT_CLR_MSK, 0);

  // Run measurement
  CrMmioUpdateBits32(
      ControllerBaseAddress + Context->CtlReg,
      CLOCK_DEBUGCC_CTL_REG_XO_DIV4_TERM_COUNT_MSK,
      (SampleTicks & CLOCK_DEBUGCC_CTL_REG_XO_DIV4_TERM_COUNT_MSK));

  // Start measurement
  CrMmioUpdateBits32(
      ControllerBaseAddress + Context->CtlReg,
      CLOCK_DEBUGCC_CTL_REG_COUNT_EN_MSK, CLOCK_DEBUGCC_CTL_REG_COUNT_EN_MSK);

  // Poll for done

  for (UINT32 i = 0; i < CLOCK_DEBUGCC_STATUS_REG_COUNT_DONE_MAX_WAIT_US; i++) {
    MeasurementResult =
        CrMmioRead32(ControllerBaseAddress + Context->StatusReg);
    if (MeasurementResult & CLOCK_DEBUGCC_STATUS_REG_XO_DIV4_COUNT_DONE_MSK) {
      break;
    }
    cr_sleep(1);
  }

  // Disable measurement
  CrMmioUpdateBits32(
      ControllerBaseAddress + Context->CtlReg,
      CLOCK_DEBUGCC_CTL_REG_COUNT_EN_MSK, 0);

  // Read measurement result
  MeasurementResult = CrMmioRead32(ControllerBaseAddress + Context->StatusReg);

  return (MeasurementResult & CLOCK_DEBUGCC_STATUS_REG_MEASURE_COUNT_MSK);
}

CR_STATUS
DebugccMeasureClockRate(IN CONST CHAR8 *ClockName, OUT UINT64 *FrequencyHz)
{
  // Get contexts
  DebugccDriverContext *Context          = CrTargetGetDebugccContext();
  ClockDriverContext   *ClockContext     = CrTargetGetClockContext();
  DebugccController   **TargetController = Context->ClockControllers;
  DebugccInfo          *TargetClock      = NULL;
  ClockControllerType   ControllerType   = 0;

  /* Input validation */
  if (ClockName == NULL || FrequencyHz == NULL || Context == NULL ||
      ClockContext == NULL) {
    log_err("Invalid parameters to DebugccMeasureClockRate");
    return CR_INVALID_PARAMETER;
  }

  // Find clock info by name
  while ((*TargetController) != NULL) {
    DebugccInfo **DebugClks = (*TargetController)->DebugClks;
    if (*DebugClks != NULL) {
      while (*DebugClks != NULL) {
        if (cr_strcmp((*DebugClks)->Name, ClockName) == 0) {
          TargetClock = *DebugClks;
          break;
        }
        DebugClks++;
      }
    }
    if (TargetClock != NULL)
      break; /* Found */
    TargetController++;
    ControllerType++;
  }

  // Check if clock found
  if (TargetClock == NULL || *TargetController == NULL) {
    log_err("Clock '" CR_LOG_CHAR8_STR_FMT "' not found in Debugcc context", ClockName);
    return CR_NOT_FOUND;
  }

  // For MCCC
  if (TargetClock->MuxSel == 0) {
    *FrequencyHz = 1000000000000ULL; // 1 THz
    *FrequencyHz /= CrMmioRead32(
        ClockContext->ClockControllers[ControllerType].Address +
        (*TargetController)->PeriodOffset);
    log_info("Measured clock '" CR_LOG_CHAR8_STR_FMT "' frequency: %llu Hz", ClockName, *FrequencyHz);
    return CR_SUCCESS;
  }
  else {
    // For other clocks
    // Set mux sel
    CrMmioUpdateBits32(
        ClockContext->ClockControllers[ControllerType].Address +
            (*TargetController)->DebugOffset,
        (*TargetController)->SrcSelMsk,
        (TargetClock->MuxSel << (*TargetController)->SrcSelSft) &
            (*TargetController)->SrcSelMsk);

    // Setup mux post div
    CrMmioUpdateBits32(
        ClockContext->ClockControllers[ControllerType].Address +
            (*TargetController)->PostDivOffset,
        (*TargetController)->PostDivMsk,
        (((*TargetController)->PostDivVal - 1)
         << (*TargetController)->PostDivSft) &
            (*TargetController)->PostDivMsk);

    if ((*TargetController)->EnMsk == 0) {
      (*TargetController)->EnMsk = CLOCK_DEBUGCC_CBCR_EN_MSK;
    }

    // Enable clock mux
    if ((*TargetController)->CbcrOffset != (UINT32)-1) {
      CrMmioUpdateBits32(
          ClockContext->ClockControllers[ControllerType].Address +
              (*TargetController)->CbcrOffset,
          (*TargetController)->EnMsk, (*TargetController)->EnMsk);
    }

    // Measure clock rate
    // Set bit 0 of XoDiv4
    CrMmioUpdateBits32(
        ClockContext->ClockControllers[CC_TYPE_GCC].Address +
            Context->XoDiv4CbcReg,
        BIT(0), BIT(0));

    // Run 1ms measurement
    UINT32 ShortMeasurementResult = DebugccMeasureClockInternal(
        ClockContext->ClockControllers[CC_TYPE_GCC].Address, Context,
        TargetClock, CLOCK_DEBUGCC_SAMPLE_TICKS_1_MS);

    // Run 27ms measurement
    UINT32 FullMeasurementResult = DebugccMeasureClockInternal(
        ClockContext->ClockControllers[CC_TYPE_GCC].Address, Context,
        TargetClock, CLOCK_DEBUGCC_SAMPLE_TICKS_27_MS);

    // clear bit 0 of XoDiv4
    CrMmioUpdateBits32(
        ClockContext->ClockControllers[CC_TYPE_GCC].Address +
            Context->XoDiv4CbcReg,
        BIT(0), 0);

    // Analyze results
    if (ShortMeasurementResult == FullMeasurementResult) {
      *FrequencyHz = 0;
    }
    else {
      CONST UINT8 Magnification = 1;
      *FrequencyHz =
          ((((FullMeasurementResult * 10) + 15) * CLOCK_DEBUGCC_TCXO_DIV_4_HZ) /
           ((CLOCK_DEBUGCC_SAMPLE_TICKS_27_MS * 10) + 35)) *
          Magnification;
    }

    // Calculate final frequency
    if ((*TargetController)->PostDivVal != 0)
      *FrequencyHz *= (*TargetController)->PostDivVal;

    if (TargetClock->PreDivVal != 0)
      *FrequencyHz *= TargetClock->PreDivVal;

    // Disable clock mux
    if ((*TargetController)->CbcrOffset != (UINT32)-1) {
      CrMmioUpdateBits32(
          ClockContext->ClockControllers[ControllerType].Address +
              (*TargetController)->CbcrOffset,
          (*TargetController)->EnMsk, 0);
    }
  }

  return CR_SUCCESS;
}

VOID DebugccDumpAllClocksFreq(VOID)
{
  DebugccDriverContext *Context          = CrTargetGetDebugccContext();
  DebugccController   **TargetController = Context->ClockControllers;
  UINT64                Frequency;

  if (Context == NULL || Context->ClockControllers == NULL) {
    log_err("Invalid DebugccContext");
    return;
  }

  log_info("=================================================================");
  log_info("Clock Frequency Measurement Results:");
  log_info("=================================================================");

  /* Iterate through all controllers until NULL terminator */
  while (*TargetController != NULL) {
    DebugccInfo **DebugClks = (*TargetController)->DebugClks;
    while (*DebugClks != NULL) {
      if (DebugccMeasureClockRate((*DebugClks)->Name, &Frequency) ==
          CR_SUCCESS) {
        if (Frequency == 0) {
          log_info("%-50a: OFF", (*DebugClks)->Name);
        }
        else {
          log_info(
              "%-50a: %llu Hz (%llu MHz)", (*DebugClks)->Name, Frequency,
              Frequency / 1000000);
        }
      }
      else {
        log_err("Failed to measure clock '" CR_LOG_CHAR8_STR_FMT "'", (*DebugClks)->Name);
      }
      DebugClks++;
    }
    TargetController++;
  }
  log_info("=================================================================");
}