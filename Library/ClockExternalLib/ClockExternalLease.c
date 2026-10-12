/* SPDX-License-Identifier: MIT */
#include <Library/ClockExternalLease.h>
#include <oskal/cr_mutex.h>

STATIC CR_STATUS Exact(CR_STATUS S) {
  return S == CR_SUCCESS ? S : CR_ERROR(S) ? S : CR_NOT_READY;
}
STATIC CR_STATUS Owner(CR_CLOCK_EXTERNAL_LEASE *L, VOID *O) {
  if (CR_ERROR(CrMutexCheckLevel()) || !L || !L->External.Initialized || !O)
    return CR_INVALID_PARAMETER;
  return L->External.Owner == O ? CR_SUCCESS : CR_BUSY;
}
STATIC BOOLEAN DomainBeforeAhb(CONST CR_CLOCK_EXTERNAL *E) {
  return E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8250 || E->Soc==CR_CLOCK_EXTERNAL_SOC_SM8350;
}
STATIC BOOLEAN Ready(CR_CLOCK_EXTERNAL_LEASE *L) {
  if (!L->Ready || L->CleanupPending || !L->DomainOwned ||
      !L->External.Domain->ReferenceCount || L->External.Domain->ReleasePending)
    return FALSE;
  for (UINT32 I=0; I<3; ++I)
    if (!L->AccessOwned[I] || !L->External.Access[I]->ReferenceCount ||
        L->External.Access[I]->ReleasePending) return FALSE;
  return TRUE;
}
CR_STATUS CrClockExternalLeaseInit(CR_CLOCK_EXTERNAL_LEASE *L, ClockDriverContext *C) {
  return CrClockExternalLeaseInitForSoc(L,C,CR_CLOCK_EXTERNAL_SOC_SM8450);
}
CR_STATUS CrClockExternalLeaseInitForSoc(CR_CLOCK_EXTERNAL_LEASE *L,ClockDriverContext *C,UINT32 Soc) {
  if (CR_ERROR(CrMutexCheckLevel()) || !L) return CR_INVALID_PARAMETER;
  /* Never erase retained state on repeated initialization. */
  if (L->Incarnation || L->Ready || L->CleanupPending || L->DomainOwned ||
      L->AccessOwned[0] || L->AccessOwned[1] || L->AccessOwned[2]) return CR_BUSY;
  return CrClockExternalInitForSoc(&L->External,C,Soc);
}
CR_STATUS CrClockExternalLeaseReserve(CR_CLOCK_EXTERNAL_LEASE *L, VOID *O, BOOLEAN *Owned) {
  if (!Owned) return CR_INVALID_PARAMETER;
  *Owned=FALSE;
  if (CR_ERROR(CrMutexCheckLevel()) || !L || !O || !L->External.Initialized)
    return CR_INVALID_PARAMETER;
  CR_CLOCK_EXTERNAL *E=&L->External;
  if (E->Owner) {
    if (E->Owner!=O) return CR_BUSY;
    *Owned=TRUE;
    return Ready(L) ? CR_SUCCESS : CR_NOT_READY;
  }
  if (L->Incarnation==CR_MAX_UINT64) return CR_OUT_OF_RESOURCES;
  CR_STATUS S=Exact(CrClockExternalReserve(E,O));
  if (S!=CR_SUCCESS) return S;
  ++L->Incarnation;
  *Owned=TRUE;
  L->CleanupPending=TRUE;
  /* MDSS genpd runtime resume powers the SM8250/8350 domain before the
     internal clock bulk. GCC fabric/XO make DISPCC power control accessible;
     a collapsed domain cannot acknowledge its child AHB. Keep the existing
     SM8450 access order. Every vote belongs only to this private lease. */
  UINT32 AccessCount=DomainBeforeAhb(E)?2:3;
  for (UINT32 I=0; I<AccessCount; ++I) {
    BOOLEAN Held=FALSE;
    S=Exact(ClockAcquire(E->Core,E->Access[I],I==2?19200000:0,&Held));
    L->AccessOwned[I]=Held;
    if (S!=CR_SUCCESS) return S;
    if (!Held) return CR_NOT_READY;
  }
  BOOLEAN Held=FALSE;
  S=Exact(ClockGdscAcquire(E->Core,E->Domain,&Held));
  L->DomainOwned=Held;
  if (S!=CR_SUCCESS) return S;
  if (!Held) return CR_NOT_READY;
  if(DomainBeforeAhb(E)){
    Held=FALSE;
    S=Exact(ClockAcquire(E->Core,E->Access[2],19200000,&Held));
    L->AccessOwned[2]=Held;
    if(S!=CR_SUCCESS)return S;
    if(!Held)return CR_NOT_READY;
  }
  L->CleanupPending=FALSE;
  L->Ready=TRUE;
  return Ready(L) ? CR_SUCCESS : CR_NOT_READY;
}
CR_STATUS CrClockExternalLeaseSetPlan(CR_CLOCK_EXTERNAL_LEASE *L, VOID *O, UINT32 I,
    CONST CR_CLOCK_DSI_PLAN *P, BOOLEAN On, BOOLEAN *Owned) {
  if (!Owned) return CR_INVALID_PARAMETER;
  *Owned=FALSE;
  CR_STATUS S=Owner(L,O); if(S!=CR_SUCCESS) return S;
  if (I>1 || On>1) return CR_INVALID_PARAMETER;
  *Owned=L->External.PlanValid[I];
  if (!Ready(L)) return CR_NOT_READY;
  S=Exact(CrClockExternalSetPlan(&L->External,O,I,P,On));
  *Owned=L->External.PlanValid[I];
  return S;
}
CR_STATUS CrClockExternalLeaseGetPlan(CR_CLOCK_EXTERNAL_LEASE *L, VOID *O, UINT32 I,
    CR_CLOCK_DSI_PLAN *P, BOOLEAN *On, BOOLEAN *Pending, UINT64 *Incarnation) {
  if (!P || !On || !Pending || !Incarnation) return CR_INVALID_PARAMETER;
  CR_STATUS S=Owner(L,O); if(S!=CR_SUCCESS) return S;
  S=Exact(CrClockExternalGetPlan(&L->External,O,I,P,On,Pending));
  if(S==CR_SUCCESS) {
    *Pending=*Pending || !Ready(L);
    *Incarnation=L->Incarnation;
  }
  return S;
}
CR_STATUS CrClockExternalLeaseRelease(CR_CLOCK_EXTERNAL_LEASE *L, VOID *O) {
  CR_STATUS S=Owner(L,O); if(S!=CR_SUCCESS) return S;
  CR_CLOCK_EXTERNAL *E=&L->External;
  /* Partial inherited retirement needs a still-ready access lease on retry.
     A refused Release cannot put that lease into cleanup-only state. */
  if(CrClockExternalNativePending(E))return CR_NOT_READY;
  /* Drain an uncertain or owned analogue source while this lease still
     proves live access/domain suppliers. Failure retains Ready for retry. */
  S=Exact(CrClockExternalReleaseFunctionalPll(E,O));if(S!=CR_SUCCESS)return S;
  L->Ready=FALSE;
  L->CleanupPending=TRUE;
  /* Do not relinquish the exclusive token until every prerequisite vote has
     retired. A failed group restore needs those exact votes for its retry. */
  for (UINT32 I=2; I-- >0;) {
    if (I && !E->SecondPresent) continue;
    S=Exact(CrClockExternalSetPlan(E,O,I,NULL,FALSE));
    if (S!=CR_SUCCESS) return S;
  }
  if (CrClockExternalNeedsAccess(E)) return CR_NOT_READY;
  /* Reverse the qualified power-on order. An uncertain child release must
     retain its domain and fabric suppliers for the next cleanup attempt. */
  if(DomainBeforeAhb(E) && L->AccessOwned[2]){
    S=Exact(ClockEnable(E->Core,E->Access[2],0,FALSE));
    if(S!=CR_SUCCESS)return S;
    L->AccessOwned[2]=FALSE;
  }
  if (L->DomainOwned) {
    S=Exact(ClockGdscSet(E->Core,E->Domain,FALSE));
    if (S!=CR_SUCCESS) return S;
    L->DomainOwned=FALSE;
  }
  for (UINT32 I=3; I-- >0;) if (L->AccessOwned[I]) {
    S=Exact(ClockEnable(E->Core,E->Access[I],0,FALSE));
    if (S!=CR_SUCCESS) return S;
    L->AccessOwned[I]=FALSE;
  }
  S=Exact(CrClockExternalReleaseOwner(E,O));
  if(S==CR_SUCCESS) L->CleanupPending=FALSE;
  return S;
}
STATIC BOOLEAN Ancestor(ClockNode *Root, ClockNode *N, UINT32 Depth, UINT32 *Budget) {
  if (!Root || Depth==32 || !*Budget || Root->ParentCount>32 ||
      (Root->ParentCount && !Root->Parents)) return TRUE;
  --*Budget;
  if (Root==N) return TRUE;
  for (UINTN I=0; I<Root->ParentCount; ++I)
    if (Ancestor(Root->Parents[I],N,Depth+1,Budget)) return TRUE;
  return FALSE;
}
CR_STATUS CrClockExternalLeaseRetireInheritedDsi(CR_CLOCK_EXTERNAL_LEASE *L,VOID *O,UINT64 Generation) {
  CR_STATUS S=Owner(L,O);if(S!=CR_SUCCESS)return S;
  if(!Ready(L))return CR_NOT_READY;
  return Exact(CrClockExternalRetireInheritedDsi(&L->External,O,Generation));
}
CR_STATUS CrClockExternalLeaseCheckInheritedDsiOff(CR_CLOCK_EXTERNAL_LEASE *L,VOID *O,UINT64 Generation) {
  CR_STATUS S=Owner(L,O);if(S!=CR_SUCCESS)return S;
  if(!Ready(L))return CR_NOT_READY;
  return Exact(CrClockExternalCheckInheritedDsiOff(&L->External,O,Generation));
}
CR_STATUS CrClockExternalLeaseGuard(CR_CLOCK_EXTERNAL_LEASE *L, ClockNode *N) {
  if (CR_ERROR(CrMutexCheckLevel()) || !L || !N) return CR_INVALID_PARAMETER;
  if (!L->External.Initialized || !L->External.Owner) return CR_SUCCESS;
  CR_STATUS S=CrClockExternalGuard(&L->External,N); if(S!=CR_SUCCESS) return S;
  if (N==L->External.Domain) return CR_BUSY;
  UINT32 Budget=512;
  for (UINT32 I=0; I<3; ++I)
    if (Ancestor(L->External.Access[I],N,0,&Budget)) return CR_BUSY;
  return CR_SUCCESS;
}
CR_STATUS CrClockExternalLeaseResetCore(CR_CLOCK_EXTERNAL_LEASE *L,VOID *O,UINT64 G,BOOLEAN A) {
  CR_STATUS S=Owner(L,O);if(S!=CR_SUCCESS)return S;
  if(!Ready(L))return CR_NOT_READY;
  return Exact(CrClockExternalResetCore(&L->External,O,G,A));
}
CR_STATUS CrClockExternalLeaseCheckCoreResetReleased(CR_CLOCK_EXTERNAL_LEASE *L,VOID *O,UINT64 G) {
  CR_STATUS S=Owner(L,O);if(S!=CR_SUCCESS)return S;
  if(!Ready(L))return CR_NOT_READY;
  return Exact(CrClockExternalCheckCoreResetReleased(&L->External,O,G));
}
CR_STATUS CrClockExternalLeaseRetireInheritedFunctional(CR_CLOCK_EXTERNAL_LEASE *L,VOID *O,UINT64 G) {
  CR_STATUS S=Owner(L,O);if(S!=CR_SUCCESS)return S;
  if(!Ready(L))return CR_NOT_READY;
  return Exact(CrClockExternalRetireInheritedFunctional(&L->External,O,G));
}
CR_STATUS CrClockExternalLeasePrepareFunctionalPll(CR_CLOCK_EXTERNAL_LEASE *L,VOID *O,UINT64 G,
    CR_CLOCK_PLL_SUPPLY_CHECK Supply,VOID *Context) {
  CR_STATUS S=Owner(L,O);if(S!=CR_SUCCESS)return S;
  if(!Ready(L))return CR_NOT_READY;
  return Exact(CrClockExternalPrepareFunctionalPll(&L->External,O,G,Supply,Context));
}
CR_STATUS CrClockExternalLeaseCheckFunctionalPll(CR_CLOCK_EXTERNAL_LEASE *L,VOID *O,UINT64 G) {
  CR_STATUS S=Owner(L,O);if(S!=CR_SUCCESS)return S;
  if(!Ready(L))return CR_NOT_READY;
  return Exact(CrClockExternalCheckFunctionalPll(&L->External,O,G));
}
