/** @file Portable external-source and divider configuration.
 * SPDX-License-Identifier: MIT
 */
#include "clock_internal.h"

STATIC BOOLEAN ConfigRegisterFits(CONST ClockController *Controller,UINT64 Offset) {
  if (Controller==NULL || Controller->Address==0 || (Controller->Address&3U)!=0 ||
      Offset>(UINTN)-1-Controller->Address-3U) return FALSE;
#ifndef _KERNEL_MODE
  if (Controller->Size==0) return TRUE; /* legacy native mapped owner */
#else
  if (!Controller->MemMapped) return FALSE;
#endif
  return Controller->Size>=4 && Offset<=Controller->Size-4U &&
         Controller->Address<=(UINTN)-1-Controller->Size;
}
ClockNode *ClockConfigSource(ClockNode *Node) {
  if (Node==NULL) return NULL;
  /* External configuration has no requested child rate, so it cannot select
     a conditional dependency or manufacture its retained ownership ledger.
     These graphs use their qualified table-rate acquisition path instead. */
  if (ClockHasConditionalParents(Node)) return NULL;
  if (Node->ParentCount==0) return Node;
  if (Node->ParentCount!=1 || Node->Parents==NULL) return NULL;
  return Node->Parents[0];
}
STATIC BOOLEAN RcgConfigFits(ClockNode *Node) {
  return Node!=NULL && Node->Type==CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 &&
    Node->HIDWidth>=1 && Node->HIDWidth<=8 && Node->MNDWidth<=16 &&
    ConfigRegisterFits(Node->ParentController,Node->CmdRegister) &&
    ConfigRegisterFits(Node->ParentController,(UINT64)Node->CmdRegister+Node->CmdRCfgOffset+0x10U);
}

/* Repeated votes on one child do not increment the parent's vote. Also
 * inspect child owners when the caller names the mux itself. */
STATIC BOOLEAN MuxHasSharedChild(ClockDriverContext *Context,ClockNode *Mux) {
  if (Context->ClockControllers==NULL || Context->ClockControllerCount>CC_TYPE_MAX)
    return TRUE;
  for (UINTN C=0;C<Context->ClockControllerCount;++C) {
    ClockController *Controller=&Context->ClockControllers[C];
    if (Controller->ClkCount!=0 && Controller->Clks==NULL) return TRUE;
    for (UINTN I=0;I<Controller->ClkCount;++I) {
      ClockNode *Child=Controller->Clks[I];
      if (Child==NULL || Child->ReferenceCount<=1) continue;
      if (Child->ParentCount!=0 && Child->Parents==NULL) return TRUE;
      for (UINTN P=0;P<Child->ParentCount;++P)
        if (Child->Parents[P]==Mux) return TRUE;
    }
  }
  return FALSE;
}

CR_STATUS ClockRestoreRcgSnapshotLocked(ClockDriverContext *Context,ClockNode *Node) {
  if (Context==NULL || !RcgConfigFits(Node)) return CR_INVALID_PARAMETER;
  if (!Node->RcgRestorePending) return CR_SUCCESS;
  UINTN Config=Node->ParentController->Address+Node->CmdRegister+
               Node->CmdRCfgOffset+CLOCK_NODE_RCG_CMD_REGISTER_CFG_REG_OFFSET;
  /* Restore fractional operands before selecting/latching the old source. */
  for (UINTN I=1;I<4;++I)
    if (Node->RcgSavedMask[I]) CrMmioUpdateBits32(Config+4U*I,
      Node->RcgSavedMask[I],Node->RcgSavedValue[I]);
  CrMmioUpdateBits32(Config,Node->RcgSavedMask[0],Node->RcgSavedValue[0]);
  CrMemoryFence();
  for (UINTN I=0;I<4;++I)
    if (Node->RcgSavedMask[I] &&
        (CrMmioRead32(Config+4U*I)&Node->RcgSavedMask[I])!=Node->RcgSavedValue[I])
      return CR_DEVICE_ERROR;
  CR_STATUS Status=ClockRcg2UpdateConfig(Context,Node);
  if (CR_ERROR(Status)) return Status;
  for (UINTN I=0;I<4;++I)
    if (Node->RcgSavedMask[I] &&
        (CrMmioRead32(Config+4U*I)&Node->RcgSavedMask[I])!=Node->RcgSavedValue[I])
      return CR_DEVICE_ERROR;
  Node->RcgRestorePending=FALSE;
  return CR_SUCCESS;
}

CR_STATUS ClockRecoverConfigurationLocked(ClockDriverContext *Context,ClockNode *Node) {
  if (!RcgConfigFits(Node)) return CR_INVALID_PARAMETER;
  if (Node->RcgRestorePending) return ClockRestoreRcgSnapshotLocked(Context,Node);
  if (!Node->ReleasePending) return CR_SUCCESS;
  CR_STATUS Status=ClockRcg2UpdateConfig(Context,Node);
  if (!CR_ERROR(Status)) Node->ReleasePending=FALSE;
  return Status;
}
CR_STATUS ClockRecoverConfiguration(ClockDriverContext *Context,ClockNode *Node) {
  if (Context==NULL || !Context->LockInitialized || !RcgConfigFits(Node))
    return CR_INVALID_PARAMETER;
  CrLockAcquire(&Context->Lock);
  CR_STATUS Status=ClockRecoverConfigurationLocked(Context,Node);
  CrLockRelease(&Context->Lock);
  return Status;
}

STATIC CR_STATUS ConfigureRcg(ClockDriverContext *Context,ClockNode *Node,
                             UINT32 Source,UINT32 Divider,UINT32 M,UINT32 N,UINT32 TwiceD,
                             BOOLEAN SelectSource) {
  UINT32 Old[4];
  if (Node->RcgRestorePending) return CR_NOT_READY;
  UINTN Config=Node->ParentController->Address+Node->CmdRegister+
               Node->CmdRCfgOffset+CLOCK_NODE_RCG_CMD_REGISTER_CFG_REG_OFFSET;
  if (Node->ReleasePending) {
    CR_STATUS Recovery=ClockRcg2UpdateConfig(Context,Node);
    if (CR_ERROR(Recovery)) return Recovery;
    Node->ReleasePending=FALSE;
  }
  for (UINTN Index=0;Index<ARRAY_SIZE(Old);++Index) Old[Index]=CrMmioRead32(Config+Index*4U);
  UINT32 Value=(Old[0]&~(UINT32)(BIT(Node->HIDWidth)-1U)) | (Divider-1U);
  if (SelectSource) {
    Value=(Value&~(UINT32)(CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK |
                          CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_MSK)) |
          (UINT32)SET_FIELD(Source,CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK);
    if (M!=0 || N!=0 || TwiceD!=0) {
      UINT32 Mask=(UINT32)BIT(Node->MNDWidth)-1U;
      CrMmioUpdateBits32(Config+4,Mask,M);
      CrMmioUpdateBits32(Config+8,Mask,~(N-M));
      CrMmioUpdateBits32(Config+12,Mask,~TwiceD);
      if (M!=N) Value|=CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_DUAL_EDGE_MSK;
    }
  }
  CrMmioWrite32(Config,Value);CrMemoryFence();
  CR_STATUS Status=ClockRcg2UpdateConfig(Context,Node);
  if (CR_ERROR(Status)) {
    /* Restore the previous register image before returning the primary error.
     * A failed relatch must retain the mapping; recovery/teardown retries it. */
    for (UINTN Index=1;Index<ARRAY_SIZE(Old);++Index) CrMmioWrite32(Config+Index*4U,Old[Index]);
    CrMmioWrite32(Config,Old[0]);CrMemoryFence();
    Node->ReleasePending=CR_ERROR(ClockRcg2UpdateConfig(Context,Node));
    return Status;
  }
  Node->ExternalConfigPending=TRUE;
  return CR_SUCCESS;
}

CR_STATUS ClockRestoreMuxSnapshotLocked(ClockDriverContext *Context,ClockNode *Node) {
  if (Context==NULL || !Context->LockInitialized || Node==NULL ||
      Node->Type!=CLOCK_NODE_TYPE_MUX || Node->MuxMask==0 ||
      !ConfigRegisterFits(Node->ParentController,Node->MuxRegister)) return CR_INVALID_PARAMETER;
  if (!Node->MuxRestorePending) return CR_SUCCESS;
  if (!Node->MuxAcquirePending || Node->ReferenceCount>1) return CR_COMPROMISED_DATA;
  UINTN Address=Node->ParentController->Address+Node->MuxRegister;
  CrMmioUpdateBits32(Address,Node->MuxMask,Node->MuxSavedValue);CrMemoryFence();
  if ((CrMmioRead32(Address)&Node->MuxMask)!=Node->MuxSavedValue) return CR_DEVICE_ERROR;
  Node->MuxRestorePending=FALSE;Node->MuxAcquirePending=FALSE;
  return CR_SUCCESS;
}

CR_STATUS ClockSelectExternalSourceLocked(ClockDriverContext *Context,ClockNode *Node,
                                    UINT32 Source,UINT32 Divider,
                                    UINT32 M,UINT32 N,UINT32 TwiceD,BOOLEAN RequireUnowned) {
  ClockNode *Parent=ClockConfigSource(Node);
  if (Context==NULL || !Context->LockInitialized || Parent==NULL)
    return CR_INVALID_PARAMETER;
  CR_STATUS Status=CR_SUCCESS;
  if (Parent->Type==CLOCK_NODE_TYPE_MUX &&
      (Parent->MuxRestorePending || Parent->MuxAcquirePending)) return CR_NOT_READY;
  /* First acquisition cannot configure another owner's selector, including
   * a failed cleanup lease. The check and subsequent acquire share this lock. */
  if (RequireUnowned && (Node->ReferenceCount!=0 || Parent->ReferenceCount!=0 ||
      Node->ReleasePending || Parent->ReleasePending)) return CR_BUSY;
  if (Parent->Type==CLOCK_NODE_TYPE_PHY_MUX || Parent->Type==CLOCK_NODE_TYPE_MUX) {
    if ((Source!=0 && Source!=2) || M!=0 || N!=0 || TwiceD!=0 ||
        Parent->MuxMask==0 || Parent->MuxPhyValue==Parent->MuxRefValue ||
        ((Parent->MuxPhyValue|Parent->MuxRefValue)&~Parent->MuxMask)!=0 ||
        !ConfigRegisterFits(Parent->ParentController,Parent->MuxRegister))
      return CR_UNSUPPORTED;
    if (Parent->ReferenceCount>1 || Node->ReferenceCount>1 || MuxHasSharedChild(Context,Parent))
      { Status=CR_BUSY; goto MuxExit; }
    UINTN Address=Parent->ParentController->Address+Parent->MuxRegister;
    UINT32 Selection=Source==0?Parent->MuxPhyValue:Parent->MuxRefValue;
    UINT32 Previous=CrMmioRead32(Address)&Parent->MuxMask;
    if (RequireUnowned && Parent->Type==CLOCK_NODE_TYPE_MUX) {
      Parent->MuxSavedValue=Previous;Parent->MuxAcquirePending=TRUE;
    }
    CrMmioUpdateBits32(Address,Parent->MuxMask,Selection);CrMemoryFence();
    if ((CrMmioRead32(Address)&Parent->MuxMask)!=(Selection&Parent->MuxMask)) {
      CrMmioUpdateBits32(Address,Parent->MuxMask,Previous);CrMemoryFence();
      if (RequireUnowned && Parent->Type==CLOCK_NODE_TYPE_MUX) {
        Parent->MuxRestorePending=(CrMmioRead32(Address)&Parent->MuxMask)!=Previous;
        Parent->MuxAcquirePending=Parent->MuxRestorePending;
        Parent->ReleasePending=Parent->MuxRestorePending;
      }
      Status=CR_DEVICE_ERROR;
    }
MuxExit:
    return Status;
  }
  if (!RcgConfigFits(Parent)) return CR_UNSUPPORTED;
  if (Source>7 || Divider==0 || Divider-1U>=(UINT32)BIT(Parent->HIDWidth))
    return CR_INVALID_PARAMETER;
  if (M!=0 || N!=0 || TwiceD!=0) {
    if (Parent->MNDWidth==0 || M==0 || N==0 || M>N || TwiceD==0 ||
        N>(UINT32)BIT(Parent->MNDWidth)-1U || TwiceD>(UINT32)BIT(Parent->MNDWidth)-1U)
      return CR_INVALID_PARAMETER;
  }
  if (Node->ReferenceCount!=0 || Parent->ReferenceCount!=0) Status=CR_BUSY;
  else Status=ConfigureRcg(Context,Parent,Source,Divider,M,N,TwiceD,TRUE);
  return Status;
}

CR_STATUS ClockSelectExternalSource(ClockDriverContext *Context,ClockNode *Node,
                                    UINT32 Source,UINT32 Divider,
                                    UINT32 M,UINT32 N,UINT32 TwiceD) {
  if (Context==NULL || !Context->LockInitialized) return CR_INVALID_PARAMETER;
  CrLockAcquire(&Context->Lock);
  CR_STATUS Status=ClockSelectExternalSourceLocked(Context,Node,Source,Divider,
                                                  M,N,TwiceD,FALSE);
  CrLockRelease(&Context->Lock);
  return Status;
}

CR_STATUS ClockSetDivider(ClockDriverContext *Context,ClockNode *Node,UINT32 Divider) {
  if (Context==NULL || !Context->LockInitialized || Node==NULL || Divider==0)
    return CR_INVALID_PARAMETER;
  CR_STATUS Status=CR_SUCCESS;
  if (Node->DividerRegister!=0 || Node->Type==CLOCK_NODE_TYPE_READ_ONLY_DIVIDER) {
    if (Node->RequiredDivider!=0 && Divider!=Node->RequiredDivider) return CR_UNSUPPORTED;
    if (Node->DividerWidth==0 || Node->DividerWidth>16 ||
        Divider-1U>=(UINT32)BIT(Node->DividerWidth) ||
        !ConfigRegisterFits(Node->ParentController,Node->DividerRegister))
      return CR_INVALID_PARAMETER;
    UINTN Address=Node->ParentController->Address+Node->DividerRegister;
    UINT32 Mask=(UINT32)BIT(Node->DividerWidth)-1U;
    CrLockAcquire(&Context->Lock);
    if (Node->DividerReadOnly || Node->Type==CLOCK_NODE_TYPE_READ_ONLY_DIVIDER) {
      Status=(CrMmioRead32(Address)&Mask)==Divider-1U?CR_SUCCESS:CR_UNSUPPORTED;
    } else if (Node->ReferenceCount!=0) Status=CR_BUSY;
    else {
      UINT32 Previous=CrMmioRead32(Address)&Mask;
      CrMmioUpdateBits32(Address,Mask,Divider-1U);CrMemoryFence();
      if ((CrMmioRead32(Address)&Mask)!=Divider-1U) {
        CrMmioUpdateBits32(Address,Mask,Previous);CrMemoryFence();
        Status=CR_DEVICE_ERROR;
      }
    }
    CrLockRelease(&Context->Lock);
    return Status;
  }
  ClockNode *Parent=ClockConfigSource(Node);
  if (!RcgConfigFits(Parent)) return CR_UNSUPPORTED;
  if (Divider-1U>=(UINT32)BIT(Parent->HIDWidth)) return CR_INVALID_PARAMETER;
  CrLockAcquire(&Context->Lock);
  if (Node->ReferenceCount!=0 || Parent->ReferenceCount!=0) Status=CR_BUSY;
  else Status=ConfigureRcg(Context,Parent,0,Divider,0,0,0,FALSE);
  CrLockRelease(&Context->Lock);
  return Status;
}
