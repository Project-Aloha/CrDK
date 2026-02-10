/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include "clock_internal.h"
#include <oskal/cr_memmap.h>

BOOLEAN ClockRcg2CheckEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode)
{
  if (ClockContext == NULL || TargetClockNode == NULL ||
      TargetClockNode->Type != CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) {
    log_err("Invalid ClockNode provided for RCG2 CheckEnable.");
    return FALSE;
  }

  UINT32 Val = CrMmioRead32(
      TargetClockNode->ParentController->Address +
      TargetClockNode->CmdRegister +
      CLOCK_NODE_RCG_CMD_REGISTER_CMD_REG_OFFSET);
  return !(Val & CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_OFF_MSK);
}

CR_STATUS
GetClockNode(
    IN ClockDriverContext *ClockContext, IN CONST CHAR8 *ClockName,
    IN OUT ClockControllerType *ControllerType, IN OUT UINT32 *ClockId,
    OUT ClockNode **TargetClockNode)
{
  if ((ClockContext == NULL) && (ControllerType == NULL) && (ClockId == NULL)) {
    log_err("Invalid Parameter provided.");
    return CR_INVALID_PARAMETER;
  }

  // Locate by ControllerType and ClockId if ClockName is not provided
  if (!ClockName) {
    if (*ClockId >= ClockContext->ClockControllers[*ControllerType].ClkCount) {
      log_err(
          "Invalid ClockId %u for ControllerType %u", *ClockId,
          *ControllerType);
      return CR_INVALID_PARAMETER;
    }
    *TargetClockNode =
        ClockContext->ClockControllers[*ControllerType].Clks[*ClockId];
  }
  else {
    // Locate by ClockName
    for (UINTN i = 0; i < ClockContext->ClockControllerCount; i++) {
      ClockController *Controller = &ClockContext->ClockControllers[i];
      for (UINTN j = 0; j < Controller->ClkCount; j++) {
        ClockNode *ClkNode = Controller->Clks[j];
        if (cr_strcmp(ClkNode->Name, ClockName) == 0) {
          *TargetClockNode = ClkNode;
          if (ControllerType)
            *ControllerType = (ClockControllerType)i;
          if (ClockId)
            *ClockId = (UINT32)j;
          break;
        }
      }
    }
  }

  if (*TargetClockNode == NULL) {
    log_err("Failed to find target clock.");
    return CR_NOT_FOUND;
  }
  return CR_SUCCESS;
}

CR_STATUS
ClockRcg2UpdateConfig(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode)
{
  // Write update bit in cmd rcg update bit
  CrMmioUpdateBits32(
      TargetClockNode->ParentController->Address +
          TargetClockNode->CmdRegister +
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_REG_OFFSET,
      CLOCK_NODE_RCG_CMD_REGISTER_CMD_UPDATE_MSK,
      CLOCK_NODE_RCG_CMD_REGISTER_CMD_UPDATE_MSK);

  // Wait for update until cleared or timeout
  for (UINT32 i = 0; i < CLOCK_RCG2_UPDATE_TIMEOUT_US; i++) {
    if (!(CrMmioRead32(
              TargetClockNode->ParentController->Address +
              TargetClockNode->CmdRegister +
              CLOCK_NODE_RCG_CMD_REGISTER_CMD_REG_OFFSET) &
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_UPDATE_MSK)) {
      return CR_SUCCESS;
    }
    MicroSecondDelay(1);
  }

  log_err("Timeout waiting for RCG2 clock " CR_LOG_CHAR8_STR_FMT " update", TargetClockNode->Name);
  return CR_TIMEOUT;
}

CR_STATUS
ClockRcg2SetRate(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN UINT64 RateHz, IN UINT8 Policy)
{
  ClockRcgFreqTable *TargetFreq = NULL;
  if (ClockContext == NULL || TargetClockNode == NULL ||
      TargetClockNode->Type != CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) {
    log_err("Invalid ClockNode provided for RCG2 SetRate.");
    return CR_INVALID_PARAMETER;
  }

  // Default to fastest clock in table
  if (TargetClockNode->FreqTable == NULL) {
    log_err("Frequency table is NULL for clock " CR_LOG_CHAR8_STR_FMT, TargetClockNode->Name);
    return CR_NOT_FOUND;
  }

  // Notice: The elements in FreqTable must be sorted by FrequencyHz in
  // ascending order
  for (UINT16 i = 0; i < TargetClockNode->FreqCount; i++) {
    ClockRcgFreqTable *FreqEntry = &TargetClockNode->FreqTable[i];
    // Exact match
    if (FreqEntry->FrequencyHz == RateHz) {
      TargetFreq = FreqEntry;
      break;
    }
    else if (FreqEntry->FrequencyHz >= RateHz) {
      // Default use Ceil policy
      TargetFreq = FreqEntry;
      // Switch to Floor policy if needed
      if (Policy == CLOCK_RCG2_POLICY_FLOOR) {
        if (i > 0)
          TargetFreq = &TargetClockNode->FreqTable[i - 1];
        else
          TargetFreq = NULL; // No suitable frequency found
      }
      break;
    }
  }
  if (TargetFreq == NULL && (Policy == CLOCK_RCG2_POLICY_FLOOR) &&
      (TargetClockNode->FreqCount > 0))
    TargetFreq = &TargetClockNode->FreqTable[TargetClockNode->FreqCount - 1];

  if (TargetFreq == NULL) {
    log_err(
        "No suitable frequency found for requested rate %lu Hz for clock " CR_LOG_CHAR8_STR_FMT,
        RateHz, TargetClockNode->Name);
    return CR_NOT_FOUND;
  }

  // Config RCG2
  UINT32 RegCfgVal = CrMmioRead32(
      TargetClockNode->ParentController->Address +
      TargetClockNode->CmdRegister + TargetClockNode->CmdRCfgOffset +
      CLOCK_NODE_RCG_CMD_REGISTER_CFG_REG_OFFSET);

  // Set parent source
  RegCfgVal = CLR_BITS(RegCfgVal, CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK);
  RegCfgVal |= SET_FIELD(
      TargetFreq->Source.Config, CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK);

  // Set MND values
  UINT32 MndMask = 0;
  if (TargetClockNode->MNDWidth && TargetFreq->N) {
    MndMask = BIT(TargetClockNode->MNDWidth) - 1;
    CrMmioUpdateBits32(
        TargetClockNode->ParentController->Address +
            TargetClockNode->CmdRegister + TargetClockNode->CmdRCfgOffset +
            CLOCK_NODE_RCG_CMD_REGISTER_M_REG_OFFSET,
        MndMask, TargetFreq->M);
    CrMmioUpdateBits32(
        TargetClockNode->ParentController->Address +
            TargetClockNode->CmdRegister + TargetClockNode->CmdRCfgOffset +
            CLOCK_NODE_RCG_CMD_REGISTER_N_REG_OFFSET,
        MndMask, ~(TargetFreq->N - TargetFreq->M));

    UINT32 D2Val   = TargetFreq->N;
    UINT32 N2subM2 = 2 * (TargetFreq->N - TargetFreq->M);
    // Clamp D2Val within [M, 2*(N - M)]
    D2Val =
        (D2Val < TargetFreq->M ? TargetFreq->M
                               : (D2Val > N2subM2 ? N2subM2 : D2Val));
    CrMmioUpdateBits32(
        TargetClockNode->ParentController->Address +
            TargetClockNode->CmdRegister + TargetClockNode->CmdRCfgOffset +
            CLOCK_NODE_RCG_CMD_REGISTER_D_REG_OFFSET,
        MndMask, (MndMask & (~D2Val)));
  }

  MndMask = (BIT(TargetClockNode->HIDWidth) - 1) |
            CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_MSK |
            CLOCK_NODE_RCG_CMD_REGISTER_CFG_HW_CLK_CTL_MSK;
  RegCfgVal = CLR_BITS(RegCfgVal, MndMask);
  RegCfgVal |= TargetFreq->PreDiv; // ignore zero shift for src div

  if (TargetClockNode->MNDWidth && TargetFreq->N &&
      (TargetFreq->N != TargetFreq->M)) {
    // Set Dual Edge mode
    RegCfgVal |= CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_DUAL_EDGE_MSK;
  }
  if (TargetClockNode->HwClockCtrl) {
    RegCfgVal |= CLOCK_NODE_RCG_CMD_REGISTER_CFG_HW_CLK_CTL_MSK;
  }
  // Write back to register
  CrMmioWrite32(
      TargetClockNode->ParentController->Address +
          TargetClockNode->CmdRegister + TargetClockNode->CmdRCfgOffset +
          CLOCK_NODE_RCG_CMD_REGISTER_CFG_REG_OFFSET,
      RegCfgVal);
  // Update RCG2 config
  return ClockRcg2UpdateConfig(ClockContext, TargetClockNode);
}

CR_STATUS
ClockEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN UINT64 RateHz, IN BOOLEAN Enable)
{
  CR_STATUS Status = CR_SUCCESS;

  if ((ClockContext == NULL) || (TargetClockNode == NULL) || (Enable > 1)) {
    log_err("Invalid Parameter provided.");
    return CR_INVALID_PARAMETER;
  }

  if ((TargetClockNode->EnableRegister == 0) &&
      TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH_2) {
    log_err("Clock " CR_LOG_CHAR8_STR_FMT " does not support enable/disable.", TargetClockNode->Name);
  }

  // Enable all parents first
  for (UINTN i = 0; i < TargetClockNode->ParentCount; i++) {
    ClockNode *ParentNode = TargetClockNode->Parents[i];
    if (ParentNode != NULL) {
      Status = ClockEnable(ClockContext, ParentNode, RateHz, Enable);
    }
    else
      log_info("Reach root clock node. " CR_LOG_CHAR8_STR_FMT, ParentNode->Name);
  }

  if (TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH_2) {
    // Enable target clock
    CrMmioUpdateBits32(
        TargetClockNode->ParentController->Address +
            TargetClockNode->EnableRegister,
        TargetClockNode->EnableMsk, Enable ? TargetClockNode->EnableMsk : 0);
    log_info("Clock " CR_LOG_CHAR8_STR_FMT " enabled.", TargetClockNode->Name);
    // Check halt flags and wait for clock to be enabled
    if ((TargetClockNode->HaltCheckFlag & CLOCK_NODE_BRANCH_HALT_BYPASS))
      return CR_SUCCESS;

    // Check if in hwcg(Hardware Clock Gating) mode
    if (TargetClockNode->HwcgRegister &&
        TO_BOOL(
            CrMmioRead32(
                TargetClockNode->ParentController->Address +
                TargetClockNode->HwcgRegister) &
            TargetClockNode->HwcgMsk)) {
      log_info(
          "Clock " CR_LOG_CHAR8_STR_FMT " is in hwcg mode, skip halt check.", TargetClockNode->Name);
      return CR_SUCCESS;
    }

    // Delay 10us when delay flag is set
    if ((TargetClockNode->HaltCheckFlag & CLOCK_NODE_BRANCH_HALT_DELAY) ||
        ((TargetClockNode->HaltCheckFlag & CLOCK_NODE_BRANCH_VOTED) &&
         !(Enable))) {
      cr_sleep(10);
      return CR_SUCCESS;
    }

    // If halt flag is set, poll until clock is enabled or timeout.
    // timeout is 200us in reference code
    if ((Enable &&
         (TargetClockNode->HaltCheckFlag & CLOCK_NODE_BRANCH_VOTED)) ||
        (TargetClockNode->HaltCheckFlag == !!TargetClockNode->HaltCheckFlag)) {

      UINT32  HaltRegVal     = 0;
      BOOLEAN OffBitInverted = (Enable ^ !!(TargetClockNode->HaltCheckFlag &
                                            CLOCK_NODE_BRANCH_HALT_ENABLE))
                                   ? CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK
                                   : 0;
      BOOLEAN IsOff          = FALSE;

      for (UINT32 i = 0; i < CLOCK_NODE_HALT_WAIT_TIMEOUT_US; i++) {
        // Check HALT status for branch clocks
        HaltRegVal = CrMmioRead32(
            TargetClockNode->ParentController->Address +
            TargetClockNode->HaltRegister);

        if (TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH_2) {
          IsOff =
              (OffBitInverted ==
               (HaltRegVal & CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK));

          if (IsOff) {
            log_info(
                "Clock " CR_LOG_CHAR8_STR_FMT " is now " CR_LOG_CHAR8_STR_FMT ".", TargetClockNode->Name,
                Enable ? "enabled" : "disabled");
            break;
          }

          // Enabling check
          if (Enable) {
            HaltRegVal &=
                (CLOCK_NODE_HALT_REG_CBCR_NOC_FSM_STATUS_MSK |
                 CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK);
            if (((GET_FIELD(
                     HaltRegVal,
                     CLOCK_NODE_HALT_REG_CBCR_NOC_FSM_STATUS_MSK)) ==
                 CLOCK_NODE_HALT_REG_CBCR_NOC_FSM_STATUS_ON)) {
              break;
            }
          }
        }
        else if (TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH) {
          OffBitInverted =
              TargetClockNode->HaltCheckFlag & CLOCK_NODE_BRANCH_HALT_ENABLE;

          IsOff = OffBitInverted ? !(HaltRegVal & TargetClockNode->HaltMsk)
                                 : !!(HaltRegVal & TargetClockNode->HaltMsk);
          if (IsOff == !Enable) {
            log_info(
                "Clock " CR_LOG_CHAR8_STR_FMT " is now " CR_LOG_CHAR8_STR_FMT ".", TargetClockNode->Name,
                Enable ? "enabled" : "disabled");
            break;
          }
        }
        else {
          log_err("Unsupported clock node type %u", TargetClockNode->Type);
          return CR_UNSUPPORTED;
        } /* Halt check for different Branch types*/
      } /* Timeout for loop */
    } /* Halt check block */
  }
  else if (TargetClockNode->Type == CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) {
    // For RCG2 clocks, enabling is done via SetRate function
    // ClockRcg2SetRate(
    //     ClockContext, TargetClockNode, RateHz, CLOCK_RCG2_POLICY_CEIL);
  }
  else {
    log_err("Unsupported clock node type %u", TargetClockNode->Type);
    return CR_UNSUPPORTED;
  }
  return Status;
}

CR_STATUS ClockLibInit(OUT ClockDriverContext **ClockContext)
{
  CR_STATUS Status;

  if (ClockContext == NULL)
    return CR_INVALID_PARAMETER;

  // Get Target Info
  *ClockContext = CrTargetGetClockContext();
  if (*ClockContext == NULL) {
    log_err("CrTargetGetClockContext failed.");
    return CR_NOT_FOUND;
  }

  // Map regions
  for (UINT8 i = 0; i < (*ClockContext)->ClockControllerCount; i++) {
    ClockController *Controller = &(*ClockContext)->ClockControllers[i];
    Status = MapDeviceIORegion(Controller->Address, Controller->Size);
    if (CR_ERROR(Status)) {
      log_warn(
          "Failed to map memory region for controller %u at 0x%lx", i,
          Controller->Address);
      continue;
    }
    Controller->MemMapped = TRUE;
  }

  // Call target clock init function
  Status = CrTargetClockInit(*ClockContext);
  if (CR_ERROR(Status)) {
    log_err("CrTargetClockInit failed: %r", Status);
  }

  return Status;
}

CR_STATUS
ClockDeinit(VOID)
{
  ClockDriverContext *ClockContext = CrTargetGetClockContext();
  CR_STATUS           Status       = CR_SUCCESS;

  if (ClockContext == NULL) {
    log_err("CrTargetGetClockContext failed.");
    return CR_NOT_FOUND;
  }

  // Unmap regions
  for (UINT8 i = 0; i < ClockContext->ClockControllerCount; i++) {
    ClockController *Controller = &ClockContext->ClockControllers[i];
    if (Controller->MemMapped) {
      Status = UnMapMemRegion(Controller->Address, Controller->Size);
      if (CR_ERROR(Status)) {
        log_warn(
            "Failed to unmap memory region for controller %u at 0x%lx", i,
            Controller->Address);
        continue;
      }
      Controller->MemMapped = FALSE;
    }
  }

  return Status;
}