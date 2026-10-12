/* Typed Qualcomm SCM ownership core. SPDX-License-Identifier: MIT */
#include <Library/ScmCrLib.h>
#include "ScmPas.h"
#include <oskal/cr_string.h>
#include <oskal/cr_atomic.h>

STATIC volatile UINT32 mProviderClaim;
STATIC CR_SCM_CORE *mProvider;

/* ScmPas.c wraps the unchanged PAS helper with private names and routes its
   wire calls here. The real assembly keeps its original name. Observe WAITQ
   returns inside the existing bounded Probe/Drain loops as well. */
VOID CrScmPasWire(CR_PIL_SMC_ARGS *A) {
  CR_SCM_CORE *P=mProvider;
  if (P==NULL || !P->Initialized || P->Quarantined) {
    A->X[0]=CR_MAX_UINT64;return;
  }
  CrPilArm64Smccc(A);
  if (A->X[0]!=0 && A->X[0]!=1 && (INT32)A->X[0]>=0) {
    P->UncertainResult=*A;P->UncertainOriginal=P->Pas.Original;P->UncertainRole=CR_SCM_ROLE_PAS1;P->Quarantined=TRUE;
  }
}

STATIC CR_STATUS Final(CR_STATUS S) {return S==CR_SUCCESS || CR_ERROR(S) ? S : CR_DEVICE_ERROR;}
STATIC BOOLEAN Separate(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn) {
  UINTN X=(UINTN)A,Y=(UINTN)B;
  return A!=NULL && B!=NULL && An<=(~(UINTN)0)-X && Bn<=(~(UINTN)0)-Y &&
    (X+An<=Y || Y+Bn<=X);
}
STATIC CR_STATUS Begin(CR_SCM_CORE *P) {
  if (P==NULL || ((UINTN)P%_Alignof(CR_SCM_CORE))!=0 ||
      CrMutexCheckLevel()!=CR_SUCCESS || !P->Initialized) return CR_INVALID_PARAMETER;
  return Final(CrMutexAcquire(&P->Lock));
}
STATIC CR_STATUS End(CR_SCM_CORE *P,CR_STATUS S) {CrMutexRelease(&P->Lock);return Final(S);}
STATIC CR_SCM_OWNER *Find(CR_SCM_CORE *P,CONST CR_SCM_LEASE *L) {
  if (!Separate(P,sizeof(*P),L,sizeof(*L)) || ((UINTN)L%_Alignof(CR_SCM_LEASE))!=0 ||
      L->Provider!=P || L->Owner==NULL || L->Generation==0 ||
      L->Role<1 || L->Role>CR_SCM_ROLE_COUNT) return NULL;
  CR_SCM_OWNER *O=&P->Owners[L->Role-1];
  return O->Identity==L && O->Lease.Owner==L->Owner && O->Lease.Generation==L->Generation ? O : NULL;
}
STATIC CR_STATUS Validate(CR_SCM_CORE *P,CR_SCM_OWNER *O,CONST CR_PIL_SMC_ARGS *A) {
  if (P->Quarantined || !O->Acquired) return CR_NOT_READY;
  return Final(P->Authority.Validate(P->Authority.Context,O->Lease.Role,O->Lease.Owner,
                                     O->Grant,A,O->GpuBank));
}
STATIC CR_STATUS OtherPending(CR_SCM_CORE *P,UINT32 Role) {
  if (P->Quarantined ||
      (Role!=CR_SCM_ROLE_QTEE && P->Owners[CR_SCM_ROLE_QTEE-1].Lease.Owner!=NULL) ||
      (Role!=CR_SCM_ROLE_PAS1 && P->Pas.Interrupted) ||
      (Role!=CR_SCM_ROLE_GPU && P->Gpu.Pending))
    return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrScmInitialize(CR_SCM_CORE *P,CONST CR_SCM_AUTHORITY *A) {
  if (P==NULL || ((UINTN)P%_Alignof(CR_SCM_CORE))!=0 ||
      CrMutexCheckLevel()!=CR_SUCCESS || !Separate(P,sizeof(*P),A,sizeof(*A)) ||
      A->Acquire==NULL || A->Validate==NULL || A->Release==NULL) return CR_INVALID_PARAMETER;
  if (P->Initialized) return CR_ALREADY_STARTED;
  /* NextGeneration deliberately survives complete stop/start. */
  if (P->Owners[0].Lease.Owner!=NULL || P->Owners[1].Lease.Owner!=NULL ||
      P->Owners[2].Lease.Owner!=NULL || P->Quarantined)
    return CR_NOT_READY;
  if ((CrAtomicOr32(&mProviderClaim,1)&1U)!=0) return CR_ALREADY_STARTED;
  mProvider=P;P->Authority=*A;
  P->PasTarget=(CR_PIL_TARGET){.PasId=1,.MemoryBase=0x85e00000U,.MemorySize=0x02100000U};
  CrMutexInit(&P->Lock);P->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrScmUninitialize(CR_SCM_CORE *P) {
  CR_STATUS S=Begin(P);if (S!=CR_SUCCESS) return S;
  if (P->Owners[0].Lease.Owner!=NULL || P->Owners[1].Lease.Owner!=NULL ||
      P->Owners[2].Lease.Owner!=NULL || P->Qtee.Wire.Pending || P->Qtee.MaybeOwned ||
      P->PasMaybeOwned || P->Pas.Interrupted || P->Gpu.Pending || P->Quarantined)
    return End(P,CR_NOT_READY);
  cr_memset(&P->Authority,0,sizeof(P->Authority));
  cr_memset(&P->QteeAuthority,0,sizeof(P->QteeAuthority));P->Initialized=FALSE;
  mProvider=NULL;CrAtomicAnd32(&mProviderClaim,~1U);
  return End(P,CR_SUCCESS);
}
CR_STATUS CrScmQuarantine(CR_SCM_CORE *P) {
  CR_STATUS S=Begin(P);if (S!=CR_SUCCESS) return S;
  P->Quarantined=TRUE;return End(P,CR_SUCCESS);
}
CR_STATUS CrScmAcquire(CR_SCM_CORE *P,UINT32 Role,VOID *Owner,CR_SCM_LEASE *L) {
  CR_STATUS S=Begin(P);if (S!=CR_SUCCESS) return S;
  if (Owner==NULL || Role<1 || Role>CR_SCM_ROLE_COUNT ||
      !Separate(P,sizeof(*P),L,sizeof(*L)) || ((UINTN)L%_Alignof(CR_SCM_LEASE))!=0 ||
      L->Provider!=NULL || L->Owner!=NULL || L->Generation!=0 || L->Role!=0)
    return End(P,CR_INVALID_PARAMETER);
  if (P->Quarantined) return End(P,CR_NOT_READY);
  if(Role==CR_SCM_ROLE_QTEE || P->Owners[CR_SCM_ROLE_QTEE-1].Lease.Owner) {
    S=OtherPending(P,Role);if(S!=CR_SUCCESS)return End(P,S);
  }
  if(Role==CR_SCM_ROLE_QTEE && !P->QteeAuthority.Describe)return End(P,CR_UNSUPPORTED);
  CR_SCM_OWNER *O=&P->Owners[Role-1];
  if (O->Lease.Owner!=NULL) return End(P,CR_BUSY);
  if (P->NextGeneration==CR_MAX_UINT64) return End(P,CR_OUT_OF_RESOURCES);
  O->Lease=(CR_SCM_LEASE){P,Owner,++P->NextGeneration,Role};O->Identity=L;*L=O->Lease;
  if (Role==CR_SCM_ROLE_PAS1) {
    S=CrScmPrivatePasInitialize(&P->Pas,&P->PasTarget);
    if (S!=CR_SUCCESS) return End(P,S);
  }
  S=Final(P->Authority.Acquire(P->Authority.Context,Role,Owner,&O->Grant,&O->GpuBank));
  if (S!=CR_SUCCESS) return End(P,S);
  if (O->Grant==NULL || (Role==CR_SCM_ROLE_GPU && O->GpuBank>255)) return End(P,CR_COMPROMISED_DATA);
  if(Role==CR_SCM_ROLE_QTEE) {
    CR_SCM_QTEE_MEMORY M={0};
    S=Final(P->QteeAuthority.Describe(P->QteeAuthority.Context,Owner,O->Grant,&M));
    if(S!=CR_SUCCESS)return End(P,S);
    if(M.Bytes!=8192 || M.Profile!=1 || !M.Physical || (M.Physical&4095) ||
        M.Physical>CR_MAX_UINT64-8192)return End(P,CR_UNSUPPORTED);
    P->Qtee.Memory=M;
  }
  O->Acquired=TRUE;return End(P,CR_SUCCESS);
}
CR_STATUS CrScmRelease(CR_SCM_CORE *P,CR_SCM_LEASE *L) {
  CR_STATUS S=Begin(P);if (S!=CR_SUCCESS) return S;
  CR_SCM_OWNER *O=Find(P,L);if (O==NULL) return End(P,CR_INVALID_PARAMETER);
  if (P->Quarantined || (L->Role==CR_SCM_ROLE_PAS1 ?
      (P->Pas.Interrupted || P->PasMaybeOwned) : L->Role==CR_SCM_ROLE_GPU ?
      P->Gpu.Pending : (P->Qtee.Wire.Pending || P->Qtee.MaybeOwned))) return End(P,CR_NOT_READY);
  /* Closing is one-way even if the underlying release fails or is nonfinal. */
  O->Acquired=FALSE;
  if (O->Grant!=NULL) {
    S=Final(P->Authority.Release(P->Authority.Context,L->Role,L->Owner,O->Grant));
    if (S!=CR_SUCCESS) return End(P,S);
    O->Grant=NULL;
  }
  if (L->Role==CR_SCM_ROLE_PAS1 && P->Pas.Initialized) {
    S=CrScmPrivatePasUninitialize(&P->Pas);if (S!=CR_SUCCESS) return End(P,S);
  }
  if (L->Role==CR_SCM_ROLE_GPU) cr_memset(&P->Gpu,0,sizeof(P->Gpu));
  if (L->Role==CR_SCM_ROLE_QTEE) cr_memset(&P->Qtee,0,sizeof(P->Qtee));
  cr_memset(O,0,sizeof(*O));cr_memset(L,0,sizeof(*L));return End(P,CR_SUCCESS);
}
CR_STATUS CrScmPasProbe(CR_SCM_CORE *P,CONST CR_SCM_LEASE *L) {
  CR_STATUS S=Begin(P);if (S!=CR_SUCCESS) return S;
  CR_SCM_OWNER *O=Find(P,L);
  if (O==NULL || L->Role!=CR_SCM_ROLE_PAS1) return End(P,CR_INVALID_PARAMETER);
  S=OtherPending(P,L->Role);if (S==CR_SUCCESS) S=Validate(P,O,NULL);
  if (S==CR_SUCCESS) S=CrScmPrivatePasProbe(&P->Pas);
  if (P->Quarantined) S=CR_NOT_READY;
  return End(P,S);
}
CR_STATUS CrScmPasCall(CR_SCM_CORE *P,CONST CR_SCM_LEASE *L,CR_PIL_SMC_ARGS *A) {
  CR_STATUS S=Begin(P);if (S!=CR_SUCCESS) return S;
  CR_SCM_OWNER *O=Find(P,L);
  if (O==NULL || L->Role!=CR_SCM_ROLE_PAS1 ||
      !Separate(P,sizeof(*P),A,sizeof(*A)) || !Separate(L,sizeof(*L),A,sizeof(*A)) ||
      ((UINTN)A%_Alignof(CR_PIL_SMC_ARGS))!=0) return End(P,CR_INVALID_PARAMETER);
  CR_PIL_SMC_ARGS Original=A->X[0]==1 ? P->Pas.Original : *A;
  UINT64 Fid=Original.X[0];
  BOOLEAN Pas=(Fid==0x42000201U || Fid==0x02000201U || Fid==0x42000202U ||
    Fid==0x02000202U || Fid==0x42000205U || Fid==0x02000205U);
  BOOLEAN Shutdown=(Fid==0x42000206U || Fid==0x02000206U);
  if ((Pas || Shutdown) && (!P->Pas.Qualified || (Fid&0xff000000U)!=P->Pas.SmcBase))
    return End(P,CR_UNSUPPORTED);
  S=OtherPending(P,L->Role);if (S==CR_SUCCESS) S=Validate(P,O,&Original);
  if (S!=CR_SUCCESS) return End(P,S);
  S=CrScmPrivatePasCall(&P->Pas,A);
  if (Pas && cr_memcmp(&P->Pas.Original,&Original,sizeof(Original))==0) P->PasMaybeOwned=TRUE;
  if (S==CR_SUCCESS && Shutdown && A->X[0]==0 && A->X[1]==0) P->PasMaybeOwned=FALSE;
  /* 2/3 are SCM WAITQ states. There is no IRQ/waitqueue provider yet, and a
     completed instruction cannot establish that firmware has retired the call. */
  if (S==CR_SUCCESS && A->X[0]!=0 && A->X[0]!=1 && (INT32)A->X[0]>=0) P->Quarantined=TRUE;
  if (P->Quarantined) S=CR_NOT_READY;
  return End(P,S);
}
CR_STATUS CrScmPasSnapshot(CR_SCM_CORE *P,CONST CR_SCM_LEASE *L,CR_SCM_PAS_STATE *Out) {
  CR_STATUS S=Begin(P);if (S!=CR_SUCCESS) return S;
  CR_SCM_OWNER *O=Find(P,L);
  if (O==NULL || L->Role!=CR_SCM_ROLE_PAS1 || !Separate(P,sizeof(*P),Out,sizeof(*Out)) ||
      !Separate(L,sizeof(*L),Out,sizeof(*Out)) || ((UINTN)Out%_Alignof(CR_SCM_PAS_STATE))!=0)
    return End(P,CR_INVALID_PARAMETER);
  *Out=(CR_SCM_PAS_STATE){P->Pas.Original,P->Pas.ResumeCookie,P->Pas.PasSupportResult,
    P->Pas.ProbeStep,P->Pas.SmcBase,P->Pas.Interrupted,P->Pas.Qualified,
    P->Pas.PasSupportKnown,P->PasMaybeOwned,P->Quarantined,P->UncertainResult,P->UncertainOriginal};
  return End(P,CR_SUCCESS);
}
STATIC CR_STATUS GpuFrame(CR_SCM_CORE *P,CR_SCM_OWNER *O,CONST CR_PIL_SMC_ARGS *Frame,
                          CR_PIL_SMC_ARGS *Result) {
  CR_SCM_GPU_STATE *G=&P->Gpu;
  if (G->Pending && cr_memcmp(Frame,&G->Original,sizeof(*Frame))!=0) return CR_NOT_READY;
  if (!G->Pending) G->Original=*Frame;
  for (UINT32 I=0;I<CR_SCM_RESUME_BUDGET;++I) {
    CR_STATUS S=Validate(P,O,&G->Original);if (S!=CR_SUCCESS) return S;
    CR_PIL_SMC_ARGS A=G->Original;
    if (G->Pending) {A.X[0]=1;A.X[6]=G->Cookie;}
    CrPilArm64Smccc(&A);
    if ((INT32)A.X[0]==1) {G->Pending=TRUE;G->Cookie=A.X[6];continue;}
    G->Pending=FALSE;G->Cookie=0;*Result=A;
    if ((INT32)A.X[0]>1) {
      P->UncertainResult=A;P->UncertainOriginal=G->Original;P->UncertainRole=CR_SCM_ROLE_GPU;P->Quarantined=TRUE;return CR_NOT_READY;
    }
    return CR_SUCCESS;
  }
  return CR_TIMEOUT;
}
CR_STATUS CrScmGpuAperture(CR_SCM_CORE *P,CONST CR_SCM_LEASE *L) {
  CR_STATUS S=Begin(P);if (S!=CR_SUCCESS) return S;
  CR_SCM_OWNER *O=Find(P,L);
  if (O==NULL || L->Role!=CR_SCM_ROLE_GPU) return End(P,CR_INVALID_PARAMETER);
  S=OtherPending(P,L->Role);if (S==CR_SUCCESS) S=Validate(P,O,NULL);
  if (S!=CR_SUCCESS) return End(P,S);
  if (CrPilArm64CurrentEl()!=1) return End(P,CR_UNSUPPORTED);
  CR_SCM_GPU_STATE *G=&P->Gpu;
  while (!G->Ready) {
    CR_PIL_SMC_ARGS F={{0}},R={{0}};
    F.X[1]=1;
    if (G->Step<2) {F.X[0]=G->Step==0 ? 0xc2000601U : 0x82000601U;F.X[2]=0x02000601U;}
    else {F.X[0]=G->Base|0x601U;F.X[2]=0x02000c1bU;}
    S=GpuFrame(P,O,&F,&R);if (S!=CR_SUCCESS) return End(P,S);
    if (G->Step<2) {
      if (R.X[0]==0 && R.X[1]==1) {G->Base=G->Step==0 ? 0x42000000U : 0x02000000U;G->Step=2;}
      else if (G->Step==0) G->Step=1;
      else {G->Step=0;return End(P,CR_UNSUPPORTED);}
    } else {
      if (R.X[0]!=0 || R.X[1]==0) return End(P,CR_UNSUPPORTED);
      G->Ready=TRUE;
    }
  }
  CR_PIL_SMC_ARGS F={{G->Base|0xc1bU,4,0xffff0000U|O->GpuBank,
                       0xffffffffU,0xffffffffU,0xffffffffU,0,0}},R={{0}};
  S=GpuFrame(P,O,&F,&R);if (S!=CR_SUCCESS) return End(P,S);
  if (R.X[0]==0) return End(P,CR_SUCCESS);
  return End(P,(INT32)R.X[0]==-12 ? CR_BUSY : CR_DEVICE_ERROR);
}

CR_STATUS CrScmConfigureQtee(CR_SCM_CORE *P,CONST CR_SCM_QTEE_AUTHORITY *A) {
  CR_STATUS S=Begin(P);if(S!=CR_SUCCESS)return S;
  if(!Separate(P,sizeof(*P),A,sizeof(*A)) || !A->Context || !A->Describe)
    return End(P,CR_INVALID_PARAMETER);
  if(P->Quarantined || P->QteeAuthority.Describe || P->Owners[2].Lease.Owner)
    return End(P,CR_NOT_READY);
  P->QteeAuthority=*A;return End(P,CR_SUCCESS);
}
STATIC CR_STATUS QteeValid(CR_SCM_CORE *P,CR_SCM_OWNER *O,CONST CR_PIL_SMC_ARGS *F) {
  CR_STATUS S=Validate(P,O,F);if(S!=CR_SUCCESS)return S;
  CR_SCM_QTEE_MEMORY M={0};
  S=Final(P->QteeAuthority.Describe(P->QteeAuthority.Context,O->Lease.Owner,O->Grant,&M));
  if(S!=CR_SUCCESS)return S;
  if(M.Physical!=P->Qtee.Memory.Physical || M.Bytes!=8192 || M.Profile!=1)
    return CR_COMPROMISED_DATA;
  return CR_SUCCESS;
}
STATIC CR_STATUS QteeFrame(CR_SCM_CORE *P,CR_SCM_OWNER *O,
    CONST CR_PIL_SMC_ARGS *F,CR_PIL_SMC_ARGS *Out) {
  CR_SCM_GPU_STATE *W=&P->Qtee.Wire;
  if(W->Pending && cr_memcmp(F,&W->Original,sizeof(*F)))return CR_NOT_READY;
  if(!W->Pending)W->Original=*F;
  for(UINT32 I=0;I<CR_SCM_RESUME_BUDGET;++I) {
    CR_STATUS S=QteeValid(P,O,&W->Original);if(S!=CR_SUCCESS)return S;
    CR_PIL_SMC_ARGS A=W->Original;
    if(W->Pending){A.X[0]=1;A.X[6]=W->Cookie;}
    CrPilArm64Smccc(&A);*Out=A;
    if(A.X[0]==1){W->Pending=TRUE;W->Cookie=A.X[6];continue;}
    W->Pending=FALSE;W->Cookie=0;
    if(A.X[0]!=0 && (INT32)A.X[0]>=0) {
      P->UncertainOriginal=W->Original;P->UncertainResult=A;
      P->UncertainRole=CR_SCM_ROLE_QTEE;P->Quarantined=TRUE;return CR_NOT_READY;
    }
    return CR_SUCCESS;
  }
  return CR_TIMEOUT;
}
CR_STATUS CrScmQteeCall(CR_SCM_CORE *P,CONST CR_SCM_LEASE *L,
    CR_SCM_QTEE_OPERATION Op,CR_PIL_SMC_ARGS *Out) {
  CR_STATUS S=Begin(P);if(S!=CR_SUCCESS)return S;
  CR_SCM_OWNER *O=Find(P,L);
  if(!O || L->Role!=CR_SCM_ROLE_QTEE || Op>CrScmQteeDelete || (INT32)Op<0 ||
      !Separate(P,sizeof(*P),Out,sizeof(*Out)) || !Separate(L,sizeof(*L),Out,sizeof(*Out)) ||
      ((UINTN)Out%_Alignof(CR_PIL_SMC_ARGS)))return End(P,CR_INVALID_PARAMETER);
  cr_memset(Out,0,sizeof(*Out));
  S=OtherPending(P,L->Role);if(S==CR_SUCCESS)S=QteeValid(P,O,NULL);
  if(S!=CR_SUCCESS)return End(P,S);
  CR_SCM_QTEE_STATE *Q=&P->Qtee;
  if(CrPilArm64CurrentEl()!=1){
    /* A fresh unsupported execution convention issued no secure request.
       Existing continuations/bridge ownership still forbid local cleanup. */
    if(!Q->Wire.Ready&&!Q->Wire.Pending&&!Q->Enabled&&!Q->MaybeOwned)
      Q->CapabilityRejected=TRUE;
    return End(P,CR_UNSUPPORTED);
  }
  /* Availability encodes owner/service/command without the calling convention.
     Profile1 supports only the actual SMC64 convention, never a 32-bit fallback. */
  STATIC CONST UINT32 Calls[]={0x02000c1cU,0x02000c1eU,0x32000602U,0x02000c1dU};
  while(!Q->Wire.Ready) {
    CR_PIL_SMC_ARGS F={{0}},R={{0}};F.X[1]=1;
    F.X[0]=Q->Wire.Step==0 ? 0xc2000601U : 0x42000601U;
    F.X[2]=Q->Wire.Step==0 ? 0x02000601U : Calls[Q->Wire.Step-1];
    S=QteeFrame(P,O,&F,&R);*Out=R;if(S!=CR_SUCCESS)return End(P,S);
    if(R.X[0]!=0 || R.X[1]!=1){Q->CapabilityRejected=TRUE;return End(P,CR_UNSUPPORTED);}
    if(++Q->Wire.Step==5)Q->Wire.Ready=TRUE;
  }
  CR_PIL_SMC_ARGS F={{0}};
  switch(Op) {
  case CrScmQteeEnable:
    if(Q->Enabled || Q->MaybeOwned)return End(P,CR_ALREADY_STARTED);
    F.X[0]=0x42000c1cU;break;
  case CrScmQteeCreate:
    if(!Q->Enabled || (Q->MaybeOwned && !Q->Wire.Pending))return End(P,CR_NOT_READY);
    F.X[0]=0x42000c1eU;F.X[1]=4;
    F.X[2]=Q->Memory.Physical|6;F.X[3]=Q->Memory.Physical|6;
    F.X[4]=8192U|(1U<<9);F.X[5]=3;break;
  case CrScmQteeInvoke:
    if(!Q->MaybeOwned || !Q->Created)return End(P,CR_NOT_READY);
    F.X[0]=0x72000602U;F.X[1]=0x224;
    F.X[2]=Q->Memory.Physical;F.X[3]=4096;
    F.X[4]=Q->Memory.Physical+4096;F.X[5]=4096;break;
  case CrScmQteeDelete:
    if(!Q->MaybeOwned || !Q->Created)return End(P,CR_NOT_READY);
    F.X[0]=0x42000c1dU;F.X[1]=1;F.X[2]=Q->BridgeHandle;break;
  default:return End(P,CR_INVALID_PARAMETER);
  }
  /* Even a rejected resume operation must not replace the retained original. */
  if(Q->Wire.Pending && cr_memcmp(&F,&Q->Wire.Original,sizeof(F)))return End(P,CR_NOT_READY);
  if(Op==CrScmQteeCreate)Q->MaybeOwned=TRUE;
  S=QteeFrame(P,O,&F,Out);if(S!=CR_SUCCESS)return End(P,S);
  if(Out->X[0]==0) {
    if(Op==CrScmQteeEnable && Out->X[1]==0)Q->Enabled=TRUE;
    if(Op==CrScmQteeCreate) {
      if(Out->X[1]==0){Q->BridgeHandle=Out->X[2];Q->Created=TRUE;}
      else if(Out->X[2]==0)Q->MaybeOwned=FALSE;
      else {P->UncertainOriginal=F;P->UncertainResult=*Out;
        P->UncertainRole=CR_SCM_ROLE_QTEE;P->Quarantined=TRUE;return End(P,CR_COMPROMISED_DATA);}
    }
    /* Android delete wrapper returns only transport; x1 is not an ack code. */
    if(Op==CrScmQteeDelete){Q->MaybeOwned=FALSE;Q->Created=FALSE;Q->BridgeHandle=0;}
  }
  return End(P,CR_SUCCESS);
}

CR_STATUS CrScmQteeSnapshot(CR_SCM_CORE *P,CONST CR_SCM_LEASE *L,CR_SCM_QTEE_SNAPSHOT *Out) {
  CR_STATUS S=Begin(P);if(S!=CR_SUCCESS)return S;
  CR_SCM_OWNER *O=Find(P,L);
  if(!O || L->Role!=CR_SCM_ROLE_QTEE || !Separate(Out,sizeof(*Out),P,sizeof(*P)) ||
     !Separate(Out,sizeof(*Out),L,sizeof(*L)))return End(P,CR_INVALID_PARAMETER);
  *Out=(CR_SCM_QTEE_SNAPSHOT){P->Qtee.CapabilityRejected && !P->Qtee.Wire.Ready &&
    !P->Qtee.Enabled && !P->Qtee.Created,P->Qtee.Wire.Pending,P->Qtee.MaybeOwned,P->Quarantined};
  return End(P,CR_SUCCESS);
}
