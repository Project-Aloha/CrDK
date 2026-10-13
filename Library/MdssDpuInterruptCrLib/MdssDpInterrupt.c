/* Owned SM8450 DP AUX packed status/mask/ack policy. SPDX-License-Identifier: MIT */
#include <Library/MdssDpInterruptCrLib.h>

static CR_STATUS Exact(CR_STATUS S) { return S == CR_SUCCESS || CR_ERROR(S) ? S : CR_NOT_READY; }
static VOID Zero(VOID *P, UINTN N) { UINT8 *B = P; while (N--) *B++ = 0; }
static BOOLEAN Overlap(CONST VOID *A,UINTN N,CONST VOID *B,UINTN M) {
 UINTN X=(UINTN)A,Y=(UINTN)B;
 return !A||!B||!N||!M||X>~(UINTN)0-N||Y>~(UINTN)0-M||(X<Y+M&&Y<X+N);
}
static BOOLEAN Output(CR_MDSS_DP_IRQ_CORE *C,CONST VOID *P,UINTN N) {
 return P&&!Overlap(C,sizeof(*C),P,N)&&(!C->Identity||!Overlap(C->Identity,sizeof(*C->Identity),P,N))&&(!C->Link.Identity||!Overlap(C->Link.Identity,sizeof(*C->Link.Identity),P,N));
}
static BOOLEAN Valid(CONST CR_MDSS_DP_IRQ_CORE *C) {
  return C && C->Initialized && C->Io.Context && C->Io.Read32 && C->Io.Write32 && C->Io.Barrier;
}
static CR_STATUS Fail(CR_MDSS_DP_IRQ_CORE *C, CR_STATUS S) {
  C->Faulted = TRUE; C->LastError = Exact(S); return C->LastError;
}
static CR_STATUS Read(CR_MDSS_DP_IRQ_CORE *C, CR_MDSS_IRQ_WINDOW W, UINT32 O, UINT32 *V) {
  return Exact(C->Io.Read32(C->Io.Context, W, O, V));
}
static BOOLEAN Match(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *O, CONST CR_MDSS_DP_IRQ_TOKEN *T) {
  return Valid(C) && O && T && C->Identity == T && C->Owner == O &&
    T->Provider == C && T->Incarnation == C->Incarnation && T->Serial == C->Serial;
}
static CR_STATUS Set(CR_MDSS_DP_IRQ_CORE *C, BOOLEAN Enable, BOOLEAN Ack) {
  UINT32 V, R; CR_STATUS S = Read(C, CrMdssIrqDpAhb, 0x20, &V);
  if (S != CR_SUCCESS) return Fail(C, S);
  /* HPD belongs to a future separately registered child. Never acknowledge it
     or silently preserve an enabled unowned HPD interrupt. */
  if (V & (1U << 2)) return Fail(C, CR_NOT_READY);
  UINT32 Write = (Enable ? CR_MDSS_DP_AUX_IRQ_MASK : 0) |
    (Ack ? (V & CR_MDSS_DP_AUX_IRQ_STATUS) << 1 : 0);
  S = Exact(C->Io.Write32(C->Io.Context, CrMdssIrqDpAhb, 0x20, Write));
  if (S != CR_SUCCESS) return Fail(C, S);
  S = Exact(C->Io.Barrier(C->Io.Context)); if (S != CR_SUCCESS) return Fail(C, S);
  S = Read(C, CrMdssIrqDpAhb, 0x20, &R); if (S != CR_SUCCESS) return Fail(C, S);
  if ((R & CR_MDSS_DP_AUX_IRQ_MASK) != (Enable ? CR_MDSS_DP_AUX_IRQ_MASK : 0) ||
      (Ack && (R & CR_MDSS_DP_AUX_IRQ_STATUS))) return Fail(C, CR_NOT_READY);
  return CR_SUCCESS;
}
CR_STATUS CrMdssDpIrqInit(CR_MDSS_DP_IRQ_CORE *C, CONST CR_MDSS_IRQ_IO *I, UINT64 N) {
  if (!C || !I || !I->Context || !I->Read32 || !I->Write32 || !I->Barrier || !N) return CR_INVALID_PARAMETER;
  if (C->Initialized || C->Identity) return CR_BUSY;
  if (Overlap(C,sizeof(*C),I,sizeof(*I))) return CR_INVALID_PARAMETER;
  CR_MDSS_IRQ_IO Copy = *I; Zero(C, sizeof(*C)); C->Io = Copy; C->Incarnation = N; C->Initialized = TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrMdssDpIrqRegister(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *O, CR_MDSS_DP_IRQ_TOKEN *T, BOOLEAN *Owned) {
  UINT32 V;
  if (!Valid(C) || !O || !T || !Owned || Overlap(C,sizeof(*C),T,sizeof(*T)) ||
      Overlap(C,sizeof(*C),Owned,sizeof(*Owned)) || Overlap(T,sizeof(*T),Owned,sizeof(*Owned))) return CR_INVALID_PARAMETER;
  *Owned = FALSE;
  if (C->Identity) { if (!Match(C, O, T)) return CR_BUSY; *Owned = TRUE; return C->Closed || C->Faulted ? CR_NOT_READY : CR_SUCCESS; }
  if (C->Closed || C->Faulted) return CR_NOT_READY;
  if (T->Provider || T->Incarnation || T->Serial) return CR_INVALID_PARAMETER;
  CR_STATUS S = Read(C, CrMdssIrqDpAhb, 0x20, &V); if (S != CR_SUCCESS) return S;
  if (V & (CR_MDSS_DP_AUX_IRQ_MASK | (1U << 2))) return CR_BUSY;
  if (C->Serial == CR_MAX_UINT64) return CR_OUT_OF_RESOURCES;
  C->Identity = T; C->Owner = O; ++C->Serial;
  *T = (CR_MDSS_DP_IRQ_TOKEN){C, C->Incarnation, C->Serial}; *Owned = TRUE; return CR_SUCCESS;
}
CR_STATUS CrMdssDpIrqArm(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *O, CR_MDSS_DP_IRQ_TOKEN *T,
  CONST VOID *Request, UINT64 *Generation) {
  if (!Match(C, O, T) || !Request || !Output(C,Generation,sizeof(*Generation)) ||
      Overlap(Generation,sizeof(*Generation),Request,1)) return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted) return CR_NOT_READY;
  if (C->Request || C->Armed || C->Pending) return CR_BUSY;
  if (C->Generation == CR_MAX_UINT64) return CR_OUT_OF_RESOURCES;
  CR_STATUS S = Set(C, FALSE, TRUE); if (S != CR_SUCCESS) return S;
  C->Request = Request; ++C->Generation; C->Armed = TRUE; C->Status = 0;
  S = Set(C, TRUE, FALSE); if (S == CR_SUCCESS) *Generation = C->Generation; return S;
}
CR_STATUS CrMdssDpIrqCapture(CR_MDSS_DP_IRQ_CORE *C, BOOLEAN *Handled, BOOLEAN *Pending) {
  UINT32 V, Observed, Parent;
  if (!Valid(C) || !Output(C,Handled,sizeof(*Handled)) || !Output(C,Pending,sizeof(*Pending)) ||
      Overlap(Handled,sizeof(*Handled),Pending,sizeof(*Pending))) return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted) return CR_NOT_READY;
  CR_STATUS S = Read(C, CrMdssIrqParent, CR_MDSS_IRQ_PARENT_STATUS, &Parent); if (S != CR_SUCCESS) return Fail(C, S);
  if (!(Parent & CR_MDSS_IRQ_PARENT_DP)) { *Handled = FALSE; *Pending = FALSE; return CR_SUCCESS; }
  S = Read(C, CrMdssIrqDpAhb, 0x20, &V); if (S != CR_SUCCESS) return Fail(C, S);
  if (!(V & CR_MDSS_DP_AUX_IRQ_STATUS)) { *Handled=FALSE; *Pending=FALSE; return CR_SUCCESS; }
  if (!C->Identity || !C->Armed || !C->Request || (V & (1U << 2)) ||
      (V & CR_MDSS_DP_AUX_IRQ_MASK) != CR_MDSS_DP_AUX_IRQ_MASK) return Fail(C, CR_NOT_READY);
  Observed = V & CR_MDSS_DP_AUX_IRQ_STATUS;
  if (!Observed || C->Pending) return Fail(C, CR_NOT_READY);
  if (C->Sequence == CR_MAX_UINT64) return Fail(C, CR_OUT_OF_RESOURCES);
  S = Set(C, FALSE, TRUE); if (S != CR_SUCCESS) return S;
  C->Status = Observed; ++C->Sequence; C->Armed = FALSE; C->Pending = TRUE;
  *Handled = TRUE; *Pending = TRUE; return CR_SUCCESS;
}
CR_STATUS CrMdssDpIrqTake(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *O, CR_MDSS_DP_IRQ_TOKEN *T,
  CR_MDSS_DP_IRQ_EVENT *Out) {
  if (!Match(C, O, T) || !Output(C,Out,sizeof(*Out)) || (C->Request&&Overlap(C->Request,1,Out,sizeof(*Out)))) return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted) return CR_NOT_READY;
  if (!C->Pending) return CR_NOT_FOUND;
  *Out = (CR_MDSS_DP_IRQ_EVENT){T, O, C->Request, C->Serial, C->Generation, C->Sequence, C->Status};
  C->Pending = FALSE; return CR_SUCCESS;
}
CR_STATUS CrMdssDpIrqDisarm(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *O, CR_MDSS_DP_IRQ_TOKEN *T) {
  if (!Match(C, O, T)) return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted) return CR_NOT_READY;
  CR_STATUS S = Set(C, FALSE, TRUE); if (S == CR_SUCCESS) C->Armed = FALSE; return S;
}
CR_STATUS CrMdssDpIrqDiscard(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *O, CR_MDSS_DP_IRQ_TOKEN *T) {
  if (!Match(C, O, T)) return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted) return CR_NOT_READY;
  if (C->Armed) return CR_BUSY;
  C->Pending = FALSE; C->Status = 0; return CR_SUCCESS;
}
CR_STATUS CrMdssDpIrqRetireRequest(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *O,
  CR_MDSS_DP_IRQ_TOKEN *T, CONST VOID *Request) {
  if (!Match(C, O, T) || !Request || C->Request != Request) return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted) return CR_NOT_READY;
  if (C->Armed || C->Pending) return CR_BUSY;
  C->Request = NULL; return CR_SUCCESS;
}
CR_STATUS CrMdssDpIrqUnregister(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *O, CR_MDSS_DP_IRQ_TOKEN *T) {
  if (!Match(C, O, T)) return CR_INVALID_PARAMETER;
  if (C->Request || C->Armed || C->Pending) return CR_BUSY;
  CR_STATUS S = CrMdssDpIrqDisarm(C, O, T); if (S != CR_SUCCESS) return S;
  C->Identity = NULL; C->Owner = NULL; Zero(T, sizeof(*T)); return CR_SUCCESS;
}
VOID CrMdssDpIrqQuarantine(CR_MDSS_DP_IRQ_CORE *C) { if (Valid(C)) { C->Closed = TRUE; C->LastError = CR_NOT_READY; } }
CR_STATUS CrMdssDpIrqUninit(CR_MDSS_DP_IRQ_CORE *C) {
  if (!Valid(C)) return CR_INVALID_PARAMETER;
  if (C->Identity || C->Request || C->Link.Identity || C->Link.Request) return CR_BUSY;
  Zero(C, sizeof(*C)); return CR_SUCCESS;
}
