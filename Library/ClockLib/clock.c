/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include "clock_internal.h"
#ifndef _KERNEL_MODE
#include <oskal/cr_memmap.h>
#endif

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
  if (ClockContext->ClockControllers == NULL ||
      ClockContext->ClockControllerCount == 0 ||
      ClockContext->ClockControllerCount > CC_TYPE_MAX) {
    return CR_INVALID_PARAMETER;
  }

  // Locate by ControllerType and ClockId if ClockName is not provided
  if (!ClockName) {
    if ((UINT32)*ControllerType >= ClockContext->ClockControllerCount ||
        ClockContext->ClockControllers[*ControllerType].Clks == NULL ||
        *ClockId >= ClockContext->ClockControllers[*ControllerType].ClkCount) {
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
      if (Controller->ClkCount != 0 && Controller->Clks == NULL) {
        return CR_INVALID_PARAMETER;
      }
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
  if (ClockContext->ClockControllers == NULL ||
      ClockContext->ClockControllerCount == 0 ||
      ClockContext->ClockControllerCount > CC_TYPE_MAX) {
    return CR_INVALID_PARAMETER;
  }
  for (ControllerIndex = 0;
       ControllerIndex < ClockContext->ClockControllerCount;
       ControllerIndex++) {
    ClockController *Controller =
        &ClockContext->ClockControllers[ControllerIndex];
    if (Controller->GdscCount != 0 && Controller->Gdscs == NULL) {
      return CR_INVALID_PARAMETER;
    }
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
    cr_sleep(1);
  }

  log_err(
      "Timeout waiting for RCG2 clock " CR_LOG_CHAR8_STR_FMT
      " update cmd=0x%08x cfg=0x%08x",
      TargetClockNode->Name,
      CrMmioRead32(TargetClockNode->ParentController->Address +
                   TargetClockNode->CmdRegister +
                   CLOCK_NODE_RCG_CMD_REGISTER_CMD_REG_OFFSET),
      CrMmioRead32(TargetClockNode->ParentController->Address +
                   TargetClockNode->CmdRegister +
                   TargetClockNode->CmdRCfgOffset +
                   CLOCK_NODE_RCG_CMD_REGISTER_CFG_REG_OFFSET));
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
      TargetClockNode->ParentController == NULL ||
      TargetClockNode->HIDWidth<1 || TargetClockNode->HIDWidth>8 ||
      TargetClockNode->MNDWidth>16) {
    log_err("Invalid ClockNode provided for RCG2 SetRate.");
    return CR_INVALID_PARAMETER;
  }

  if (TargetClockNode->FixedRateHz && RateHz != TargetClockNode->FixedRateHz)
    return CR_UNSUPPORTED;

  if (TargetClockNode->RcgRestorePending) return CR_NOT_READY;

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

  /* Some Qualcomm RCG parents are analogue PLLs that are not represented by
   * a generic ClockNode.  Prepare that parent before writing the RCG config;
   * otherwise the hardware update handshake can remain asserted forever when
   * the selected source is stopped. */
  if (ClockContext->PrepareSource != NULL) {
    CR_STATUS SourceStatus = ClockContext->PrepareSource (
        ClockContext, TargetClockNode, TargetFreq);
    if (CR_ERROR (SourceStatus)) {
      log_err("Clock source preparation failed for " CR_LOG_CHAR8_STR_FMT,
              TargetClockNode->Name);
      return SourceStatus;
    }
  }

  UINTN ConfigAddress=TargetClockNode->ParentController->Address+
    TargetClockNode->CmdRegister+TargetClockNode->CmdRCfgOffset+
    CLOCK_NODE_RCG_CMD_REGISTER_CFG_REG_OFFSET;
  UINT32 ConfigMask=(UINT32)(BIT(TargetClockNode->HIDWidth)-1U) |
    CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK |
    CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_MSK |
    CLOCK_NODE_RCG_CMD_REGISTER_CFG_HW_CLK_CTL_MSK;
  UINT32 OperandMask=TargetClockNode->MNDWidth && TargetFreq->N ?
    (UINT32)BIT(TargetClockNode->MNDWidth)-1U : 0;
  UINT32 Expected[4]={0};
  for (UINTN I=0;I<4;++I) {
    TargetClockNode->RcgSavedMask[I]=I==0?ConfigMask:OperandMask;
    TargetClockNode->RcgSavedValue[I]=TargetClockNode->RcgSavedMask[I] ?
      CrMmioRead32(ConfigAddress+4U*I)&TargetClockNode->RcgSavedMask[I] : 0;
  }
  TargetClockNode->RcgRestorePending=TRUE;
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
    Expected[1]=TargetFreq->M&MndMask;
    Expected[2]=~(TargetFreq->N-TargetFreq->M)&MndMask;
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
    Expected[3]=~D2Val&MndMask;
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
  CrMemoryFence();
  Expected[0]=RegCfgVal&ConfigMask;
  CR_STATUS Status=ClockRcg2UpdateConfig(ClockContext,TargetClockNode);
  if (!CR_ERROR(Status)) for (UINTN I=0;I<4;++I) {
    if (TargetClockNode->RcgSavedMask[I] &&
        (CrMmioRead32(ConfigAddress+4U*I)&TargetClockNode->RcgSavedMask[I])!=Expected[I]) {
      Status=CR_DEVICE_ERROR;break;
    }
  }
  if (!CR_ERROR(Status)) {
    TargetClockNode->RcgRestorePending=FALSE;
    return CR_SUCCESS;
  }
  CR_STATUS Cleanup=ClockRestoreRcgSnapshotLocked(ClockContext,TargetClockNode);
  if (CR_ERROR(Cleanup)) {
    TargetClockNode->ReleasePending=TRUE;
    if (!TargetClockNode->ReferenceCount) TargetClockNode->ReferenceCount=1;
  }
  return Status;
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
  if (TargetClockNode->Type == CLOCK_NODE_TYPE_MUX) {
    /* A clk_regmap_mux selects a parent but has no enable/disable operation;
       its child branch owns the actual gate. */
    return TRUE;
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

STATIC CR_STATUS ClockReadOnlyDividerDescription(ClockNode *Node)
{
  ClockController *C=Node->ParentController;
  if (!C || !C->MemMapped || !C->Address || (C->Address&3U) || C->Size<4 ||
      C->Address>(UINTN)-1-C->Size || (Node->DividerRegister&3U) ||
      Node->DividerRegister>C->Size-4U || !Node->DividerWidth ||
      Node->DividerWidth>16 || Node->DividerReadOnly!=TRUE ||
      !Node->DividerInputRateHz) return CR_INVALID_PARAMETER;
  if (Node->RequiredDivider && Node->RequiredDivider>(UINT32)BIT(Node->DividerWidth))
    return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}

STATIC CR_STATUS ClockReadOnlyDividerCheck(ClockNode *Node,UINT64 RateHz,
                                         BOOLEAN Snapshot)
{
  CR_STATUS S=ClockReadOnlyDividerDescription(Node);
  if (CR_ERROR(S)) return S;
  UINT32 Divider=(CrMmioRead32(Node->ParentController->Address+Node->DividerRegister)&
    ((UINT32)BIT(Node->DividerWidth)-1U))+1U;
  UINT64 Rate=Node->DividerInputRateHz/Divider+
    (Node->DividerInputRateHz%Divider!=0);
  if (Rate!=RateHz || (Node->RequiredDivider && Divider!=Node->RequiredDivider))
    return CR_UNSUPPORTED;
  if (Snapshot) Node->ActiveDivider=Divider;
  else if (Node->ActiveDivider!=Divider) return CR_NOT_READY;
  return CR_SUCCESS;
}

/* Shared acquisition never reprograms the graph. Requalify fixed PLLs and
 * every observed RO divider, including its exact divisor snapshot. Ordinary
 * branch-only graphs retain their existing no-MMIO shared behavior. */
STATIC CR_STATUS
ClockCheckFixedParents(ClockNode *Node,UINT64 RateHz,UINT32 Depth,UINT32 *Budget)
{
  if (Node == NULL || Depth == 32 || *Budget == 0 ||
      CR_ERROR(ClockParentDescription(Node)))
    return CR_INVALID_PARAMETER;
  --*Budget;
  if (Node->ReleasePending) return CR_NOT_READY;
  if (Node->FixedRateHz && Node->FixedRateHz!=RateHz) return CR_UNSUPPORTED;
  if (Node->Type == CLOCK_NODE_TYPE_FIXED_PLL) {
    if (RateHz!=Node->FixedPll.Description.FrequencyHz) return CR_UNSUPPORTED;
    return CrFixedPllCheck(&Node->FixedPll);
  }
  if (Node->Type == CLOCK_NODE_TYPE_READ_ONLY_DIVIDER) {
    CR_STATUS Status=ClockReadOnlyDividerCheck(Node,RateHz,FALSE);
    if (CR_ERROR(Status)) return Status;
  }
  for (UINTN I=0;I<Node->ParentCount;++I) {
    if (ClockHasConditionalParents(Node) &&
        (Node->ParentsHeld!=Node->ParentCount ||
         ((Node->ParentHeldMask&(UINT32)BIT(I))!=0)!=ClockNeedsParent(Node,I,RateHz)))
      return CR_COMPROMISED_DATA;
    if (!ClockNeedsParent(Node,I,RateHz)) continue;
    CR_STATUS Status=ClockCheckFixedParents(Node->Parents[I],
      ClockParentRate(Node,I,RateHz),Depth+1,Budget);
    if (CR_ERROR(Status)) return Status;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS
ClockEnableInternal(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN UINT64 RateHz, IN BOOLEAN Enable)
{
  CR_STATUS Status;
  BOOLEAN   HardwareEnable;
  UINTN     Index;

  if ((ClockContext == NULL) || (TargetClockNode == NULL) || (Enable > 1)) {
    log_err("Invalid Parameter provided.");
    return CR_INVALID_PARAMETER;
  }
  if (Enable && TargetClockNode->FixedRateHz && RateHz!=TargetClockNode->FixedRateHz)
    return CR_UNSUPPORTED;
  if (Enable && (TargetClockNode->KeepEnabled>1 || (TargetClockNode->KeepEnabled &&
      TargetClockNode->Type!=CLOCK_NODE_TYPE_BRANCH &&
      TargetClockNode->Type!=CLOCK_NODE_TYPE_BRANCH_2))) return CR_INVALID_PARAMETER;
  Status=ClockParentDescription(TargetClockNode);
  if (CR_ERROR(Status)) return Status;
  if (Enable && TargetClockNode->Type==CLOCK_NODE_TYPE_READ_ONLY_DIVIDER) {
    Status=ClockReadOnlyDividerDescription(TargetClockNode);
    if (CR_ERROR(Status)) return Status;
  }
  if (TargetClockNode->Type == CLOCK_NODE_TYPE_FIXED_PLL) {
    CR_FIXED_PLL_VOTE *P = &TargetClockNode->FixedPll;
    if (TargetClockNode->ParentController == NULL ||
        (Enable && RateHz != P->Description.FrequencyHz)) return CR_INVALID_PARAMETER;
    if (!P->Initialized) {
      CR_FIXED_PLL_DESCRIPTION D = P->Description;
      D.Base = TargetClockNode->ParentController->Address;
      D.Size = TargetClockNode->ParentController->Size;
      Status = CrFixedPllInit(P, &D);
      if (CR_ERROR(Status)) return Status;
    }
    if (Enable) {
      if (TargetClockNode->ReleasePending) return CR_NOT_READY;
      if (TargetClockNode->ReferenceCount == CR_MAX_UINT32) return CR_OUT_OF_RESOURCES;
      if (TargetClockNode->ReferenceCount != 0) {
        Status = CrFixedPllCheck(P);
        if (!CR_ERROR(Status)) ++TargetClockNode->ReferenceCount;
        return Status;
      }
      Status = CrFixedPllAcquire(P);
      TargetClockNode->ReferenceCount = P->Owned ? 1U : 0U;
      TargetClockNode->ActiveRateHz = P->Owned ? RateHz : 0;
      TargetClockNode->ReleasePending = CR_ERROR(Status) && P->Owned;
      return Status;
    }
    if (TargetClockNode->ReferenceCount > 1) {
      --TargetClockNode->ReferenceCount;
      return CR_SUCCESS;
    }
    TargetClockNode->ReleasePending = P->Owned;
    Status = CrFixedPllRelease(P);
    if (!CR_ERROR(Status)) {
      TargetClockNode->ReferenceCount = 0;
      TargetClockNode->ActiveRateHz = 0;
      TargetClockNode->ReleasePending = FALSE;
    }
    return Status;
  }
  if (TargetClockNode->Type==CLOCK_NODE_TYPE_MUX && TargetClockNode->MuxRestorePending) {
    if (Enable) return CR_NOT_READY;
    Status=ClockRestoreMuxSnapshotLocked(ClockContext,TargetClockNode);
    if (CR_ERROR(Status)) return Status;
    TargetClockNode->ReleasePending=FALSE;
  }
  if (TargetClockNode->Type==CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 &&
      TargetClockNode->ReleasePending) {
    if (Enable && (TargetClockNode->RcgRestorePending ||
                   TargetClockNode->ReferenceCount)) return CR_NOT_READY;
    Status=ClockRecoverConfigurationLocked(ClockContext,TargetClockNode);
    if (CR_ERROR(Status)) return Status;
  }
  if (Enable && TargetClockNode->ReleasePending &&
      ((TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH) ||
       (TargetClockNode->Type == CLOCK_NODE_TYPE_BRANCH_2) ||
       (TargetClockNode->Type == CLOCK_NODE_TYPE_READ_ONLY_DIVIDER))) {
    return CR_NOT_READY;
  }
  if (Enable) {
    if (TargetClockNode->ReferenceCount == CR_MAX_UINT32) {
      return CR_OUT_OF_RESOURCES;
    }
    if (TargetClockNode->ReferenceCount != 0) {
      if (TargetClockNode->ActiveRateHz != RateHz) {
        return CR_BUSY;
      }
      UINT32 Budget=512;
      Status=ClockCheckFixedParents(TargetClockNode,RateHz,0,&Budget);
      if (CR_ERROR(Status)) return Status;
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
    CrMemoryFence();
    TargetClockNode->ReferenceCount = Enable ? 1U : 0U;
    TargetClockNode->ActiveRateHz = Enable ? RateHz : 0;
    if (!Enable) {
      TargetClockNode->RestoreControlEnabled = FALSE;
    }
    return CR_SUCCESS;
  }

  if (TargetClockNode->Type == CLOCK_NODE_TYPE_MUX) {
    /* Linux's clk_regmap_mux has no gate callbacks.  Keep the software vote
       so a child branch can order its dependency, but preserve the selector
       chosen by SelectExternalSource across enable/disable. */
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
      CrMemoryFence();
      if ((CrMmioRead32(CommandAddress)&CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK)!=
          (TargetClockNode->RestoreControlEnabled?CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK:0)) {
        TargetClockNode->ReferenceCount=1;
        TargetClockNode->ReleasePending=TRUE;
        return CR_DEVICE_ERROR;
      }
      TargetClockNode->ReferenceCount = 0;
      TargetClockNode->ActiveRateHz = 0;
      TargetClockNode->RestoreControlEnabled = FALSE;
      TargetClockNode->ExternalConfigPending = FALSE;
      TargetClockNode->ReleasePending=FALSE;
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
        cr_sleep(1);
      }
      if (Index == CLOCK_RCG2_UPDATE_TIMEOUT_US) {
        CrMmioUpdateBits32(
            CommandAddress, CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK,
            TargetClockNode->RestoreControlEnabled ?
              CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK : 0);
        return CR_TIMEOUT;
      }
      if (TargetClockNode->ExternalConfigPending) {
        /* ConfigureExternalSource intentionally runs while the leaf branch
         * is gated.  Its source/divider values are already latched by the
         * RCG update command; selecting the table's default rate here would
         * silently undo the external DSI PHY parent. */
        Status = CR_SUCCESS;
        TargetClockNode->ExternalConfigPending = FALSE;
      } else {
        Status = ClockRcg2SetRate(
            ClockContext, TargetClockNode, RateHz,
            CLOCK_RCG2_POLICY_CEIL);
      }
      if (CR_ERROR(Status)) {
        if (!TargetClockNode->RcgRestorePending) CrMmioUpdateBits32(
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
      (TargetClockNode->Type != CLOCK_NODE_TYPE_BRANCH_2) &&
      (TargetClockNode->Type != CLOCK_NODE_TYPE_READ_ONLY_DIVIDER)) {
    log_err("Unsupported clock node type %u", TargetClockNode->Type);
    return CR_UNSUPPORTED;
  }
  if (TargetClockNode->Type != CLOCK_NODE_TYPE_READ_ONLY_DIVIDER &&
      ((TargetClockNode->EnableRegister == 0) ||
      (TargetClockNode->EnableMsk == 0) ||
      (TargetClockNode->ParentController == NULL))) {
    log_err("Clock " CR_LOG_CHAR8_STR_FMT " does not support enable/disable.", TargetClockNode->Name);
    return CR_UNSUPPORTED;
  }
  if ((TargetClockNode->ParentCount != 0) &&
      (TargetClockNode->Parents == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  if (TargetClockNode->ParentsHeld > TargetClockNode->ParentCount) {
    return CR_COMPROMISED_DATA;
  }
  if (ClockHasConditionalParents(TargetClockNode) &&
      (TargetClockNode->ParentHeldMask &
       ~(TargetClockNode->ParentsHeld==32 ? CR_MAX_UINT32 :
         (UINT32)BIT(TargetClockNode->ParentsHeld)-1U)))
    return CR_COMPROMISED_DATA;
  for (Index = 0; Index < TargetClockNode->ParentCount; ++Index) {
    if (TargetClockNode->Parents[Index] == NULL) {
      return CR_INVALID_PARAMETER;
    }
  }

  /* Cold enable follows the dependency graph from source to leaf. */
  if (Enable) {
    if ((TargetClockNode->ParentsHeld != 0) || TargetClockNode->GateHeld ||
        (ClockHasConditionalParents(TargetClockNode) && TargetClockNode->ParentHeldMask)) {
      return CR_COMPROMISED_DATA;
    }
    // Retain a cleanup owner before acquiring dependencies. A failed enable
    // can itself have a failed undo; that owner must remain discoverable.
    TargetClockNode->ReferenceCount = 1;
    TargetClockNode->ActiveRateHz = RateHz;
    for (Index = 0; Index < TargetClockNode->ParentCount; ++Index) {
      if (!ClockNeedsParent(TargetClockNode,Index,RateHz)) {
        ++TargetClockNode->ParentsHeld;
        continue;
      }
      UINT32 Before = TargetClockNode->Parents[Index]->ReferenceCount;
      Status = ClockEnableInternal(
          ClockContext, TargetClockNode->Parents[Index],
          ClockParentRate(TargetClockNode,Index,RateHz),
          TRUE);
      // A failed child acquire may retain its own cleanup reference. Include
      // that reference in this prefix so the failed ancestor can retire it.
      if (!CR_ERROR(Status) ||
          (TargetClockNode->Parents[Index]->ReferenceCount > Before)) {
        ++TargetClockNode->ParentsHeld;
        if (ClockHasConditionalParents(TargetClockNode))
          TargetClockNode->ParentHeldMask|=(UINT32)BIT(Index);
      }
      if (CR_ERROR(Status)) {
        CR_STATUS Cleanup = ClockEnableInternal(
            ClockContext, TargetClockNode, RateHz, FALSE);
        if (CR_ERROR(Cleanup)) {
          log_err("Clock " CR_LOG_CHAR8_STR_FMT " parent rollback remains pending",
                  TargetClockNode->Name);
        }
        return Status;
      }
    }
    TargetClockNode->GateHeld = TRUE;
  } else {
    TargetClockNode->ReleasePending = TRUE;
    // Once the gate was restored, a later parent failure must not make retry
    // poll an off/reset leaf again or reacquire any withdrawn dependency.
    if (!TargetClockNode->GateHeld) {
      goto ReleaseParents;
    }
  }

  if (TargetClockNode->Type == CLOCK_NODE_TYPE_READ_ONLY_DIVIDER) {
    Status=Enable ? ClockReadOnlyDividerCheck(TargetClockNode,RateHz,TRUE) : CR_SUCCESS;
    goto CheckResult;
  }
  if (!Enable && TargetClockNode->KeepEnabled) goto ReleaseParents;

  CrMmioUpdateBits32(
      TargetClockNode->ParentController->Address +
          TargetClockNode->EnableRegister,
      TargetClockNode->EnableMsk,
      HardwareEnable ? TargetClockNode->EnableMsk : 0);
  log_info(
      "Clock " CR_LOG_CHAR8_STR_FMT " " CR_LOG_CHAR8_STR_FMT ".",
      TargetClockNode->Name,
      (!Enable && TargetClockNode->RestoreControlEnabled) ?
        "restored inherited enable" : (HardwareEnable ? "enabled" : "disabled"));

  {
    UINT8 HaltCheck;
    UINT32 LastHaltRegVal;

    HaltCheck = (UINT8)(TargetClockNode->HaltCheckFlag &
                        ~CLOCK_NODE_BRANCH_VOTED);
    LastHaltRegVal = 0;
    if (!Enable && TargetClockNode->RestoreControlEnabled) {
      /* Withdrawing the last native reference restores the firmware's
         gate request, not its former running state. A core/PHY reset may
         legitimately leave CLK_OFF set with that request still latched.
         Check the restored control bits; requiring CLK_OFF to clear would
         retain every reference forever after USB/DP unplug. */
      UINT32 Control = CrMmioRead32(
          TargetClockNode->ParentController->Address +
          TargetClockNode->EnableRegister);
      if ((Control & TargetClockNode->EnableMsk) != TargetClockNode->EnableMsk) {
        log_err("Clock " CR_LOG_CHAR8_STR_FMT " inherited enable did not restore",
                TargetClockNode->Name);
        Status = CR_DEVICE_ERROR;
      } else {
        if (HaltCheck == CLOCK_NODE_BRANCH_HALT_DELAY ||
            (TargetClockNode->HaltCheckFlag & CLOCK_NODE_BRANCH_VOTED) != 0)
          cr_sleep(10);
        Status = CR_SUCCESS;
      }
    } else if (HaltCheck == CLOCK_NODE_BRANCH_HALT_SKIP) {
      Status = CR_SUCCESS;
    } else if ((TargetClockNode->HwcgRegister != 0) &&
               ((CrMmioRead32(
                     TargetClockNode->ParentController->Address +
                     TargetClockNode->HwcgRegister) &
                 TargetClockNode->HwcgMsk) != 0)) {
      Status = CR_SUCCESS;
    } else if ((HaltCheck == CLOCK_NODE_BRANCH_HALT_DELAY) ||
               (!Enable && ((TargetClockNode->HaltCheckFlag &
                                     CLOCK_NODE_BRANCH_VOTED) != 0))) {
      cr_sleep(10);
      Status = CR_SUCCESS;
    } else if ((HaltCheck == CLOCK_NODE_BRANCH_HALT) ||
               (HaltCheck == CLOCK_NODE_BRANCH_HALT_ENABLE) ||
               (Enable && ((TargetClockNode->HaltCheckFlag &
                                    CLOCK_NODE_BRANCH_VOTED) != 0)) ||
               (HaltCheck == CLOCK_NODE_BRANCH_HALT_POLL)) {
      Status = CR_TIMEOUT;
      for (Index = 0; Index < CLOCK_NODE_HALT_WAIT_TIMEOUT_US; ++Index) {
        UINT32  HaltRegVal;
        BOOLEAN Halted;

        HaltRegVal = CrMmioRead32(
            TargetClockNode->ParentController->Address +
            TargetClockNode->HaltRegister);
        LastHaltRegVal = HaltRegVal;
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
        cr_sleep(1);
      }
      if (CR_ERROR(Status)) {
        UINT32  FinalHaltReg;
        UINT32  FinalEnableReg;
        UINT32  ParentMuxReg;
        UINT32  ParentMuxVal;

        /* Preserve the raw hardware state when a BRANCH_2 status poll fails.
         * This is especially useful for PHY supplied clocks: the source is a
         * separate mux register (for example GCC 0x9d080), while the branch
         * status and vote are at 0x9d038 and 0x62000 respectively.  Reading
         * these values only on timeout keeps normal boot logs unchanged and
         * lets the next hardware run distinguish a rejected selector write
         * from a CBCR status encoding or PHY readiness problem. */
        /* The final poll sample is the current CBCR state.  Avoid a second
         * status read here: some CBCR implementations have read side effects,
         * and the timeout path already has a complete sample. */
        FinalHaltReg = LastHaltRegVal;
        FinalEnableReg = CrMmioRead32(
            TargetClockNode->ParentController->Address +
            TargetClockNode->EnableRegister);
        ParentMuxReg = 0;
        ParentMuxVal = 0;
        if ((TargetClockNode->ParentCount == 1) &&
            (TargetClockNode->Parents != NULL) &&
            (TargetClockNode->Parents[0] != NULL) &&
            ((TargetClockNode->Parents[0]->Type == CLOCK_NODE_TYPE_MUX) ||
             (TargetClockNode->Parents[0]->Type == CLOCK_NODE_TYPE_PHY_MUX)) &&
            (TargetClockNode->Parents[0]->ParentController != NULL)) {
          ParentMuxReg = TargetClockNode->Parents[0]->MuxRegister;
          ParentMuxVal = CrMmioRead32(
              TargetClockNode->Parents[0]->ParentController->Address +
              ParentMuxReg);
        }
        log_err(
            "Clock " CR_LOG_CHAR8_STR_FMT
            " timeout readback halt=0x%08x enable=0x%08x"
            " halt-reg=0x%llx enable-reg=0x%llx"
            " parent-mux-reg=0x%llx parent-mux=0x%08x"
            " cbcr-off=%u cbcr-fsm=0x%x",
            TargetClockNode->Name, FinalHaltReg, FinalEnableReg,
            (UINT64)(TargetClockNode->ParentController->Address +
                     TargetClockNode->HaltRegister),
            (UINT64)(TargetClockNode->ParentController->Address +
                     TargetClockNode->EnableRegister),
            (UINT64)((ParentMuxReg == 0) ? 0 :
                     (TargetClockNode->Parents[0]->ParentController->Address +
                      ParentMuxReg)),
            ParentMuxVal,
            (FinalHaltReg & CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK) != 0,
            GET_FIELD(FinalHaltReg,
                      CLOCK_NODE_HALT_REG_CBCR_NOC_FSM_STATUS_MSK));
        log_err(
            "Clock " CR_LOG_CHAR8_STR_FMT " status did not become " CR_LOG_CHAR8_STR_FMT,
            TargetClockNode->Name,
            HardwareEnable ? "enabled" : "disabled");
      }
    } else {
      Status = CR_UNSUPPORTED;
    }
  }

CheckResult:
  if (CR_ERROR(Status)) {
    if (Enable) {
      CR_STATUS Cleanup = ClockEnableInternal(
          ClockContext, TargetClockNode, RateHz, FALSE);
      if (CR_ERROR(Cleanup)) {
        log_err("Clock " CR_LOG_CHAR8_STR_FMT " enable rollback remains pending",
                TargetClockNode->Name);
      }
    }
    return Status;
  }

ReleaseParents:
  if (!Enable) {
    TargetClockNode->GateHeld = FALSE;
    /* A leaf must stop using its source before that source is disabled. */
    while (TargetClockNode->ParentsHeld != 0) {
      Index = TargetClockNode->ParentsHeld;
      if (ClockHasConditionalParents(TargetClockNode) &&
          !(TargetClockNode->ParentHeldMask&(UINT32)BIT(Index-1))) {
        --TargetClockNode->ParentsHeld;
        continue;
      }
      Status = ClockEnableInternal(
          ClockContext, TargetClockNode->Parents[Index - 1],
          ClockParentRate(TargetClockNode,Index-1,TargetClockNode->ActiveRateHz),
          FALSE);
      if (CR_ERROR(Status)) {
        return Status;
      }
      if (ClockHasConditionalParents(TargetClockNode))
        TargetClockNode->ParentHeldMask&=~(UINT32)BIT(Index-1);
      --TargetClockNode->ParentsHeld;
    }
    TargetClockNode->ReferenceCount = 0;
    TargetClockNode->RestoreControlEnabled = FALSE;
    TargetClockNode->ActiveRateHz = 0;
    TargetClockNode->ActiveDivider = 0;
    TargetClockNode->ReleasePending = FALSE;
  } else {
    TargetClockNode->ReferenceCount = 1;
    TargetClockNode->ActiveRateHz = RateHz;
  }
  return CR_SUCCESS;
}

CR_STATUS
ClockCheckAcquired(ClockDriverContext *Context,ClockNode *Node,UINT64 RateHz)
{
  if (!Context || !Context->LockInitialized || !Node) return CR_INVALID_PARAMETER;
  CrLockAcquire(&Context->Lock);
  UINT32 Budget=512;
  CR_STATUS Status=!Node->ReferenceCount || Node->ReleasePending ? CR_NOT_READY :
    Node->ActiveRateHz!=RateHz ? CR_BUSY : ClockCheckFixedParents(Node,RateHz,0,&Budget);
  CrLockRelease(&Context->Lock);
  return Status;
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

CR_STATUS ClockAcquire (ClockDriverContext *Context, ClockNode *Node,
                        UINT64 RateHz, BOOLEAN *Owned)
{
  CR_STATUS Status;
  UINT32 Before;
  if (Owned == NULL) return CR_INVALID_PARAMETER;
  *Owned = FALSE;
  if (Context == NULL || !Context->LockInitialized || Node == NULL)
    return CR_INVALID_PARAMETER;
  CrLockAcquire (&Context->Lock);
  Before = Node->ReferenceCount;
  Status = ClockEnableInternal (Context, Node, RateHz, TRUE);
  *Owned = !CR_ERROR (Status) || Node->ReferenceCount > Before;
  CrLockRelease (&Context->Lock);
  return Status;
}

/* Only a cold, exclusive ordinary mux transaction reaches this helper.
 * A selector can fail before any gate write; record that exact parent lease
 * without inventing gate ownership. A partially enabled branch retains its
 * original prefix/gate state so cleanup still stops the leaf first. */
STATIC VOID ClockRetainExternalMuxOwner(ClockNode *Node,ClockNode *Mux,UINT64 RateHz) {
  if (Mux->ReferenceCount==0) {
    Mux->ReferenceCount=1;Mux->ActiveRateHz=RateHz;
  }
  if (Node!=Mux && Node->ReferenceCount==0) {
    Node->ReferenceCount=1;Node->ActiveRateHz=RateHz;
    Node->ParentsHeld=1;Node->GateHeld=FALSE;
  }
  Node->ReleasePending=TRUE;Mux->ReleasePending=TRUE;
}

CR_STATUS ClockAcquireExternal (ClockDriverContext *Context, ClockNode *Node,
                                UINT64 RateHz, UINT32 Source, BOOLEAN *Owned)
{
  CR_STATUS Status;
  UINT32 Before;
  if (Owned == NULL) return CR_INVALID_PARAMETER;
  *Owned = FALSE;
  if (Context == NULL || !Context->LockInitialized || Node == NULL)
    return CR_INVALID_PARAMETER;
  CrLockAcquire (&Context->Lock);
  Before=Node->ReferenceCount;
  ClockNode *Mux=ClockConfigSource(Node);
  BOOLEAN Transaction=Mux!=NULL && Mux->Type==CLOCK_NODE_TYPE_MUX &&
      Node->ReferenceCount==0 && Mux->ReferenceCount==0 &&
      !Node->ReleasePending && !Mux->ReleasePending &&
      !Mux->MuxAcquirePending && !Mux->MuxRestorePending;
  Status = ClockSelectExternalSourceLocked (Context, Node, Source, 0, 0, 0, 0, TRUE);
  if (!CR_ERROR (Status)) {
    Status = ClockEnableInternal (Context, Node, RateHz, TRUE);
    if (Transaction && Mux->MuxAcquirePending) {
      if (!CR_ERROR(Status)) Mux->MuxAcquirePending=FALSE;
      else {
        Mux->MuxRestorePending=TRUE;
        ClockRetainExternalMuxOwner(Node,Mux,RateHz);
        /* The same ordinary release path orders gate restore before mux
         * restore. A failed cleanup remains visible through Owned below. */
        (VOID)ClockEnableInternal(Context,Node,RateHz,FALSE);
      }
    }
  } else if (Transaction && Mux->MuxRestorePending) {
    ClockRetainExternalMuxOwner(Node,Mux,RateHz);
  }
  *Owned = !CR_ERROR (Status) || Node->ReferenceCount > Before;
  CrLockRelease (&Context->Lock);
  return Status;
}

CR_STATUS ClockLibInit(IN OUT ClockDriverContext **ClockContext)
{
#ifndef _KERNEL_MODE
  CR_STATUS Status = CR_SUCCESS;
#endif

  if (ClockContext == NULL || *ClockContext == NULL ||
      (*ClockContext)->ClockControllers == NULL ||
      (*ClockContext)->ClockControllerCount == 0 ||
      (*ClockContext)->ClockControllerCount > CC_TYPE_MAX) {
    return CR_INVALID_PARAMETER;
  }

#ifdef _KERNEL_MODE
  /* The WDF adapter maps translated resources and owns their lifetime.
     Address is a kernel virtual address, never an ACPI physical address.
     Validate all windows before allowing the first register access. */
  if ((*ClockContext)->ClockControllers == NULL ||
      (*ClockContext)->ClockControllerCount == 0) {
    return CR_INVALID_PARAMETER;
  }
  for (UINTN i = 0; i < (*ClockContext)->ClockControllerCount; i++) {
    ClockController *Controller = &(*ClockContext)->ClockControllers[i];
    if (Controller->Address == 0 || Controller->Size == 0 ||
        !Controller->MemMapped || (Controller->Address & 3U) != 0 ||
        Controller->Address > (UINTN)-1 - Controller->Size) {
      return CR_NOT_READY;
    }
  }
#endif

  if (!(*ClockContext)->LockInitialized) {
    CrLockInit(&(*ClockContext)->Lock);
    (*ClockContext)->LockInitialized = TRUE;
  }

#ifndef _KERNEL_MODE
  // Native firmware owns GCD mappings; Windows owns WDF resource mappings.
  for (UINTN i = 0; i < (*ClockContext)->ClockControllerCount; i++) {
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
#endif
  return CR_SUCCESS;
}

CR_STATUS
ClockDeinit(IN OUT ClockDriverContext *ClockContext)
{
#ifndef _KERNEL_MODE
  CR_STATUS Status = CR_SUCCESS;
#endif

  if (ClockContext == NULL) {
    return CR_INVALID_PARAMETER;
  }

#ifndef _KERNEL_MODE
  // The Windows adapter releases its translated mappings after core teardown.
  for (UINTN i = 0; i < ClockContext->ClockControllerCount; i++) {
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
#else
  return CR_SUCCESS;
#endif
}
