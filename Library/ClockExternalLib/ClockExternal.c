/* DSI0 ClockLib-owned source/divider configuration and retained rollback. MIT */
#include <Library/ClockExternalLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>
STATIC CONST CHAR8 *mNativeNames[]={
  "disp_cc_mdss_byte0_clk", "disp_cc_mdss_byte0_intf_clk", "disp_cc_mdss_pclk0_clk",
  "disp_cc_mdss_dptx0_aux_clk", "disp_cc_mdss_dptx0_link_clk",
  "disp_cc_mdss_dptx0_link_intf_clk", "disp_cc_mdss_dptx0_pixel0_clk",
  "disp_cc_mdss_byte0_clk_src", "disp_cc_mdss_pclk0_clk_src",
  "disp_cc_mdss_dptx0_aux_clk_src", "disp_cc_mdss_dptx0_link_clk_src",
  "disp_cc_mdss_dptx0_pixel0_clk_src"
};
STATIC CONST UINT32 mNativeRegisters[]={
  0x8028,0x802c,0x8004,0x8058,0x8040,0x8048,0x8050,
  0x8134,0x80bc,0x81ec,0x819c,0x81bc
};
STATIC CONST UINT32 mNativeParents[]={7,7,8,9,10,10,11};
STATIC BOOLEAN Fits(ClockController *C,UINT32 O) {
  return C && C->MemMapped && C->Address && !(C->Address&3U) && C->Size>=4 &&
    O<=C->Size-4 && C->Address<=(UINTN)-1-C->Size;
}
STATIC CR_STATUS Check(CR_CLOCK_EXTERNAL *E) {
  if (CR_ERROR(CrMutexCheckLevel()) || !E || !E->Initialized || !E->Core || !E->Core->LockInitialized || !E->Core->ClockControllers ||
      E->Core->ClockControllerCount<=CC_TYPE_DISP_CC || E->Core->ClockControllerCount>CC_TYPE_MAX)
    return CR_INVALID_PARAMETER;
  ClockController *C=&E->Core->ClockControllers[CC_TYPE_DISP_CC];
  if (!Fits(C,0x9000) || !Fits(C,0x814c)) return CR_NOT_READY;
  for(UINT32 I=0;I<2;++I) if (!Fits(C,E->Source[I]->CmdRegister+E->Source[I]->CmdRCfgOffset+0x10))
    return CR_NOT_READY;
  if(E->SecondPresent)for(UINT32 I=0;I<2;++I)
    if(!E->SecondSource[I] || !Fits(C,E->SecondSource[I]->CmdRegister+0x10))return CR_NOT_READY;
  return CR_SUCCESS;
}
STATIC CR_STATUS Owner(CR_CLOCK_EXTERNAL *E,VOID *O) {
  CR_STATUS S=Check(E); if(CR_ERROR(S))return S;
  return O && O==E->Owner ? CR_SUCCESS : CR_BUSY;
}
CR_STATUS CrClockExternalInit(CR_CLOCK_EXTERNAL *E,ClockDriverContext *C) {
  if(CR_ERROR(CrMutexCheckLevel()) || !E || !C || !C->LockInitialized || !C->ClockControllers ||
     C->ClockControllerCount<=CC_TYPE_DISP_CC || C->ClockControllerCount>CC_TYPE_MAX)
    return CR_INVALID_PARAMETER;
  if(E->Initialized)return CR_ALREADY_STARTED;
  CR_CLOCK_EXTERNAL Candidate={0}; Candidate.Core=C;
  CONST CHAR8 *Leaves[]={"disp_cc_mdss_byte0_clk","disp_cc_mdss_byte0_intf_clk","disp_cc_mdss_pclk0_clk"};
  CONST UINT32 Gates[]={0x8028,0x802c,0x8004};
  for(UINT32 I=0;I<3;++I) {
    ClockControllerType T; CR_STATUS S=GetClockNode(C,Leaves[I],&T,NULL,&Candidate.Leaf[I]);
    if(CR_ERROR(S))return S;
    ClockNode *N=Candidate.Leaf[I];
    if(T!=CC_TYPE_DISP_CC || N->Type!=CLOCK_NODE_TYPE_BRANCH_2 || N->ParentCount!=1 ||
       !N->Parents || !N->Parents[0] || N->EnableRegister!=Gates[I] || N->EnableMsk!=1 ||
       !Fits(N->ParentController,N->EnableRegister) || !Fits(N->ParentController,N->HaltRegister))
      return CR_UNSUPPORTED;
  }
  Candidate.Source[0]=Candidate.Leaf[0]->Parents[0];Candidate.Source[1]=Candidate.Leaf[2]->Parents[0];
  if(Candidate.Leaf[1]->Parents[0]!=Candidate.Source[0] ||
     Candidate.Leaf[1]->DividerRegister!=0x814c || Candidate.Leaf[1]->DividerWidth!=4 ||
     Candidate.Leaf[1]->DividerReadOnly) return CR_UNSUPPORTED;
  for(UINT32 I=0;I<2;++I) {
    ClockNode *N=Candidate.Source[I];
    if(N->Type!=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 || N->ParentController!=&C->ClockControllers[CC_TYPE_DISP_CC] ||
       N->CmdRegister!=(I==0?0x8134U:0x80bcU) || N->CmdRCfgOffset!=0 || N->HIDWidth!=5 ||
       N->MNDWidth!=(I==0?0U:8U) || !Fits(N->ParentController,N->CmdRegister+0x10))return CR_UNSUPPORTED;
  }
  /* Older exact DSI0-only graphs remain valid. A partially advertised second
     group fails qualification; names alone never manufacture its topology. */
  ClockNode *Second;
  CR_STATUS Found=GetClockNode(C,"disp_cc_mdss_byte1_clk",NULL,NULL,&Second);
  if(Found!=CR_NOT_FOUND && Found!=CR_SUCCESS)return Found;
  if(Found==CR_SUCCESS) {
    CONST CHAR8 *Names[]={"disp_cc_mdss_byte1_clk","disp_cc_mdss_byte1_intf_clk","disp_cc_mdss_pclk1_clk"};
    CONST UINT32 Gates1[]={0x8030,0x8034,0x8008};
    for(UINT32 I=0;I<3;++I) {
      ClockControllerType T;ClockNode *N;
      CR_STATUS Q=GetClockNode(C,Names[I],&T,NULL,&N);if(Q!=CR_SUCCESS)return Q;
      if(T!=CC_TYPE_DISP_CC || N->Type!=CLOCK_NODE_TYPE_BRANCH_2 ||
          N->ParentController!=&C->ClockControllers[CC_TYPE_DISP_CC] ||
          N->ParentCount!=1 || !N->Parents || !N->Parents[0] ||
          N->EnableRegister!=Gates1[I] || N->HaltRegister!=Gates1[I] ||
          N->EnableMsk!=1 || N->HwClockCtrl || N->HaltCheckFlag!=CLOCK_NODE_BRANCH_HALT ||
          !Fits(N->ParentController,N->EnableRegister))return CR_UNSUPPORTED;
      Candidate.SecondLeaf[I]=N;
    }
    Candidate.SecondSource[0]=Candidate.SecondLeaf[0]->Parents[0];
    Candidate.SecondSource[1]=Candidate.SecondLeaf[2]->Parents[0];
    if(Candidate.SecondLeaf[1]->Parents[0]!=Candidate.SecondSource[0] ||
        Candidate.SecondLeaf[1]->DividerRegister!=0x8168 ||
        Candidate.SecondLeaf[1]->DividerWidth!=4 || Candidate.SecondLeaf[1]->DividerReadOnly)
      return CR_UNSUPPORTED;
    for(UINT32 I=0;I<2;++I) {
      ClockNode *N=Candidate.SecondSource[I];
      if(N->Type!=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 ||
          N->ParentController!=&C->ClockControllers[CC_TYPE_DISP_CC] ||
          N->CmdRegister!=(I==0?0x8150U:0x80d4U) || N->CmdRCfgOffset ||
          N->HIDWidth!=5 || N->MNDWidth!=(I==0?0U:8U) ||
          !Fits(N->ParentController,N->CmdRegister+0x10))return CR_UNSUPPORTED;
    }
    Candidate.SecondPresent=TRUE;
  }
  CONST CHAR8 *Access[]={"gcc_disp_ahb_clk","gcc_disp_xo_clk","disp_cc_mdss_ahb_clk"};
  for(UINT32 I=0;I<3;++I) {
    CR_STATUS S=GetClockNode(C,Access[I],NULL,NULL,&Candidate.Access[I]);if(CR_ERROR(S))return S;
  }
  CR_STATUS S=GetGdscNode(C,"disp_cc_mdss_core_gdsc",&Candidate.Domain);if(CR_ERROR(S))return S;
  for(UINT32 I=0;I<12;++I) {
    ClockControllerType T;ClockNode *N;
    S=GetClockNode(C,mNativeNames[I],&T,NULL,&N);if(S!=CR_SUCCESS)return S;
    if(T!=CC_TYPE_DISP_CC || N->ParentController!=&C->ClockControllers[CC_TYPE_DISP_CC] ||
       !Fits(N->ParentController,mNativeRegisters[I]+(I>=7?16U:0U)))return CR_UNSUPPORTED;
    if(I<7) {
      if(N->Type!=CLOCK_NODE_TYPE_BRANCH_2 || N->EnableMsk!=1 ||
         N->EnableRegister!=mNativeRegisters[I] || N->HaltRegister!=mNativeRegisters[I] ||
         N->HaltCheckFlag!=CLOCK_NODE_BRANCH_HALT || N->HwClockCtrl ||
         N->ParentCount!=1 || !N->Parents || !N->Parents[0])return CR_UNSUPPORTED;
    } else if(N->Type!=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 ||
        N->CmdRegister!=mNativeRegisters[I] || N->CmdRCfgOffset || N->HIDWidth!=5 ||
        N->MNDWidth!=(I==8?8U:I==11?16U:0U))return CR_UNSUPPORTED;
    Candidate.NativeNode[I]=N;
  }
  for(UINT32 I=0;I<7;++I)if(Candidate.NativeNode[I]->Parents[0]!=
      Candidate.NativeNode[mNativeParents[I]])return CR_UNSUPPORTED;
  Candidate.Initialized=TRUE; S=Check(&Candidate);if(CR_ERROR(S))return S;
  *E=Candidate;return CR_SUCCESS;
}
CR_STATUS CrClockExternalReserve(CR_CLOCK_EXTERNAL *E,VOID *O) {
  CR_STATUS S=Check(E);if(CR_ERROR(S))return S;if(!O)return CR_INVALID_PARAMETER;
  if(E->Owner)return E->Owner==O?CR_SUCCESS:CR_BUSY;
  for(UINT32 I=0;I<3;++I) if(E->Leaf[I]->ReferenceCount || E->Leaf[I]->ReleasePending)return CR_BUSY;
  for(UINT32 I=0;I<2;++I) if(E->Source[I]->ReferenceCount || E->Source[I]->ReleasePending ||
      E->Source[I]->ExternalConfigPending)return CR_BUSY;
  if(E->SecondPresent) {
    for(UINT32 I=0;I<3;++I)if(E->SecondLeaf[I]->ReferenceCount || E->SecondLeaf[I]->ReleasePending)return CR_BUSY;
    for(UINT32 I=0;I<2;++I)if(E->SecondSource[I]->ReferenceCount || E->SecondSource[I]->ReleasePending ||
        E->SecondSource[I]->ExternalConfigPending)return CR_BUSY;
  }
  E->Owner=O;return CR_SUCCESS;
}
STATIC BOOLEAN Related(CR_CLOCK_EXTERNAL *E,ClockNode *N,UINT32 Depth,UINT32 *Budget) {
  if(!N || Depth==32 || *Budget==0 || N->ParentCount>32 || (N->ParentCount&&!N->Parents))return TRUE;
  --*Budget;
  for(UINT32 I=0;I<3;++I)if(N==E->Leaf[I])return TRUE;
  for(UINT32 I=0;I<2;++I)if(N==E->Source[I])return TRUE;
  if(E->SecondPresent) {
    for(UINT32 I=0;I<3;++I)if(N==E->SecondLeaf[I])return TRUE;
    for(UINT32 I=0;I<2;++I)if(N==E->SecondSource[I])return TRUE;
  }
  for(UINTN I=0;I<N->ParentCount;++I)if(Related(E,N->Parents[I],Depth+1,Budget))return TRUE;
  return FALSE;
}
CR_STATUS CrClockExternalGroupConflict(CR_CLOCK_EXTERNAL *E,ClockNode *N) {
  CR_STATUS S=Check(E);if(S!=CR_SUCCESS)return S;if(!N)return CR_INVALID_PARAMETER;
  UINT32 Budget=512;return Related(E,N,0,&Budget)?CR_BUSY:CR_SUCCESS;
}
CR_STATUS CrClockExternalGuard(CR_CLOCK_EXTERNAL *E,ClockNode *N) {
  CR_STATUS S=Check(E);if(CR_ERROR(S))return S;if(!N)return CR_INVALID_PARAMETER;
  if(!E->Owner)return CR_SUCCESS;
  if(E->NativeOwner==E->Owner) {
    S=CrClockExternalNativeConflict(E,N);if(S!=CR_SUCCESS)return S;
    if(CrClockExternalNativePending(E)) {
      if(N==E->Domain)return CR_BUSY;
      for(UINT32 I=0;I<3;++I)if(N==E->Access[I])return CR_BUSY;
    }
  }
  UINT32 Budget=512;
  return Related(E,N,0,&Budget)?CR_BUSY:CR_SUCCESS;
}
CR_STATUS CrClockExternalGet(CR_CLOCK_EXTERNAL *E,VOID *O,BOOLEAN *Enabled,BOOLEAN *Pending) {
  if(!Enabled||!Pending)return CR_INVALID_PARAMETER;*Enabled=FALSE;*Pending=FALSE;
  CR_STATUS S=Owner(E,O);if(CR_ERROR(S))return S;
  *Enabled=E->Enabled;*Pending=E->RecoveryPending || CrClockExternalNativePending(E);
  for(UINT32 I=0;I<3;++I) *Pending=*Pending || E->Leaf[I]->ReleasePending ||
    (E->Leaf[I]->ReferenceCount!=(E->Held[I]?1U:0U));
  for(UINT32 I=0;I<2;++I) *Pending=*Pending || E->Source[I]->ReleasePending;
  if(E->Enabled && (E->Source[0]->ReferenceCount!=2 || E->Source[1]->ReferenceCount!=1))*Pending=TRUE;
  if(E->SecondPresent) {
    *Pending=*Pending || E->SecondRecoveryPending;
    for(UINT32 I=0;I<3;++I)*Pending=*Pending || E->SecondLeaf[I]->ReleasePending ||
      E->SecondLeaf[I]->ReferenceCount!=(E->SecondHeld[I]?1U:0U);
    for(UINT32 I=0;I<2;++I)*Pending=*Pending || E->SecondSource[I]->ReleasePending;
    if(E->SecondEnabled && (E->SecondSource[0]->ReferenceCount!=2 || E->SecondSource[1]->ReferenceCount!=1))*Pending=TRUE;
  }
  return CR_SUCCESS;
}
STATIC BOOLEAN Accessible(CR_CLOCK_EXTERNAL *E) {
  if(!E->Domain->ReferenceCount || E->Domain->ReleasePending)return FALSE;
  for(UINT32 I=0;I<3;++I)if(!E->Access[I]->ReferenceCount || E->Access[I]->ReleasePending)return FALSE;
  return TRUE;
}
typedef struct {
  ClockNode **Leaf, **Source;
  UINT32 (*SavedSource)[4], *SavedDivider;
  BOOLEAN *Held, *SourceSaved, *DividerSaved, *Enabled, *RecoveryPending;
} GROUP;
STATIC GROUP Group(CR_CLOCK_EXTERNAL *E,UINT32 I) {
  GROUP G;
  if(I==0)G=(GROUP){E->Leaf,E->Source,E->SavedSource,&E->SavedDivider,E->Held,
    E->SourceSaved,&E->DividerSaved,&E->Enabled,&E->RecoveryPending};
  else G=(GROUP){E->SecondLeaf,E->SecondSource,E->SavedSecondSource,&E->SavedSecondDivider,
    E->SecondHeld,E->SecondSourceSaved,&E->SecondDividerSaved,&E->SecondEnabled,&E->SecondRecoveryPending};
  return G;
}
STATIC CR_STATUS Exact(CR_STATUS S) {return S==CR_SUCCESS || CR_ERROR(S)?S:CR_NOT_READY;}
STATIC BOOLEAN Pending(GROUP *G) {
  BOOLEAN P=*G->RecoveryPending;
  for(UINT32 I=0;I<3;++I)P=P || G->Leaf[I]->ReleasePending ||
    G->Leaf[I]->ReferenceCount!=(G->Held[I]?1U:0U);
  for(UINT32 I=0;I<2;++I)P=P || G->Source[I]->ReleasePending;
  if(*G->Enabled && (G->Source[0]->ReferenceCount!=2 || G->Source[1]->ReferenceCount!=1))P=TRUE;
  return P;
}
STATIC UINTN Config(GROUP *G,UINT32 I) {
  ClockNode *N=G->Source[I];return N->ParentController->Address+N->CmdRegister+4;
}
STATIC CR_STATUS RestoreSource(CR_CLOCK_EXTERNAL *E,GROUP *G,UINT32 I) {
  if(!G->SourceSaved[I])return CR_SUCCESS;
  ClockNode *N=G->Source[I];if(N->ReferenceCount)return CR_BUSY;
  CrLockAcquire(&E->Core->Lock);
  UINTN A=Config(G,I);
  for(UINT32 R=1;R<4;++R)CrMmioWrite32(A+R*4,G->SavedSource[I][R]);
  CrMmioWrite32(A,G->SavedSource[I][0]);CrMemoryFence();
  CR_STATUS S=Exact(ClockRcg2UpdateConfig(E->Core,N));
  if(S==CR_SUCCESS)for(UINT32 R=0;R<4;++R)
    if(CrMmioRead32(A+R*4)!=G->SavedSource[I][R]){S=CR_DEVICE_ERROR;break;}
  N->ReleasePending=S!=CR_SUCCESS;
  if(S==CR_SUCCESS){N->ExternalConfigPending=FALSE;G->SourceSaved[I]=FALSE;}
  CrLockRelease(&E->Core->Lock);return S;
}
CR_STATUS CrClockExternalGetPlan(CR_CLOCK_EXTERNAL *E,VOID *O,UINT32 I,
    CR_CLOCK_DSI_PLAN *P,BOOLEAN *On,BOOLEAN *Recovery) {
  if(!P || !On || !Recovery || I>1)return CR_INVALID_PARAMETER;
  CR_STATUS S=Owner(E,O);if(S!=CR_SUCCESS)return S;
  if((I && !E->SecondPresent) || !E->PlanValid[I])return CR_NOT_READY;
  GROUP G=Group(E,I);
  *P=E->Plans[I];*On=*G.Enabled;*Recovery=Pending(&G) || CrClockExternalNativePending(E);
  return CR_SUCCESS;
}
CR_STATUS CrClockExternalSetPlan(CR_CLOCK_EXTERNAL *E,VOID *O,UINT32 I,
    CONST CR_CLOCK_DSI_PLAN *Plan,BOOLEAN Enable) {
  CR_STATUS S=Owner(E,O);if(S!=CR_SUCCESS)return S;
  if(I>1 || Enable>1)return CR_INVALID_PARAMETER;
  if(I && !E->SecondPresent)return CR_UNSUPPORTED;
  if(CrClockExternalNativePending(E))return CR_NOT_READY;
  GROUP G=Group(E,I);
  if(Enable) {
    S=CrClockDsiPlanValidate(Plan);if(S!=CR_SUCCESS)return S;
    if(Plan->Controller!=I)return CR_INVALID_PARAMETER;
    if(E->PlanValid[I] && !CrClockDsiPlanEqual(Plan,&E->Plans[I]))return CR_BUSY;
    if(*G.Enabled && !*G.RecoveryPending)return Pending(&G)?CR_NOT_READY:CR_SUCCESS;
    if(E->RecoveryPending || E->SecondRecoveryPending || G.SourceSaved[0] ||
        G.SourceSaved[1] || *G.DividerSaved)return CR_NOT_READY;
  }
  if(!Enable && !*G.RecoveryPending && !*G.Enabled && !G.SourceSaved[0] &&
      !G.SourceSaved[1] && !*G.DividerSaved)return CR_SUCCESS;
  if(!Accessible(E))return CR_NOT_READY;
  if(Enable) {
    /* Validate all software and inherited hardware ownership before saving
       the immutable plan or issuing the first selector mutation. */
    for(UINT32 J=0;J<3;++J)if(G.Leaf[J]->ReferenceCount || G.Leaf[J]->ReleasePending)return CR_BUSY;
    for(UINT32 J=0;J<2;++J)if(G.Source[J]->ReferenceCount || G.Source[J]->ReleasePending ||
        G.Source[J]->ExternalConfigPending)return CR_BUSY;
    CrLockAcquire(&E->Core->Lock);BOOLEAN Live=FALSE;
    for(UINT32 J=0;J<3;++J)Live=Live ||
      (CrMmioRead32(G.Leaf[J]->ParentController->Address+G.Leaf[J]->EnableRegister)&G.Leaf[J]->EnableMsk)!=0;
    for(UINT32 J=0;J<2;++J)Live=Live ||
      (CrMmioRead32(G.Source[J]->ParentController->Address+G.Source[J]->CmdRegister)&BIT(1))!=0;
    CrLockRelease(&E->Core->Lock);if(Live)return CR_NOT_READY;
    E->Plans[I]=*Plan;E->PlanValid[I]=TRUE;
  }
  *G.RecoveryPending=TRUE;
  if(Enable) {
    for(UINT32 J=0;J<2;++J) {
      if(G.Source[J]->ReferenceCount || G.Source[J]->ReleasePending || G.Source[J]->ExternalConfigPending)return CR_BUSY;
      CrLockAcquire(&E->Core->Lock);
      for(UINT32 R=0;R<4;++R)G.SavedSource[J][R]=CrMmioRead32(Config(&G,J)+R*4);
      G.SourceSaved[J]=TRUE;CrLockRelease(&E->Core->Lock);
      UINT32 M=J && Plan->PixelM!=Plan->PixelN?Plan->PixelM:0;
      UINT32 N=M?Plan->PixelN:0;
      /* Linux DISPCC map2: PHY0 dsiclk/byte=1/2, PHY1 dsiclk/byte=3/4.
         Even a PLL0-master bonded link consumes the PHY1 output on host1. */
      S=Exact(ClockSelectExternalSource(E->Core,G.Leaf[J==0?0:2],
          (J==0?2U:1U)+I*2U,2,M,N,N));if(S!=CR_SUCCESS)return S;
      UINTN A=Config(&G,J);UINT32 Expected=((J==0?2U:1U)+I*2U)<<8|1U|(M?0x2000U:0U);
      CrLockAcquire(&E->Core->Lock);
      BOOLEAN Match=(CrMmioRead32(A)&0x371fU)==Expected;
      if(M)Match=Match && (CrMmioRead32(A+4)&255U)==M &&
        (CrMmioRead32(A+8)&255U)==((~(N-M))&255U) &&
        (CrMmioRead32(A+12)&255U)==((~N)&255U);
      CrLockRelease(&E->Core->Lock);
      if(!Match)return CR_DEVICE_ERROR;
    }
    ClockNode *D=G.Leaf[1];
    *G.SavedDivider=CrMmioRead32(D->ParentController->Address+D->DividerRegister)&15U;*G.DividerSaved=TRUE;
    S=Exact(ClockSetDivider(E->Core,D,Plan->ByteInterfaceDivider));if(S!=CR_SUCCESS)return S;
    for(UINT32 J=0;J<3;++J) {
      UINT32 Before=G.Leaf[J]->ReferenceCount;
      S=Exact(ClockEnable(E->Core,G.Leaf[J],0,TRUE));
      G.Held[J]=S==CR_SUCCESS || G.Leaf[J]->ReferenceCount>Before;if(S!=CR_SUCCESS)return S;
    }
    *G.Enabled=TRUE;*G.RecoveryPending=FALSE;return CR_SUCCESS;
  }
  for(UINT32 J=3;J-- >0;)if(G.Held[J]) {
    S=Exact(ClockEnable(E->Core,G.Leaf[J],0,FALSE));if(S!=CR_SUCCESS)return S;G.Held[J]=FALSE;
  }
  *G.Enabled=FALSE;
  if(*G.DividerSaved) {
    S=Exact(ClockSetDivider(E->Core,G.Leaf[1],*G.SavedDivider+1));if(S!=CR_SUCCESS)return S;*G.DividerSaved=FALSE;
  }
  for(UINT32 J=2;J-- >0;){S=RestoreSource(E,&G,J);if(S!=CR_SUCCESS)return S;}
  *G.RecoveryPending=FALSE;E->PlanValid[I]=FALSE;
  return CR_SUCCESS;
}
CR_STATUS CrClockExternalSet(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 Pixel,UINT64 Byte,BOOLEAN Enable) {
  CR_STATUS S=Owner(E,O);if(S!=CR_SUCCESS)return S;
  if(Enable && (Pixel!=148500000 || Byte!=111375000))return CR_UNSUPPORTED;
  CR_CLOCK_DSI_PLAN P={CR_CLOCK_DSI_PLAN_REVISION,0,148500000,111375000,148500000,1,1,2,0};
  return CrClockExternalSetPlan(E,O,0,Enable?&P:NULL,Enable);
}
CR_STATUS CrClockExternalRead(CR_CLOCK_EXTERNAL *E,VOID *O,UINT32 Offset,UINT32 *Value) {
  if(!Value)return CR_INVALID_PARAMETER;*Value=0;CR_STATUS S=Owner(E,O);if(CR_ERROR(S))return S;
  if(Offset!=0x9000 && Offset!=0x8138 && Offset!=0x80c0 && Offset!=0x814c &&
      (!E->SecondPresent || (Offset!=0x8154 && Offset!=0x80d8 && Offset!=0x8168)))return CR_UNSUPPORTED;
  if(!Accessible(E))return CR_NOT_READY;
  *Value=CrMmioRead32(E->Core->ClockControllers[CC_TYPE_DISP_CC].Address+Offset);return CR_SUCCESS;
}
CR_STATUS CrClockExternalReleaseOwner(CR_CLOCK_EXTERNAL *E,VOID *O) {
  CR_STATUS S=Owner(E,O);if(CR_ERROR(S))return S;if(E->SecondPresent) {S=CrClockExternalSetPlan(E,O,1,NULL,FALSE);if(S!=CR_SUCCESS)return S;}
  S=CrClockExternalSetPlan(E,O,0,NULL,FALSE);
  if(S==CR_SUCCESS)E->Owner=NULL;return S;
}
CR_STATUS CrClockExternalDrain(CR_CLOCK_EXTERNAL *E) {
  CR_STATUS S=Check(E);if(CR_ERROR(S))return S;
  return E->Owner?CrClockExternalReleaseOwner(E,E->Owner):CR_SUCCESS;
}
CR_STATUS CrClockExternalUninit(CR_CLOCK_EXTERNAL *E) {
  CR_STATUS S=Check(E);if(CR_ERROR(S))return S;if(E->Owner)return CR_BUSY;
  cr_memset(E,0,sizeof(*E));return CR_SUCCESS;
}

/* Both the transient retired group and the generic vote adapter use this
   bounded pure graph query. Unknown/cyclic graph shapes fail closed. */
STATIC BOOLEAN NativeRelated(CR_CLOCK_EXTERNAL *E,ClockNode *N,UINT32 Depth,UINT32 *Budget) {
  if(!N || Depth==32 || !*Budget || N->ParentCount>32 || (N->ParentCount&&!N->Parents))return TRUE;
  --*Budget;
  for(UINT32 I=0;I<12;++I)if(N==E->NativeNode[I])return TRUE;
  for(UINTN I=0;I<N->ParentCount;++I)if(NativeRelated(E,N->Parents[I],Depth+1,Budget))return TRUE;
  return FALSE;
}
CR_STATUS CrClockExternalNativeConflict(CR_CLOCK_EXTERNAL *E,ClockNode *N) {
  CR_STATUS S=Check(E);if(S!=CR_SUCCESS)return S;if(!N)return CR_INVALID_PARAMETER;
  UINT32 Budget=512;return NativeRelated(E,N,0,&Budget)?CR_BUSY:CR_SUCCESS;
}
STATIC BOOLEAN SameBytes(CONST VOID *A,CONST VOID *B,UINTN Size) {
  CONST UINT8 *X=A,*Y=B;for(UINTN I=0;I<Size;++I)if(X[I]!=Y[I])return FALSE;
  return TRUE;
}
STATIC CR_STATUS NativeMask(CONST CR_MDSS_NATIVE_HANDOFF *R,UINT32 *Mask) {
  CR_STATUS S=CrMdssHandoffCanClaim(R);if(S!=CR_SUCCESS)return S;
  if(R->Routes[0].State==CR_MDSS_NATIVE_ABSENT)return CR_UNSUPPORTED;
  *Mask=0;
  for(UINT32 I=0;I<R->ClockCount;++I) {
    CONST CR_MDSS_NATIVE_CLOCK *C=&R->Clocks[I];
    if(C->Owner==CR_MDSS_NATIVE_DSI || C->Owner==CR_MDSS_NATIVE_DP) {
      STATIC CONST CHAR8 *Shared[]={"gcc_disp_ahb_clk","gcc_disp_xo_clk",
        "disp_cc_mdss_ahb_clk","disp_cc_mdss_mdp_clk","gcc_disp_hf_axi_clk",
        "gcc_disp_sf_axi_clk","disp_cc_mdss_mdp_lut_clk","disp_cc_mdss_vsync_clk",
        "disp_cc_mdss_esc0_clk"};
      STATIC CONST UINT64 Rates[]={0,0,19200000,200000000,0,0,200000000,19200000,19200000};
      BOOLEAN Known=FALSE;
      for(UINT32 J=0;J<ARRAY_SIZE(Shared);++J)if(!cr_strcmp(C->Name,Shared[J])) {
        if((J==8 && C->Owner!=CR_MDSS_NATIVE_DSI) || C->RateHz!=Rates[J] ||
           cr_strcmp(C->Controller,(J==0 || J==1 || J==4 || J==5)?"gcc":"dispcc"))return CR_UNSUPPORTED;
        Known=TRUE;
      }
      for(UINT32 J=0;J<7;++J)if(!cr_strcmp(C->Name,mNativeNames[J]))Known=TRUE;
      if(!Known)return CR_UNSUPPORTED;
    }
    for(UINT32 J=0;J<12;++J)if(!cr_strcmp(C->Name,mNativeNames[J])) {
      /* Native sources are represented by their concrete leaf demands. A
         separate source/foreign owner is not this transfer's baseline. */
      if(J>=7 || C->Owner!=(J<3?CR_MDSS_NATIVE_DSI:CR_MDSS_NATIVE_DP) ||
         cr_strcmp(C->Controller,"dispcc") || C->Flags!=CR_MDSS_NATIVE_OWNED ||
         C->RateHz!=(J==3?19200000U:0U) || (*Mask&BIT(J)))return CR_UNSUPPORTED;
      *Mask|=BIT(J)|BIT(mNativeParents[J]);
    }
  }
  if(R->Routes[0].State==CR_MDSS_NATIVE_LIVE && (*Mask&7U)!=7U)return CR_NOT_READY;
  if(R->Routes[1].State==CR_MDSS_NATIVE_LIVE && (*Mask&0x78U)!=0x78U)return CR_NOT_READY;
  return CR_SUCCESS;
}
/* Verify only owned selectors. The witness contains retained native demands
   and exact selector/divider values, not fabricated Windows references. */
STATIC CR_STATUS NativeConfig(CR_CLOCK_EXTERNAL *E,CONST CR_MDSS_NATIVE_HANDOFF *R,UINT32 Mask) {
  UINTN B=E->Core->ClockControllers[CC_TYPE_DISP_CC].Address;
  for(UINT32 I=7;I<12;++I)if(Mask&BIT(I)) {
    CONST CR_MDSS_NATIVE_ROUTE *Q=&R->Routes[I<9?0:1];
    UINT32 Select,Divider,M=0,N=0,D=0,Width=E->NativeNode[I]->MNDWidth;
    if(I==9){Select=0;Divider=2;}
    else if(I==7 || I==10){Select=Q->SourceSelect;Divider=Q->SourceDivider;}
    else {Select=Q->PixelSource;Divider=Q->PixelDivider;M=Q->PixelM;N=Q->PixelN;D=Q->PixelD;}
    if(Select>7 || !Divider || Divider>32 || (I==7 && Select!=2) ||
       (I==8 && Select!=1) || (I==10 && Select!=1) || (I==11 && Select!=2))return CR_UNSUPPORTED;
    UINT32 MndMask=Width?((1U<<Width)-1U):0;
    if(M>MndMask || N>MndMask || D>MndMask || (!!M!=!!N) || (M&&(!D||M>N||D!=N)))return CR_UNSUPPORTED;
    UINTN A=B+mNativeRegisters[I];UINT32 Config=CrMmioRead32(A+4);
    UINT32 Expected=(Select<<8)|(Divider-1)|(M?2U<<12:0U);
    if((Config&0x371fU)!=Expected || (Config&BIT(20)) ||
       (CrMmioRead32(A)&BIT(0)))return CR_NOT_READY;
    if(M && ((CrMmioRead32(A+8)&MndMask)!=M ||
        (CrMmioRead32(A+12)&MndMask)!=((~(N-M))&MndMask) ||
        (CrMmioRead32(A+16)&MndMask)!=((~D)&MndMask)))return CR_NOT_READY;
  }
  if((Mask&BIT(1)) && (!R->Routes[0].InterfaceDivider || R->Routes[0].InterfaceDivider>16 ||
      (CrMmioRead32(B+0x814c)&15U)!=R->Routes[0].InterfaceDivider-1))return CR_NOT_READY;
  if((Mask&BIT(5)) && (!R->Routes[1].InterfaceDivider || R->Routes[1].InterfaceDivider>16 ||
      (CrMmioRead32(B+0x81b4)&15U)!=R->Routes[1].InterfaceDivider-1))return CR_NOT_READY;
  return CR_SUCCESS;
}
STATIC CR_STATUS NativeOff(CR_CLOCK_EXTERNAL *E,UINT32 I) {
  UINTN A=E->Core->ClockControllers[CC_TYPE_DISP_CC].Address+mNativeRegisters[I];
  UINT32 Enable=I<7?BIT(0):BIT(1);
  /* Journal before the first effect; never restore a firmware-on baseline. */
  E->NativeTouched|=BIT(I);
  CrLockAcquire(&E->Core->Lock);
  CrMmioUpdateBits32(A,Enable,0);CrMemoryFence();
  CrLockRelease(&E->Core->Lock);
  for(UINT32 Try=0;Try<200;++Try) {
    UINT32 Value=CrMmioRead32(A);
    if(!(Value&Enable) && (Value&BIT(31))) {E->NativeOff|=BIT(I);return CR_SUCCESS;}
    cr_sleep(1);
  }
  return CR_TIMEOUT;
}
CR_STATUS CrClockExternalNativeRetire(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 Id,
    CONST CR_MDSS_NATIVE_HANDOFF *R) {
  CR_STATUS S=Owner(E,O);if(S!=CR_SUCCESS)return S;
  if(!R || !Id || Id!=R->TransferId)return CR_INVALID_PARAMETER;
  if(E->NativeOwner) {
    if(E->NativeOwner!=O)return CR_BUSY;
    if(!SameBytes(R,&E->NativeRecord,sizeof(*R)))return CR_INVALID_PARAMETER;
    /* A completed transfer is one-shot, including after Windows re-enables
       the same group. A retry must not retire the new Windows references. */
    if(E->NativeComplete)return CR_SUCCESS;
  }
  if(E->Enabled || E->RecoveryPending || E->SourceSaved[0] || E->SourceSaved[1] ||
      E->DividerSaved || E->Held[0] || E->Held[1] || E->Held[2] ||
      E->SecondEnabled || E->SecondRecoveryPending || E->SecondDividerSaved ||
      E->SecondSourceSaved[0] || E->SecondSourceSaved[1] ||
      E->SecondHeld[0] || E->SecondHeld[1] || E->SecondHeld[2])return CR_BUSY;
  UINT32 Mask;S=NativeMask(R,&Mask);if(S!=CR_SUCCESS)return S;
  if(!Accessible(E))return CR_NOT_READY;
  for(UINT32 I=0;I<12;++I) {
    ClockNode *N=E->NativeNode[I];
    if(!N || !Fits(N->ParentController,mNativeRegisters[I]+(I>=7?16U:0U)))return CR_NOT_READY;
    if((Mask&BIT(I)) && (N->ReferenceCount || N->ReleasePending || N->ExternalConfigPending ||
        N->ParentsHeld || N->RestoreControlEnabled))return CR_BUSY;
  }
  /* Qualify all selectors before the first irreversible gate write. An
     unclaimed sibling cannot lose its shared private parent. */
  S=NativeConfig(E,R,Mask);if(S!=CR_SUCCESS)return S;
  for(UINT32 I=0;I<7;++I)if(!(Mask&BIT(I)) && (Mask&BIT(mNativeParents[I]))) {
    ClockNode *N=E->NativeNode[I];
    if(N->ReferenceCount || N->ReleasePending ||
        (CrMmioRead32(N->ParentController->Address+N->EnableRegister)&1U))return CR_BUSY;
  }
  if(!E->NativeOwner) {
    E->NativeRecord=*R;E->NativeOwner=O;E->NativeMask=Mask;
  }
  /* Revalidate completed bits on partial retry; do not skip a re-enabled
     gate and do not proceed to parent retirement after an uncertain leaf. */
  for(UINT32 I=0;I<12;++I)if(Mask&BIT(I)) {
    S=NativeOff(E,I);if(S!=CR_SUCCESS)return S;
  }
  E->NativeComplete=TRUE;return CR_SUCCESS;
}

STATIC BOOLEAN AccessAncestor(ClockNode *Root,ClockNode *N,UINT32 Depth,UINT32 *Budget) {
  if(!Root || Depth==32 || !*Budget || Root->ParentCount>32 ||
      (Root->ParentCount&&!Root->Parents))return TRUE;
  --*Budget;if(Root==N)return TRUE;
  for(UINTN I=0;I<Root->ParentCount;++I)if(AccessAncestor(Root->Parents[I],N,Depth+1,Budget))return TRUE;
  return FALSE;
}
CR_STATUS CrClockExternalLegacyGuard(CR_CLOCK_EXTERNAL *E,ClockNode *N) {
  CR_STATUS S=CrClockExternalGuard(E,N);if(S!=CR_SUCCESS)return S;
  if(!E->Owner || !CrClockExternalNeedsAccess(E))return CR_SUCCESS;
  if(N==E->Domain)return CR_BUSY;
  UINT32 Budget=512;
  for(UINT32 I=0;I<3;++I)if(AccessAncestor(E->Access[I],N,0,&Budget))return CR_BUSY;
  return CR_SUCCESS;
}
