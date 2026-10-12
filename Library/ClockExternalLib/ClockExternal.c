/* DSI0 ClockLib-owned source/divider configuration and retained rollback. MIT */
#include <Library/ClockExternalLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>
#include <oskal/cr_debug.h>
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
typedef struct {
  UINT32 Soc, Gdsc;
  UINT32 Gates[2][3], Sources[2][2], Divider[2], DividerWidth;
  UINT32 Select[2][2]; /* byte, pixel; provider-owned PHY outputs */
} EXTERNAL_PROFILE;
/* Register-independent ClockLib graph admission facts. Public references:
   OnePlus SM8250 cad09e061ef6cf689a4a1e54d27562e0f042236a dispcc-kona.c;
   OnePlus SM8350 13ca08fdf0979fdd61d5e8991661874bb2d19150 dispcc-lahaina.c.
   SM8250 mainline dispcc-sm8250.c also describes a read-only /2 divider.
   The existing SM8450 native/DP inventory below retains its original ABI. */
STATIC CONST EXTERNAL_PROFILE mProfiles[]={
  {CR_CLOCK_EXTERNAL_SOC_SM8250,0x3000,
    {{0x2028,0x202c,0x2004},{0x2030,0x2034,0x2008}},
    {{0x2110,0x2098},{0x212c,0x20b0}},{0x2128,0x2144},2,{{1,1},{2,2}}},
  {CR_CLOCK_EXTERNAL_SOC_SM8350,0x3000,
    {{0x2028,0x202c,0x2004},{0x2030,0x2034,0x2008}},
    {{0x210c,0x2094},{0x2128,0x20ac}},{0x2124,0x2140},4,{{1,1},{2,2}}},
  {CR_CLOCK_EXTERNAL_SOC_SM8450,0x9000,
    {{0x8028,0x802c,0x8004},{0x8030,0x8034,0x8008}},
    {{0x8134,0x80bc},{0x8150,0x80d4}},{0x814c,0x8168},4,{{2,1},{4,3}}}
};
STATIC CONST EXTERNAL_PROFILE *Profile(UINT32 Soc) {
  for(UINT32 I=0;I<ARRAY_SIZE(mProfiles);++I)if(mProfiles[I].Soc==Soc)return &mProfiles[I];
  return NULL;
}
STATIC BOOLEAN Fits(ClockController *C,UINT32 O) {
  return C && C->MemMapped && C->Address && !(C->Address&3U) && C->Size>=4 &&
    O<=C->Size-4 && C->Address<=(UINTN)-1-C->Size;
}
STATIC CR_STATUS Check(CR_CLOCK_EXTERNAL *E) {
  if (CR_ERROR(CrMutexCheckLevel()) || !E || !E->Initialized || !E->Core || !E->Core->LockInitialized || !E->Core->ClockControllers ||
      E->Core->ClockControllerCount<=CC_TYPE_DISP_CC || E->Core->ClockControllerCount>CC_TYPE_MAX)
    return CR_INVALID_PARAMETER;
  ClockController *C=&E->Core->ClockControllers[CC_TYPE_DISP_CC];
  CONST EXTERNAL_PROFILE *P=Profile(E->Soc);if(!P)return CR_UNSUPPORTED;
  if (!Fits(C,P->Gdsc) || !Fits(C,E->Leaf[1]->DividerRegister)) return CR_NOT_READY;
  for(UINT32 I=0;I<2;++I) if (!Fits(C,E->Source[I]->CmdRegister+E->Source[I]->CmdRCfgOffset+0x10))
    return CR_NOT_READY;
  if(E->SecondPresent) {
    if(!Fits(C,E->SecondLeaf[1]->DividerRegister))return CR_NOT_READY;
    for(UINT32 I=0;I<2;++I)
      if(!E->SecondSource[I] || !Fits(C,E->SecondSource[I]->CmdRegister+0x10))return CR_NOT_READY;
  }
  return CR_SUCCESS;
}
STATIC CR_STATUS Owner(CR_CLOCK_EXTERNAL *E,VOID *O) {
  CR_STATUS S=Check(E); if(CR_ERROR(S))return S;
  return O && O==E->Owner ? CR_SUCCESS : CR_BUSY;
}
CR_STATUS CrClockExternalInit(CR_CLOCK_EXTERNAL *E,ClockDriverContext *C) {
  return CrClockExternalInitForSoc(E,C,CR_CLOCK_EXTERNAL_SOC_SM8450);
}
STATIC CR_STATUS AdmitGroup(CR_CLOCK_EXTERNAL *E,CONST EXTERNAL_PROFILE *P,UINT32 I) {
  STATIC CONST CHAR8 *Names[2][5]={
    {"disp_cc_mdss_byte0_clk","disp_cc_mdss_byte0_intf_clk","disp_cc_mdss_pclk0_clk",
      "disp_cc_mdss_byte0_clk_src","disp_cc_mdss_pclk0_clk_src"},
    {"disp_cc_mdss_byte1_clk","disp_cc_mdss_byte1_intf_clk","disp_cc_mdss_pclk1_clk",
      "disp_cc_mdss_byte1_clk_src","disp_cc_mdss_pclk1_clk_src"}};
  ClockDriverContext *C=E->Core;ClockController *Cc=&C->ClockControllers[CC_TYPE_DISP_CC];
  ClockNode *Nodes[5]={0};UINT32 Found=0;CR_STATUS Missing=CR_SUCCESS;
  for(UINT32 J=0;J<5;++J) {
    ClockControllerType T;CR_STATUS S=GetClockNode(C,Names[I][J],&T,NULL,&Nodes[J]);
    if(S==CR_NOT_FOUND){Missing=S;continue;}if(S!=CR_SUCCESS)return S;
    if(T!=CC_TYPE_DISP_CC || Nodes[J]->ParentController!=Cc)return CR_UNSUPPORTED;
    ++Found;
  }
  if(I&&!Found)return CR_SUCCESS; /* wholly absent optional group */
  if(Missing!=CR_SUCCESS)return Missing;
  for(UINT32 J=0;J<3;++J) {
    ClockNode *N=Nodes[J];
    if(N->Type!=CLOCK_NODE_TYPE_BRANCH_2 || N->ParentCount!=1 || !N->Parents ||
       N->Parents[0]!=Nodes[J==2?4:3] || N->EnableRegister!=P->Gates[I][J] ||
       N->HaltRegister!=P->Gates[I][J] || N->EnableMsk!=1 || N->HwClockCtrl ||
       N->HaltCheckFlag!=CLOCK_NODE_BRANCH_HALT || !Fits(Cc,N->EnableRegister))return CR_UNSUPPORTED;
  }
  ClockNode *D=Nodes[1];
  if(D->DividerRegister!=P->Divider[I] || D->DividerWidth!=P->DividerWidth ||
     D->DividerReadOnly>1 || (D->DividerReadOnly&&P->Soc!=CR_CLOCK_EXTERNAL_SOC_SM8250) ||
     !Fits(Cc,D->DividerRegister))return CR_UNSUPPORTED;
  for(UINT32 J=0;J<2;++J) {
    ClockNode *N=Nodes[J+3];
    if(N->Type!=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 || N->CmdRegister!=P->Sources[I][J] ||
       N->CmdRCfgOffset || N->HIDWidth!=5 || N->MNDWidth!=(J?8U:0U) ||
       !Fits(Cc,N->CmdRegister+0x10))return CR_UNSUPPORTED;
  }
  ClockNode **L=I?E->SecondLeaf:E->Leaf,**S=I?E->SecondSource:E->Source;
  for(UINT32 J=0;J<3;++J)L[J]=Nodes[J];
  for(UINT32 J=0;J<2;++J)S[J]=Nodes[J+3];
  if(I)E->SecondPresent=TRUE;return CR_SUCCESS;
}
CR_STATUS CrClockExternalInitForSoc(CR_CLOCK_EXTERNAL *E,ClockDriverContext *C,UINT32 Soc) {
  if(CR_ERROR(CrMutexCheckLevel()) || !E || !C || !C->LockInitialized || !C->ClockControllers ||
     C->ClockControllerCount<=CC_TYPE_DISP_CC || C->ClockControllerCount>CC_TYPE_MAX)
    return CR_INVALID_PARAMETER;
  if(E->Initialized)return CR_ALREADY_STARTED;
  CONST EXTERNAL_PROFILE *P=Profile(Soc);if(!P)return CR_UNSUPPORTED;
  CR_CLOCK_EXTERNAL Candidate={0};Candidate.Core=C;Candidate.Soc=Soc;
  CR_STATUS S=AdmitGroup(&Candidate,P,0);if(S!=CR_SUCCESS)return S;
  S=AdmitGroup(&Candidate,P,1);if(S!=CR_SUCCESS)return S;
  CONST CHAR8 *Access[]={"gcc_disp_ahb_clk","gcc_disp_xo_clk","disp_cc_mdss_ahb_clk"};
  for(UINT32 I=0;I<3;++I) {
    CR_STATUS S=GetClockNode(C,Access[I],NULL,NULL,&Candidate.Access[I]);if(CR_ERROR(S))return S;
  }
  S=GetGdscNode(C,"disp_cc_mdss_core_gdsc",&Candidate.Domain);if(CR_ERROR(S))return S;
  if(Candidate.Domain->Type!=CLOCK_NODE_TYPE_GDSC ||
     Candidate.Domain->ParentController!=&C->ClockControllers[CC_TYPE_DISP_CC] ||
     Candidate.Domain->GdscRegister!=P->Gdsc || !Fits(Candidate.Domain->ParentController,P->Gdsc))return CR_UNSUPPORTED;
  /* The old wire record names an exact SM8450 DSI/DP resource inventory.
     Other admitted graphs cannot consume that native ownership contract. */
  if(Soc==CR_CLOCK_EXTERNAL_SOC_SM8450) {
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
  }
  Candidate.Initialized=TRUE;S=Check(&Candidate);if(CR_ERROR(S))return S;
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
  /* The early boot reset owns the complete DISPCC mutation interval.
     Access prerequisites use the lease's direct ClockLib path. */
  if((E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8250 || E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8350) &&
      E->BootGeneration && !E->FunctionalComplete &&
      N->ParentController==&E->Core->ClockControllers[CC_TYPE_DISP_CC])return CR_BUSY;
  if(E->FunctionalPll.Pending &&
      N->ParentController==&E->Core->ClockControllers[CC_TYPE_DISP_CC])return CR_BUSY;
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
STATIC CR_STATUS CurrentDivider(CR_CLOCK_EXTERNAL *E,GROUP *G,UINT32 Divider) {
  ClockNode *D=G->Leaf[1];if(!D->DividerReadOnly)return CR_SUCCESS;
  if(!Accessible(E))return CR_NOT_READY;
  CrLockAcquire(&E->Core->Lock);
  UINT32 Actual=(CrMmioRead32(D->ParentController->Address+D->DividerRegister)&
    ((1U<<D->DividerWidth)-1U))+1U;
  CrLockRelease(&E->Core->Lock);
  return Actual==Divider?CR_SUCCESS:CR_UNSUPPORTED;
}
CR_STATUS CrClockExternalGetPlan(CR_CLOCK_EXTERNAL *E,VOID *O,UINT32 I,
    CR_CLOCK_DSI_PLAN *P,BOOLEAN *On,BOOLEAN *Recovery) {
  if(!P || !On || !Recovery || I>1)return CR_INVALID_PARAMETER;
  CR_STATUS S=Owner(E,O);if(S!=CR_SUCCESS)return S;
  if((I && !E->SecondPresent) || !E->PlanValid[I])return CR_NOT_READY;
  GROUP G=Group(E,I);
  if(*G.Enabled) {S=CurrentDivider(E,&G,E->Plans[I].ByteInterfaceDivider);if(S!=CR_SUCCESS)return S;}
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
    if(*G.Enabled && !*G.RecoveryPending) {
      if(Pending(&G))return CR_NOT_READY;
      return CurrentDivider(E,&G,Plan->ByteInterfaceDivider);
    }
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
    /* A RO divisor is a current hardware contract, never permission to
       write the divider. ClockLib's existing API only verifies that value. */
    if(G.Leaf[1]->DividerReadOnly) {
      S=Exact(ClockSetDivider(E->Core,G.Leaf[1],Plan->ByteInterfaceDivider));
      if(S!=CR_SUCCESS)return S;
    }
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
      CONST EXTERNAL_PROFILE *P=Profile(E->Soc);UINT32 Select=P->Select[I][J];
      S=Exact(ClockSelectExternalSource(E->Core,G.Leaf[J==0?0:2],
          Select,2,M,N,N));if(S!=CR_SUCCESS)return S;
      UINTN A=Config(&G,J);UINT32 Expected=Select<<8|1U|(M?0x2000U:0U);
      CrLockAcquire(&E->Core->Lock);
      BOOLEAN Match=(CrMmioRead32(A)&0x371fU)==Expected;
      if(M)Match=Match && (CrMmioRead32(A+4)&255U)==M &&
        (CrMmioRead32(A+8)&255U)==((~(N-M))&255U) &&
        (CrMmioRead32(A+12)&255U)==((~N)&255U);
      CrLockRelease(&E->Core->Lock);
      if(!Match)return CR_DEVICE_ERROR;
    }
    ClockNode *D=G.Leaf[1];
    if(!D->DividerReadOnly) {
      *G.SavedDivider=CrMmioRead32(D->ParentController->Address+D->DividerRegister)&((1U<<D->DividerWidth)-1U);
      *G.DividerSaved=TRUE;
    }
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
  CONST EXTERNAL_PROFILE *P=Profile(E->Soc);BOOLEAN Allowed=Offset==P->Gdsc;
  for(UINT32 I=0;I<(E->SecondPresent?2U:1U);++I)Allowed=Allowed ||
    Offset==P->Sources[I][0]+4 || Offset==P->Sources[I][1]+4 || Offset==P->Divider[I];
  if(!Allowed)return CR_UNSUPPORTED;
  if(!Accessible(E))return CR_NOT_READY;
  *Value=CrMmioRead32(E->Core->ClockControllers[CC_TYPE_DISP_CC].Address+Offset);return CR_SUCCESS;
}
CR_STATUS CrClockExternalReleaseOwner(CR_CLOCK_EXTERNAL *E,VOID *O) {
  CR_STATUS S=Owner(E,O);if(CR_ERROR(S))return S;
  S=CrClockExternalReleaseFunctionalPll(E,O);if(S!=CR_SUCCESS)return S;
  if(E->SecondPresent) {S=CrClockExternalSetPlan(E,O,1,NULL,FALSE);if(S!=CR_SUCCESS)return S;}
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
  if(E->Soc!=CR_CLOCK_EXTERNAL_SOC_SM8450)return CR_UNSUPPORTED;
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

STATIC ClockNode *InheritedDsiNode(CR_CLOCK_EXTERNAL *E,UINT32 I) {
  if(I<6)return I<3?E->Leaf[I]:E->SecondLeaf[I-3];
  return I<8?E->Source[I-6]:E->SecondSource[I-8];
}
STATIC BOOLEAN AccessAncestor(ClockNode *,ClockNode *,UINT32,UINT32 *);
STATIC CR_STATUS BootAdmission(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 G) {
  CR_STATUS S=Owner(E,O);if(S!=CR_SUCCESS)return S;
  if(E->Soc!=CR_CLOCK_EXTERNAL_SOC_SM8250 && E->Soc!=CR_CLOCK_EXTERNAL_SOC_SM8350)
    return CR_UNSUPPORTED;
  if(!G || (E->BootGeneration && E->BootGeneration!=G))return CR_INVALID_PARAMETER;
  if(!Accessible(E))return CR_NOT_READY;
  if(!Fits(&E->Core->ClockControllers[CC_TYPE_DISP_CC],0x2000))return CR_NOT_READY;
  return CR_SUCCESS;
}
STATIC BOOLEAN BootIdle(CR_CLOCK_EXTERNAL *E) {
  ClockController *C=&E->Core->ClockControllers[CC_TYPE_DISP_CC];
  /* ABL hardware requests have no synthetic native references. Any live
     native client outside our access chain makes destructive reset unsafe. */
  for(UINTN I=0;I<C->ClkCount;++I) {
    ClockNode *N=C->Clks[I];if(!N)continue;
    UINT32 Budget=512;
    if(AccessAncestor(E->Access[2],N,0,&Budget))continue;
    if(N->ReferenceCount || N->ReleasePending || N->ExternalConfigPending ||
       N->ParentsHeld || N->RestoreControlEnabled)return FALSE;
  }
  return !E->Enabled && !E->SecondEnabled && !E->RecoveryPending && !E->SecondRecoveryPending;
}
CR_STATUS CrClockExternalResetCore(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 G,BOOLEAN Assert) {
  CR_STATUS S=BootAdmission(E,O,G);if(S!=CR_SUCCESS)return S;
  if(Assert>1)return CR_INVALID_PARAMETER;
  if(E->CoreResetComplete)return CR_SUCCESS;
  if(!Assert && !E->CoreResetTouched)return CR_NOT_READY;
  if(!BootIdle(E))return CR_BUSY;
  E->BootGeneration=G;E->CoreResetTouched=TRUE;
  /* Public dispcc-sm8250/8350 reset map: CORE_BCR +0x2000, bit 0.
     This operation bypasses no generic guard: it has an explicit owner. */
  UINTN A=E->Core->ClockControllers[CC_TYPE_DISP_CC].Address+0x2000;
  CrLockAcquire(&E->Core->Lock);
  CrMmioUpdateBits32(A,BIT(0),Assert?BIT(0):0);CrMemoryFence();
  UINT32 V=CrMmioRead32(A);CrLockRelease(&E->Core->Lock);
  if((V&BIT(0))!=(Assert?BIT(0):0))return CR_DEVICE_ERROR;
  if(!Assert)E->CoreResetComplete=TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrClockExternalCheckCoreResetReleased(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 G) {
  CR_STATUS S=BootAdmission(E,O,G);if(S!=CR_SUCCESS)return S;
  if(!E->CoreResetComplete)return CR_NOT_READY;
  UINTN A=E->Core->ClockControllers[CC_TYPE_DISP_CC].Address+0x2000;
  return CrMmioRead32(A)&BIT(0)?CR_NOT_READY:CR_SUCCESS;
}
CR_STATUS CrClockExternalRetireInheritedFunctional(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 G) {
  CR_STATUS S=CrClockExternalCheckCoreResetReleased(E,O,G);if(S!=CR_SUCCESS)return S;
  if(E->FunctionalComplete)return CR_SUCCESS;
  if(!BootIdle(E))return CR_BUSY;
  STATIC CONST CHAR8 *Names[]={"disp_cc_mdss_mdp_clk","disp_cc_mdss_mdp_lut_clk",
    "disp_cc_mdss_vsync_clk","disp_cc_mdss_esc0_clk","disp_cc_mdss_esc1_clk",
    "disp_cc_mdss_mdp_clk_src","disp_cc_mdss_vsync_clk_src",
    "disp_cc_mdss_esc0_clk_src","disp_cc_mdss_esc1_clk_src"};
  CONST UINT32 Offsets[]={0x200c,0x201c,0x2024,0x2038,0x203c,
    E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8250?0x20c8:0x20c4,
    E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8250?0x20f8:0x20f4,
    E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8250?0x2148:0x2144,
    E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8250?0x2160:0x215c};
  ClockNode *Nodes[9];ClockController *C=&E->Core->ClockControllers[CC_TYPE_DISP_CC];
  for(UINT32 I=0;I<9;++I) {
    S=GetClockNode(E->Core,Names[I],NULL,NULL,&Nodes[I]);if(S!=CR_SUCCESS)return S;
    ClockNode *N=Nodes[I];
    if(N->ParentController!=C || !Fits(C,Offsets[I]) ||
       (I<5?(N->Type!=CLOCK_NODE_TYPE_BRANCH_2 || N->EnableRegister!=Offsets[I] ||
          N->HaltRegister!=Offsets[I] || N->EnableMsk!=BIT(0) ||
          N->HaltCheckFlag!=(I==1?CLOCK_NODE_BRANCH_VOTED:CLOCK_NODE_BRANCH_HALT)):
         (N->Type!=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 || N->CmdRegister!=Offsets[I] ||
          N->CmdRCfgOffset || N->MNDWidth || N->HIDWidth!=5 ||
          N->HwClockCtrl!=(I==5))))return CR_UNSUPPORTED;
  }
  E->FunctionalTouched=TRUE;
  /* Stop leaves before withdrawing functional RCG force requests. MDP
     and LUT share one RCG; clear it once. Selectors/PLL analogue state
     and shared GCC stay intact. */
  for(UINT32 I=0;I<9;++I) {
    UINT32 Mask=I<5?BIT(0):BIT(1);UINTN A=C->Address+Offsets[I];
    CrLockAcquire(&E->Core->Lock);CrMmioUpdateBits32(A,Mask,0);CrMemoryFence();
    UINT32 V=CrMmioRead32(A);
    CrLockRelease(&E->Core->Lock);
    if(V&Mask){
      log_err("MDSS clock retirement " CR_LOG_CHAR8_STR_FMT
        " local request retained reg=0x%llx value=0x%08x mask=0x%08x",
        Names[I],(UINT64)A,V,Mask);
      return CR_DEVICE_ERROR;
    }
    /* Public clk-branch.c: withdrawing a voted request waits 10us, not
       for the global CLK_OFF bit. Other hardware votes may keep LUT ON.
       Still prove our own request cleared before retiring any RCG. */
    if(I<5 && (Nodes[I]->HaltCheckFlag&CLOCK_NODE_BRANCH_VOTED)){
      cr_sleep(10);V=CrMmioRead32(A);
      if(V&Mask){
        log_err("MDSS clock retirement " CR_LOG_CHAR8_STR_FMT
          " local vote reasserted reg=0x%llx value=0x%08x",
          Names[I],(UINT64)A,V);
        return CR_DEVICE_ERROR;
      }
      log_warn("MDSS clock retirement " CR_LOG_CHAR8_STR_FMT
        " local vote cleared reg=0x%llx value=0x%08x",
        Names[I],(UINT64)A,V);
      continue;
    }
    /* Public clk-rcg2 clear_force_enable withdraws ROOT_EN; it does not
       wait for aggregate ROOT_OFF. The admitted MDP source has a voted
       LUT consumer and hardware control, VSYNC also feeds RSCC, and the
       ESC sources rely on their already-halted branch feedback. Their
       local force and UPDATE must settle; another hardware request may
       keep the root ON. This is not the private DSI stopped-state proof. */
    for(UINT32 Retry=0;;++Retry) {
      V=CrMmioRead32(A);
      if(!(V&Mask) && (I<5?(V&BIT(31)):!(V&BIT(0))))break;
      if(Retry==199){
        log_err("MDSS clock retirement " CR_LOG_CHAR8_STR_FMT
          " timeout reg=0x%llx value=0x%08x request-mask=0x%08x",
          Names[I],(UINT64)A,V,Mask);
        return CR_TIMEOUT;
      }
      cr_sleep(1);
    }
    if(I>=5 && !(V&BIT(31)))log_warn("MDSS clock retirement " CR_LOG_CHAR8_STR_FMT
      " local force cleared reg=0x%llx value=0x%08x",
      Names[I],(UINT64)A,V);
  }
  E->FunctionalComplete=TRUE;return CR_SUCCESS;
}
STATIC CR_STATUS InheritedDsiAdmission(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 Generation) {
  CR_STATUS S=Owner(E,O);if(S!=CR_SUCCESS)return S;
  if(E->Soc!=CR_CLOCK_EXTERNAL_SOC_SM8250 && E->Soc!=CR_CLOCK_EXTERNAL_SOC_SM8350)
    return CR_UNSUPPORTED;
  if(!Generation)return CR_INVALID_PARAMETER;
  if(!E->SecondPresent)return CR_UNSUPPORTED;
  if(E->InheritedDsiOwner && (E->InheritedDsiOwner!=O ||
      E->InheritedDsiGeneration!=Generation))return CR_INVALID_PARAMETER;
  if(E->BootGeneration && E->BootGeneration!=Generation)return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}
STATIC BOOLEAN InheritedDsiIdle(CR_CLOCK_EXTERNAL *E) {
  if(E->Enabled || E->RecoveryPending || E->DividerSaved || E->SourceSaved[0] ||
      E->SourceSaved[1] || E->SecondEnabled || E->SecondRecoveryPending ||
      E->SecondDividerSaved || E->SecondSourceSaved[0] || E->SecondSourceSaved[1])return FALSE;
  for(UINT32 I=0;I<3;++I)if(E->Held[I] || E->SecondHeld[I])return FALSE;
  for(UINT32 I=0;I<10;++I) {
    ClockNode *N=InheritedDsiNode(E,I);
    if(N->ReferenceCount || N->ReleasePending || N->ExternalConfigPending ||
        N->ParentsHeld || N->RestoreControlEnabled)return FALSE;
  }
  return TRUE;
}
STATIC UINTN InheritedDsiAddress(CR_CLOCK_EXTERNAL *E,UINT32 I) {
  ClockNode *N=InheritedDsiNode(E,I);
  return N->ParentController->Address+(I<6?N->EnableRegister:N->CmdRegister);
}
CR_STATUS CrClockExternalCheckInheritedDsiOff(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 Generation) {
  CR_STATUS S=InheritedDsiAdmission(E,O,Generation);if(S!=CR_SUCCESS)return S;
  if(!E->InheritedDsiComplete || !Accessible(E))return CR_NOT_READY;
  if(!InheritedDsiIdle(E))return CR_BUSY;
  for(UINT32 I=0;I<10;++I) {
    UINT32 Enable=I<6?BIT(0):BIT(1),V=CrMmioRead32(InheritedDsiAddress(E,I));
    if((V&Enable) || !(V&BIT(31)) || (I>=6&&(V&BIT(0))))return CR_NOT_READY;
  }
  return CR_SUCCESS;
}
CR_STATUS CrClockExternalRetireInheritedDsi(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 Generation) {
  CR_STATUS S=InheritedDsiAdmission(E,O,Generation);if(S!=CR_SUCCESS)return S;
  if(E->InheritedDsiComplete)return CR_SUCCESS; /* never retire a new epoch */
  if(!Accessible(E))return CR_NOT_READY;
  if(!InheritedDsiIdle(E))return CR_BUSY;
  E->InheritedDsiOwner=O;E->InheritedDsiGeneration=Generation;
  /* Leaves precede their private parents. Journal before every attempted
     write; partial retries revalidate all acknowledgements. Selectors and
     dividers are left untouched, and firmware ON is never restored. */
  for(UINT32 I=0;I<10;++I) {
    UINT32 Enable=I<6?BIT(0):BIT(1);UINTN A=InheritedDsiAddress(E,I);
    E->InheritedDsiTouched|=BIT(I);E->InheritedDsiOff&=~BIT(I);
    CrLockAcquire(&E->Core->Lock);
    CrMmioUpdateBits32(A,Enable,0);CrMemoryFence();
    CrLockRelease(&E->Core->Lock);
    for(UINT32 Retry=0;;++Retry) {
      UINT32 V=CrMmioRead32(A);
      if(!(V&Enable) && (V&BIT(31)) && (I<6 || !(V&BIT(0)))) {
        E->InheritedDsiOff|=BIT(I);break;
      }
      if(Retry==199)return CR_TIMEOUT;
      cr_sleep(1);
    }
  }
  E->InheritedDsiComplete=TRUE;return CR_SUCCESS;
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

STATIC CR_INHERITED_PLL_DESCRIPTION FunctionalDescription(CR_CLOCK_EXTERNAL *E) {
  ClockController *C=&E->Core->ClockControllers[CC_TYPE_DISP_CC];
  return (CR_INHERITED_PLL_DESCRIPTION){.Base=C->Address+0x1000,.Size=C->Size-0x1000,
    .ExpectedL=31,.ExpectedAlpha=0x4000,.ParentHz=19200000,.FrequencyHz=600000000,
    .FsmInModeRegister=E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8250};
}
STATIC CR_STATUS FunctionalPllGraph(CR_CLOCK_EXTERNAL *E,ClockNode **Pll) {
  ClockController *C=&E->Core->ClockControllers[CC_TYPE_DISP_CC];
  CR_STATUS S=GetClockNode(E->Core,"disp_cc_pll1",NULL,NULL,Pll);if(S!=CR_SUCCESS)return S;
  ClockNode *N=*Pll;
  return Fits(C,0x1040) && N->ParentController==C && N->Type==CLOCK_NODE_TYPE_READ_ONLY_SOURCE &&
    N->FixedRateHz==600000000 && N->ReadOnlySourceCheck?CR_SUCCESS:CR_UNSUPPORTED;
}
STATIC CR_STATUS FunctionalQuiet(CR_CLOCK_EXTERNAL *E) {
  ClockNode *Pll;CR_STATUS S=FunctionalPllGraph(E,&Pll);if(S!=CR_SUCCESS)return S;
  if(Pll->ReferenceCount || Pll->ReleasePending || Pll->ParentsHeld ||
      Pll->RestoreControlEnabled || Pll->ExternalConfigPending){
    log_err("MDSS PLL1 quiet refused source references=%u pending=%u parents=%u",
      Pll->ReferenceCount,(UINT32)Pll->ReleasePending,(UINT32)Pll->ParentsHeld);
    return CR_BUSY;
  }
  ClockController *C=&E->Core->ClockControllers[CC_TYPE_DISP_CC];
  /* Public PLL1 consumers: AHB, MDP/LUT and ROT. The access clock remains
     running from XO; never configure the PLL while AHB consumes it. */
  ClockNode *Ahb=E->Access[2];
  if(Ahb->ParentCount!=1 || !Ahb->Parents || !Ahb->Parents[0] ||
      Ahb->Parents[0]->ParentController!=C ||
      Ahb->Parents[0]->Type!=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 ||
      Ahb->Parents[0]->CmdRegister!=0x22a0 || Ahb->ActiveRateHz!=19200000 ||
      !Fits(C,0x22a4))return CR_UNSUPPORTED;
  UINT32 AhbConfig=CrMmioRead32(C->Address+0x22a4),AhbCommand=CrMmioRead32(C->Address+0x22a0);
  if((AhbConfig&0x71f)!=1 || (AhbCommand&BIT(0))){
    log_err("MDSS PLL1 quiet refused AHB reg=0x%llx command=0x%08x config=0x%08x",
      (UINT64)C->Address+0x22a0,AhbCommand,AhbConfig);return CR_BUSY;
  }
  STATIC CONST CHAR8 *Names[]={"disp_cc_mdss_mdp_clk","disp_cc_mdss_mdp_lut_clk",
    "disp_cc_mdss_mdp_clk_src"};
  CONST UINT32 Offsets[]={0x200c,0x201c,0x20c4};
  for(UINT32 I=0;I<3;++I){ClockNode *N=NULL;
    S=GetClockNode(E->Core,Names[I],NULL,NULL,&N);if(S!=CR_SUCCESS)return S;
    if(N->ParentController!=C || !Fits(C,Offsets[I]+(I==2?4U:0U)) ||
        (I<2?(N->Type!=CLOCK_NODE_TYPE_BRANCH_2 || N->EnableRegister!=Offsets[I] ||
          N->HaltRegister!=Offsets[I] || N->EnableMsk!=1):
         (N->Type!=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 || N->CmdRegister!=Offsets[I])))return CR_UNSUPPORTED;
    if(N->ReferenceCount || N->ReleasePending || N->ParentsHeld || N->GateHeld ||
        N->ExternalConfigPending || N->RcgRestorePending || N->RestoreControlEnabled){
      log_err("MDSS PLL1 quiet refused " CR_LOG_CHAR8_STR_FMT " references=%u pending=%u parents=%u",
        Names[I],N->ReferenceCount,(UINT32)N->ReleasePending,(UINT32)N->ParentsHeld);return CR_BUSY;
    }
  }
  /* ROT is not exported by the selected graph. Admit its exact public
     physical layout read-only; no generic target is manufactured or stopped.
     Local-only LUT withdrawal cannot prove quiet for analogue ownership. */
  CONST UINT32 Gates[]={0x200c,0x201c,0x2014};
  STATIC CONST CHAR8 *GateNames[]={"MDP","LUT","ROT"};
  CONST UINT32 Roots[]={0x20c4,0x20dc};
  for(UINT32 I=0;I<ARRAY_SIZE(Gates);++I){
    if(!Fits(C,Gates[I]))return CR_UNSUPPORTED;
    UINT32 V=CrMmioRead32(C->Address+Gates[I]);
    if((V&1) || !(V&BIT(31))){
      log_err("MDSS PLL1 quiet refused " CR_LOG_CHAR8_STR_FMT " reg=0x%llx value=0x%08x",
        GateNames[I],(UINT64)C->Address+Gates[I],V);return CR_BUSY;
    }
  }
  for(UINT32 I=0;I<ARRAY_SIZE(Roots);++I){
    if(!Fits(C,Roots[I]+4))return CR_UNSUPPORTED;
    UINT32 V=CrMmioRead32(C->Address+Roots[I]);
    UINT32 Mux=(CrMmioRead32(C->Address+Roots[I]+4)>>8)&7;
    if((V&3) || (!(V&BIT(31)) && (Mux==4 || Mux==5))){
      log_err("MDSS PLL1 quiet refused root reg=0x%llx value=0x%08x mux=%u",
        (UINT64)C->Address+Roots[I],V,Mux);return CR_BUSY;
    }
  }
  return CR_SUCCESS;
}
STATIC CR_STATUS FunctionalReady(VOID *Context,BOOLEAN Quiet) {
  CR_CLOCK_EXTERNAL *E=Context;
  CR_STATUS S=CrClockExternalCheckCoreResetReleased(E,E->Owner,E->BootGeneration);
  if(S!=CR_SUCCESS)return S;
  if(!E->FunctionalComplete || !Accessible(E) || !E->FunctionalPllSupply ||
      !E->FunctionalPllSupplyContext)return CR_NOT_READY;
  S=Exact(E->FunctionalPllSupply(E->FunctionalPllSupplyContext));if(S!=CR_SUCCESS)return S;
  return Quiet?FunctionalQuiet(E):CR_SUCCESS;
}
CR_STATUS CrClockExternalCheckFunctionalPll(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 G) {
  CR_STATUS S=BootAdmission(E,O,G);if(S!=CR_SUCCESS)return S;
  S=FunctionalReady(E,FALSE);if(S!=CR_SUCCESS)return S;
  if(CrColdPllNeedsAccess(&E->FunctionalPll))return CrColdPllCheck(&E->FunctionalPll);
  if(!E->FunctionalPllInherited)return CR_NOT_READY;
  CR_INHERITED_PLL_DESCRIPTION D=FunctionalDescription(E);
  return CrInheritedPllCheck(&D,600000000);
}
CR_STATUS CrClockExternalPrepareFunctionalPll(CR_CLOCK_EXTERNAL *E,VOID *O,UINT64 G,
    CR_CLOCK_PLL_SUPPLY_CHECK Supply,VOID *Context) {
  CR_STATUS S=BootAdmission(E,O,G);if(S!=CR_SUCCESS)return S;
  if(!Supply || !Context)return CR_INVALID_PARAMETER;
  ClockNode *Pll;S=FunctionalPllGraph(E,&Pll);if(S!=CR_SUCCESS)return S;
  if(E->FunctionalPllSupply && (Supply!=E->FunctionalPllSupply ||
      Context!=E->FunctionalPllSupplyContext))return CR_INVALID_PARAMETER;
  E->FunctionalPllSupply=Supply;E->FunctionalPllSupplyContext=Context;
  S=FunctionalReady(E,FALSE);if(S!=CR_SUCCESS)return S;
  if(E->FunctionalPll.Ready || E->FunctionalPllInherited)
    return CrClockExternalCheckFunctionalPll(E,O,G); /* no live rewrite */
  if(E->FunctionalPll.Pending){
    S=CrColdPllRelease(&E->FunctionalPll);if(S!=CR_SUCCESS)return S;
  }
  CR_INHERITED_PLL_DESCRIPTION D=FunctionalDescription(E);
  if(!E->FunctionalPll.ColdClaimed){
    /* A qualified inherited source remains fully read-only. Failed live,
       foreign/FSM and mismatched configured state cannot authorize writes. */
    S=CrInheritedPllCheck(&D,600000000);
    if(S==CR_SUCCESS){E->FunctionalPllInherited=TRUE;return S;}
    if(E->Soc!=CR_CLOCK_EXTERNAL_SOC_SM8350)return S;
  }
  if(!E->FunctionalPll.Initialized){
    S=CrColdPllInit(&E->FunctionalPll,D.Base,D.Size,CR_COLD_PLL_SM8350_DISP1,FunctionalReady,E);
    if(S!=CR_SUCCESS)return S;
  }
  S=CrColdPllPrepare(&E->FunctionalPll);
  if(S==CR_SUCCESS)log_warn("MDSS PLL1 cold source ready rate=600000000");
  else log_err("MDSS PLL1 cold source refused status=0x%llx touched=%u pending=%u",
    (UINT64)S,(UINT32)E->FunctionalPll.Touched,(UINT32)E->FunctionalPll.Pending);
  return S;
}
CR_STATUS CrClockExternalReleaseFunctionalPll(CR_CLOCK_EXTERNAL *E,VOID *O) {
  CR_STATUS S=Owner(E,O);if(S!=CR_SUCCESS)return S;
  if(CrColdPllNeedsAccess(&E->FunctionalPll)){
    S=CrColdPllRelease(&E->FunctionalPll);if(S!=CR_SUCCESS)return S;
  }
  return CR_SUCCESS;
}
