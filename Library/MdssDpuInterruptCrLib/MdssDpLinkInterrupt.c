/* Owned SM8450 READY/IDLE packed interrupt ledger. SPDX-License-Identifier: MIT
 * Register layout: Qualcomm dp_catalog.c and mainline msm/dp/dp_ctrl.c.
 * No producer register access, DMA fence or hardware readiness is granted. */
#include <Library/MdssDpInterruptCrLib.h>
#define FOREIGN_MASK ((1U << 8) | (1U << 11) | (1U << 17) | (1U << 20) | (1U << 24) | (1U << 27))
static CR_STATUS Exact(CR_STATUS S)
{
  return S == CR_SUCCESS || CR_ERROR(S) ? S : CR_NOT_READY;
}
static BOOLEAN Overlap(CONST VOID *A, UINTN N, CONST VOID *B, UINTN M)
{
  UINTN X = (UINTN)A, Y = (UINTN)B;
  return !A || !B || !N || !M || X > ~(UINTN)0 - N || Y > ~(UINTN)0 - M || (X < Y + M && Y < X + N);
}
static BOOLEAN Valid(CR_MDSS_DP_IRQ_CORE *C)
{
  return C && C->Initialized && C->Io.Context && C->Io.Read32 && C->Io.Write32 && C->Io.Barrier;
}
static BOOLEAN Output(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *P, UINTN N)
{
  return P && !Overlap(C, sizeof(*C), P, N) &&
         (!C->Identity || !Overlap(C->Identity, sizeof(*C->Identity), P, N)) &&
         (!C->Link.Identity || !Overlap(C->Link.Identity, sizeof(*C->Link.Identity), P, N));
}
static BOOLEAN Match(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *Owner, CONST CR_MDSS_DP_IRQ_TOKEN *T)
{
  return Valid(C) && Owner && T && C->Link.Identity == T && C->Link.Owner == Owner &&
         T->Provider == C && T->Incarnation == C->Incarnation && T->Serial == C->Link.Serial;
}
static CR_STATUS Fail(CR_MDSS_DP_IRQ_CORE *C, CR_STATUS S)
{
  C->Faulted = TRUE;
  C->LastError = Exact(S);
  return C->LastError;
}
static CR_STATUS Read(CR_MDSS_DP_IRQ_CORE *C, CR_MDSS_IRQ_WINDOW W, UINT32 Offset, UINT32 *V)
{
  return Exact(C->Io.Read32(C->Io.Context, W, Offset, V));
}
static CR_STATUS Mask(CR_MDSS_DP_IRQ_CORE *C, BOOLEAN On, BOOLEAN Ack)
{
  UINT32 V, R;
  CR_STATUS S = Read(C, CrMdssIrqDpAhb, 0x24, &V);
  if (S != CR_SUCCESS)
    return Fail(C, S);
  if (V & FOREIGN_MASK)
    return Fail(C, CR_NOT_READY);
  UINT32 Enable = On ? C->Link.Expected << 2 : 0;
  UINT32 Value = Enable | (Ack ? (V & CR_MDSS_DP_LINK_STATUS) << 1 : 0);
  S = Exact(C->Io.Write32(C->Io.Context, CrMdssIrqDpAhb, 0x24, Value));
  if (S != CR_SUCCESS)
    return Fail(C, S);
  S = Exact(C->Io.Barrier(C->Io.Context));
  if (S != CR_SUCCESS)
    return Fail(C, S);
  S = Read(C, CrMdssIrqDpAhb, 0x24, &R);
  if (S != CR_SUCCESS)
    return Fail(C, S);
  if ((R & CR_MDSS_DP_LINK_MASK) != Enable || (R & FOREIGN_MASK) ||
      (Ack && (R & CR_MDSS_DP_LINK_STATUS)))
    return Fail(C, CR_NOT_READY);
  return CR_SUCCESS;
}
CR_STATUS CrMdssDpLinkIrqRegister(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *Owner,
                                  CR_MDSS_DP_IRQ_TOKEN *T, BOOLEAN *Owned)
{
  if (!Valid(C) || !Owner || !T || !Owned || !Output(C, Owned, sizeof(*Owned)) ||
      Overlap(C, sizeof(*C), T, sizeof(*T)) || Overlap(T, sizeof(*T), Owned, sizeof(*Owned)) ||
      (C->Identity && Overlap(C->Identity, sizeof(*C->Identity), T, sizeof(*T))))
    return CR_INVALID_PARAMETER;
  *Owned = FALSE;
  if (C->Link.Identity)
  {
    if (!Match(C, Owner, T))
      return CR_BUSY;
    *Owned = TRUE;
    return C->Closed || C->Faulted ? CR_NOT_READY : CR_SUCCESS;
  }
  if (C->Closed || C->Faulted)
    return CR_NOT_READY;
  if (T->Provider || T->Serial || T->Incarnation)
    return CR_INVALID_PARAMETER;
  UINT32 V;
  CR_STATUS S = Read(C, CrMdssIrqDpAhb, 0x24, &V);
  if (S != CR_SUCCESS)
    return S;
  if (V & (FOREIGN_MASK | CR_MDSS_DP_LINK_MASK))
    return CR_BUSY;
  if (C->Link.Serial == CR_MAX_UINT64)
    return CR_OUT_OF_RESOURCES;
  C->Link.Identity = T;
  C->Link.Owner = Owner;
  ++C->Link.Serial;
  *T = (CR_MDSS_DP_IRQ_TOKEN){C, C->Incarnation, C->Link.Serial};
  *Owned = TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrMdssDpLinkIrqArm(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *Owner, CR_MDSS_DP_IRQ_TOKEN *T,
                             CONST VOID *Request, UINT32 Expected, UINT64 *Generation)
{
  if (!Match(C, Owner, T) || !Request || !Output(C, Generation, sizeof(*Generation)) ||
      Overlap(Generation, sizeof(*Generation), Request, 1) ||
      (Expected != CR_MDSS_DP_LINK_READY && Expected != CR_MDSS_DP_LINK_IDLE))
    return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted)
    return CR_NOT_READY;
  if (C->Link.Request || C->Link.Armed || C->Link.Pending)
    return CR_BUSY;
  if (C->Link.Generation == CR_MAX_UINT64)
    return CR_OUT_OF_RESOURCES;
  CR_STATUS S = Mask(C, FALSE, TRUE);
  if (S != CR_SUCCESS)
    return S;
  C->Link.Request = Request;
  C->Link.Expected = Expected;
  ++C->Link.Generation;
  C->Link.Armed = TRUE;
  C->Link.Status = 0;
  S = Mask(C, TRUE, FALSE);
  if (S == CR_SUCCESS)
    *Generation = C->Link.Generation;
  return S;
}
CR_STATUS CrMdssDpLinkIrqCapture(CR_MDSS_DP_IRQ_CORE *C, BOOLEAN *Handled, BOOLEAN *Pending)
{
  if (!Valid(C) || !Output(C, Handled, sizeof(*Handled)) || !Output(C, Pending, sizeof(*Pending)) ||
      Overlap(Handled, sizeof(*Handled), Pending, sizeof(*Pending)))
    return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted)
    return CR_NOT_READY;
  UINT32 Parent, V;
  CR_STATUS S = Read(C, CrMdssIrqParent, CR_MDSS_IRQ_PARENT_STATUS, &Parent);
  if (S != CR_SUCCESS)
    return Fail(C, S);
  if (!(Parent & CR_MDSS_IRQ_PARENT_DP))
  {
    *Handled = FALSE;
    *Pending = FALSE;
    return CR_SUCCESS;
  }
  S = Read(C, CrMdssIrqDpAhb, 0x24, &V);
  if (S != CR_SUCCESS)
    return Fail(C, S);
  UINT32 Observed = V & CR_MDSS_DP_LINK_STATUS;
  if (!Observed)
  {
    *Handled = FALSE;
    *Pending = FALSE;
    return CR_SUCCESS;
  }
  if (!C->Link.Identity || !C->Link.Owner || !C->Link.Armed || !C->Link.Request ||
      C->Link.Pending || Observed != C->Link.Expected ||
      (V & CR_MDSS_DP_LINK_MASK) != (C->Link.Expected << 2) || (V & FOREIGN_MASK))
    return Fail(C, CR_NOT_READY);
  if (C->Link.Sequence == CR_MAX_UINT64)
    return Fail(C, CR_OUT_OF_RESOURCES);
  S = Mask(C, FALSE, TRUE);
  if (S != CR_SUCCESS)
    return S;
  C->Link.Status = Observed;
  ++C->Link.Sequence;
  C->Link.Armed = FALSE;
  C->Link.Pending = TRUE;
  *Handled = TRUE;
  *Pending = TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrMdssDpLinkIrqTake(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *Owner, CR_MDSS_DP_IRQ_TOKEN *T,
                              CR_MDSS_DP_IRQ_EVENT *Out)
{
  if (!Match(C, Owner, T) || !Output(C, Out, sizeof(*Out)) ||
      (C->Link.Request && Overlap(C->Link.Request, 1, Out, sizeof(*Out))))
    return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted)
    return CR_NOT_READY;
  if (!C->Link.Pending)
    return CR_NOT_FOUND;
  *Out = (CR_MDSS_DP_IRQ_EVENT){T,
                                Owner,
                                C->Link.Request,
                                C->Link.Serial,
                                C->Link.Generation,
                                C->Link.Sequence,
                                C->Link.Status};
  C->Link.Pending = FALSE;
  return CR_SUCCESS;
}
CR_STATUS CrMdssDpLinkIrqDisarm(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *Owner, CR_MDSS_DP_IRQ_TOKEN *T)
{
  if (!Match(C, Owner, T))
    return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted)
    return CR_NOT_READY;
  CR_STATUS S = Mask(C, FALSE, TRUE);
  if (S == CR_SUCCESS)
    C->Link.Armed = FALSE;
  return S;
}
CR_STATUS CrMdssDpLinkIrqDiscard(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *Owner, CR_MDSS_DP_IRQ_TOKEN *T)
{
  if (!Match(C, Owner, T))
    return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted)
    return CR_NOT_READY;
  if (C->Link.Armed)
    return CR_BUSY;
  C->Link.Pending = FALSE;
  C->Link.Status = 0;
  return CR_SUCCESS;
}
CR_STATUS CrMdssDpLinkIrqRetireRequest(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *Owner,
                                       CR_MDSS_DP_IRQ_TOKEN *T, CONST VOID *Request)
{
  if (!Match(C, Owner, T) || !Request || C->Link.Request != Request)
    return CR_INVALID_PARAMETER;
  if (C->Closed || C->Faulted)
    return CR_NOT_READY;
  if (C->Link.Armed || C->Link.Pending)
    return CR_BUSY;
  C->Link.Request = NULL;
  C->Link.Expected = 0;
  return CR_SUCCESS;
}
CR_STATUS CrMdssDpLinkIrqUnregister(CR_MDSS_DP_IRQ_CORE *C, CONST VOID *Owner,
                                    CR_MDSS_DP_IRQ_TOKEN *T)
{
  if (!Match(C, Owner, T))
    return CR_INVALID_PARAMETER;
  if (C->Link.Request || C->Link.Armed || C->Link.Pending)
    return CR_BUSY;
  CR_STATUS S = CrMdssDpLinkIrqDisarm(C, Owner, T);
  if (S != CR_SUCCESS)
    return S;
  C->Link.Identity = NULL;
  C->Link.Owner = NULL;
  *T = (CR_MDSS_DP_IRQ_TOKEN){0};
  return CR_SUCCESS;
}
