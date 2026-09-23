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
      TargetClockNode->Type != CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 ||
      TargetClockNode->ParentController == NULL) {
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
  if (ClockContext == NULL || TargetClockNode == NULL ||
      (ClockName == NULL && (ControllerType == NULL || ClockId == NULL))) {
    log_err("Invalid Parameter provided.");
    return CR_INVALID_PARAMETER;
  }
  *TargetClockNode = NULL;

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
        if (ClkNode != NULL && ClkNode->Name != NULL &&
            cr_strcmp(ClkNode->Name, ClockName) == 0) {
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
GetGdscNode(
    IN ClockDriverContext *ClockContext, IN CONST CHAR8 *GdscName,
    OUT ClockNode **TargetGdscNode)
{
  UINTN ControllerIndex;
  UINTN GdscIndex;

  if (ClockContext == NULL || GdscName == NULL || TargetGdscNode == NULL) {
    return CR_INVALID_PARAMETER;
  }
  *TargetGdscNode = NULL;
  for (ControllerIndex = 0;
       ControllerIndex < ClockContext->ClockControllerCount;
       ControllerIndex++) {
    ClockController *Controller =
        &ClockContext->ClockControllers[ControllerIndex];
    for (GdscIndex = 0; GdscIndex < Controller->GdscCount; GdscIndex++) {
      ClockNode *Gdsc = Controller->Gdscs[GdscIndex];
      if (Gdsc != NULL && Gdsc->Name != NULL &&
          cr_strcmp(Gdsc->Name, GdscName) == 0) {
        *TargetGdscNode = Gdsc;
        return CR_SUCCESS;
      }
    }
  }
  return CR_NOT_FOUND;
}

CR_STATUS
ClockRcg2UpdateConfig(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode)
{
  if (ClockContext == NULL || TargetClockNode == NULL ||
      TargetClockNode->Type != CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 ||
      TargetClockNode->ParentController == NULL) {
    return CR_INVALID_PARAMETER;
  }

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
      TargetClockNode->Type != CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 ||
      TargetClockNode->ParentController == NULL) {
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

STATIC BOOLEAN
ClockControlIsEnabled(
    IN ClockNode *TargetClockNode)
{
  UINTN  Address;
  UINT32 Value;

  if ((TargetClockNode == NULL) ||
      (TargetClockNode->ParentController == NULL)) {
    return FALSE;
  }
  if (TargetClockNode->Type == CLOCK_NODE_TYPE_PHY_MUX) {
    Address = TargetClockNode->ParentController->Address +
              TargetClockNode->MuxRegister;
    Value = CrMmioRead32(Address) & TargetClockNode->MuxMask;
    return Value == (TargetClockNode->MuxPhyValue &
                     TargetClockNode->MuxMask);
  }
  if (TargetClockNode->Type == CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) {
    Address = TargetClockNode->ParentController->Address +
              TargetClockNode->CmdRegister +
              CLOCK_NODE_RCG_CMD_REGISTER_CMD_REG_OFFSET;
    return (CrMmioRead32(Address) &
            CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK) != 0;
  }
  if ((TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH) ||
      (TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH_2)) {
    Address = TargetClockNode->ParentController->Address +
              TargetClockNode->EnableRegister;
    return (CrMmioRead32(Address) & TargetClockNode->EnableMsk) != 0;
  }
  return FALSE;
}

STATIC CR_STATUS
ClockEnableInternal(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN UINT64 RateHz, IN BOOLEAN Enable)
{
  CR_STATUS Status;
  BOOLEAN   HardwareEnable;
  UINTN     Index;
  UINTN     ParentsAcquired;

  if ((ClockContext == NULL) || (TargetClockNode == NULL) || (Enable > 1)) {
    log_err("Invalid Parameter provided.");
    return CR_INVALID_PARAMETER;
  }
  if (Enable) {
    if (TargetClockNode->ReferenceCount == MAX_UINT32) {
      return CR_OUT_OF_RESOURCES;
    }
    if (TargetClockNode->ReferenceCount != 0) {
      if (TargetClockNode->ActiveRateHz != RateHz) {
        return CR_BUSY;
      }
      ++TargetClockNode->ReferenceCount;
      return CR_SUCCESS;
    }
    TargetClockNode->RestoreControlEnabled =
        ClockControlIsEnabled(TargetClockNode);
  } else {
    if (TargetClockNode->ReferenceCount == 0) {
      return CR_SUCCESS;
    }
    if (TargetClockNode->ReferenceCount > 1) {
      --TargetClockNode->ReferenceCount;
      return CR_SUCCESS;
    }
  }
  HardwareEnable = Enable || TargetClockNode->RestoreControlEnabled;
  ParentsAcquired = 0;

  if (TargetClockNode->Type == CLOCK_NODE_TYPE_PHY_MUX) {
    UINT32 Value;

    if (TargetClockNode->MuxRegister == 0 ||
        TargetClockNode->MuxMask == 0 ||
        TargetClockNode->ParentController == NULL ||
        TargetClockNode->ParentController->Address == 0) {
      return CR_INVALID_PARAMETER;
    }
    Value = HardwareEnable ? TargetClockNode->MuxPhyValue
                           : TargetClockNode->MuxRefValue;
    CrMmioUpdateBits32(
        TargetClockNode->ParentController->Address +
          TargetClockNode->MuxRegister,
        TargetClockNode->MuxMask,
        Value);
    MemoryFence();
    TargetClockNode->ReferenceCount = Enable ? 1U : 0U;
    TargetClockNode->ActiveRateHz = Enable ? RateHz : 0;
    if (!Enable) {
      TargetClockNode->RestoreControlEnabled = FALSE;
    }
    return CR_SUCCESS;
  }

  if (TargetClockNode->Type == CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) {
    UINTN CommandAddress;

    if (TargetClockNode->ParentController == NULL) {
      return CR_INVALID_PARAMETER;
    }
    CommandAddress = TargetClockNode->ParentController->Address +
                     TargetClockNode->CmdRegister +
                     CLOCK_NODE_RCG_CMD_REGISTER_CMD_REG_OFFSET;
    if (!Enable) {
      CrMmioUpdateBits32(
          CommandAddress, CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK,
          TargetClockNode->RestoreControlEnabled ?
            CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK : 0);
      MemoryFence();
      TargetClockNode->ReferenceCount = 0;
      TargetClockNode->ActiveRateHz = 0;
      TargetClockNode->RestoreControlEnabled = FALSE;
      return CR_SUCCESS;
    }
    if (HardwareEnable) {
      /* ROOT_OFF is a read-only status bit.  Linux forces the root on through
         ROOT_EN before changing its configuration. */
      CrMmioUpdateBits32(
          CommandAddress, CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK,
          CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK);
      for (Index = 0; Index < CLOCK_RCG2_UPDATE_TIMEOUT_US; ++Index) {
        if (ClockRcg2CheckEnable(ClockContext, TargetClockNode)) {
          break;
        }
        MicroSecondDelay(1);
      }
      if (Index == CLOCK_RCG2_UPDATE_TIMEOUT_US) {
        CrMmioUpdateBits32(
            CommandAddress, CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK,
            TargetClockNode->RestoreControlEnabled ?
              CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK : 0);
        return CR_TIMEOUT;
      }
      Status = ClockRcg2SetRate(
          ClockContext, TargetClockNode, RateHz,
          CLOCK_RCG2_POLICY_CEIL);
      if (CR_ERROR(Status)) {
        CrMmioUpdateBits32(
            CommandAddress, CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK,
            TargetClockNode->RestoreControlEnabled ?
              CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK : 0);
      } else {
        TargetClockNode->ReferenceCount = 1;
        TargetClockNode->ActiveRateHz = RateHz;
      }
      return Status;
    }
  }

  if ((TargetClockNode->Type != CLOCK_NODE_TYPE_BRANCH) &&
      (TargetClockNode->Type != CLOCK_NODE_TYPE_BRANCH_2)) {
    log_err("Unsupported clock node type %u", TargetClockNode->Type);
    return CR_UNSUPPORTED;
  }
  if ((TargetClockNode->EnableRegister == 0) ||
      (TargetClockNode->EnableMsk == 0) ||
      (TargetClockNode->ParentController == NULL)) {
    log_err("Clock " CR_LOG_CHAR8_STR_FMT " does not support enable/disable.", TargetClockNode->Name);
    return CR_UNSUPPORTED;
  }
  if ((TargetClockNode->ParentCount != 0) &&
      (TargetClockNode->Parents == NULL)) {
    return CR_INVALID_PARAMETER;
  }

  /* Cold enable follows the dependency graph from source to leaf. */
  if (Enable) {
    for (Index = 0; Index < TargetClockNode->ParentCount; ++Index) {
      if (TargetClockNode->Parents[Index] == NULL) {
        return CR_INVALID_PARAMETER;
      }
      Status = ClockEnableInternal(
          ClockContext, TargetClockNode->Parents[Index], RateHz, TRUE);
      if (CR_ERROR(Status)) {
        while (ParentsAcquired != 0) {
          --ParentsAcquired;
          (VOID)ClockEnableInternal(
              ClockContext, TargetClockNode->Parents[ParentsAcquired],
              RateHz, FALSE);
        }
        return Status;
      }
      ++ParentsAcquired;
    }
  }

  CrMmioUpdateBits32(
      TargetClockNode->ParentController->Address +
          TargetClockNode->EnableRegister,
      TargetClockNode->EnableMsk,
      HardwareEnable ? TargetClockNode->EnableMsk : 0);
  log_info(
      "Clock " CR_LOG_CHAR8_STR_FMT " " CR_LOG_CHAR8_STR_FMT ".",
      TargetClockNode->Name, HardwareEnable ? "enabled" : "disabled");

  {
    UINT8 HaltCheck;

    HaltCheck = TargetClockNode->HaltCheckFlag &
                (UINT8)~CLOCK_NODE_BRANCH_VOTED;
    if (HaltCheck == CLOCK_NODE_BRANCH_HALT_SKIP) {
      Status = CR_SUCCESS;
    } else if ((TargetClockNode->HwcgRegister != 0) &&
               ((CrMmioRead32(
                     TargetClockNode->ParentController->Address +
                     TargetClockNode->HwcgRegister) &
                 TargetClockNode->HwcgMsk) != 0)) {
      Status = CR_SUCCESS;
    } else if ((HaltCheck == CLOCK_NODE_BRANCH_HALT_DELAY) ||
               (!HardwareEnable && ((TargetClockNode->HaltCheckFlag &
                                     CLOCK_NODE_BRANCH_VOTED) != 0))) {
      cr_sleep(10);
      Status = CR_SUCCESS;
    } else if ((HaltCheck == CLOCK_NODE_BRANCH_HALT) ||
               (HaltCheck == CLOCK_NODE_BRANCH_HALT_ENABLE) ||
               (HaltCheck == CLOCK_NODE_BRANCH_HALT_POLL)) {
      Status = CR_TIMEOUT;
      for (Index = 0; Index < CLOCK_NODE_HALT_WAIT_TIMEOUT_US; ++Index) {
        UINT32  HaltRegVal;
        BOOLEAN Halted;

        HaltRegVal = CrMmioRead32(
            TargetClockNode->ParentController->Address +
            TargetClockNode->HaltRegister);
        if (TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH_2) {
          if (HardwareEnable &&
              (GET_FIELD(
                   HaltRegVal,
                   CLOCK_NODE_HALT_REG_CBCR_NOC_FSM_STATUS_MSK) ==
               CLOCK_NODE_HALT_REG_CBCR_NOC_FSM_STATUS_ON)) {
            Status = CR_SUCCESS;
            break;
          }
          Halted =
              ((HaltRegVal & CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK) != 0);
        } else {
          Halted = ((HaltRegVal & TargetClockNode->HaltMsk) != 0);
        }
        if (HaltCheck == CLOCK_NODE_BRANCH_HALT_ENABLE) {
          Halted = !Halted;
        }
        if (Halted == !HardwareEnable) {
          Status = CR_SUCCESS;
          break;
        }
        MicroSecondDelay(1);
      }
      if (CR_ERROR(Status)) {
        log_err(
            "Clock " CR_LOG_CHAR8_STR_FMT " status did not become " CR_LOG_CHAR8_STR_FMT,
            TargetClockNode->Name,
            HardwareEnable ? "enabled" : "disabled");
      }
    } else {
      Status = CR_UNSUPPORTED;
    }
  }

  if (CR_ERROR(Status)) {
    if (Enable) {
      CrMmioUpdateBits32(
          TargetClockNode->ParentController->Address +
              TargetClockNode->EnableRegister,
          TargetClockNode->EnableMsk,
          TargetClockNode->RestoreControlEnabled ?
            TargetClockNode->EnableMsk : 0);
      while (ParentsAcquired != 0) {
        --ParentsAcquired;
        (VOID)ClockEnableInternal(
            ClockContext, TargetClockNode->Parents[ParentsAcquired],
            RateHz, FALSE);
      }
    }
    return Status;
  }

  if (!Enable) {
    /* A leaf must stop using its source before that source is disabled. */
    for (Index = TargetClockNode->ParentCount; Index != 0; --Index) {
      if (TargetClockNode->Parents[Index - 1] == NULL) {
        return CR_INVALID_PARAMETER;
      }
      Status = ClockEnableInternal(
          ClockContext, TargetClockNode->Parents[Index - 1], RateHz, FALSE);
      if (CR_ERROR(Status)) {
        return Status;
      }
    }
    TargetClockNode->ReferenceCount = 0;
    TargetClockNode->RestoreControlEnabled = FALSE;
    TargetClockNode->ActiveRateHz = 0;
  } else {
    TargetClockNode->ReferenceCount = 1;
    TargetClockNode->ActiveRateHz = RateHz;
  }
  return CR_SUCCESS;
}

CR_STATUS
ClockEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN UINT64 RateHz, IN BOOLEAN Enable)
{
  CR_STATUS Status;

  if ((ClockContext == NULL) || !ClockContext->LockInitialized) {
    return CR_INVALID_PARAMETER;
  }
  CrLockAcquire(&ClockContext->Lock);
  Status = ClockEnableInternal(
      ClockContext, TargetClockNode, RateHz, Enable);
  CrLockRelease(&ClockContext->Lock);
  return Status;
}

CR_STATUS ClockLibInit(IN OUT ClockDriverContext **ClockContext)
{
  CR_STATUS Status = CR_SUCCESS;

  if (ClockContext == NULL || *ClockContext == NULL)
    return CR_INVALID_PARAMETER;

  if (!(*ClockContext)->LockInitialized) {
    CrLockInit(&(*ClockContext)->Lock);
    (*ClockContext)->LockInitialized = TRUE;
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

  /* Existing GCD mappings can reject AddMemorySpace.  Preserve the target's
   * prior behavior: the clock owner performs its platform initialization
   * after this best-effort mapping pass. */
  return CR_SUCCESS;
}

CR_STATUS
ClockDeinit(IN OUT ClockDriverContext *ClockContext)
{
  CR_STATUS Status = CR_SUCCESS;

  if (ClockContext == NULL) {
    return CR_INVALID_PARAMETER;
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
