/* SPDX-License-Identifier: MIT */
#include <Library/Sm8450GpuResourcesCrLib.h>

#define MAX64 ((UINT64)-1)
#define MAXN ((UINTN)-1)
#define VIEW_BIT(V) (1U << ((V) - 1U))

/* Original bounded layout implementation. Address facts are from the pinned
   BSD-3-Clause SM8450 DTS; source-relative semantics are documented in README. */
static CONST CR_SM8450_GPU_VIEW_DESCRIPTOR Policy[CR_SM8450_GPU_VIEW_COUNT] = {
  {CrSm8450GpuViewGpu, CR_SM8450_GPU_RELATION_DEDICATED, 0x03d00000, 0x40000},
  {CrSm8450GpuViewCxDebug, CR_SM8450_GPU_RELATION_DEDICATED, 0x03d61000, 0x800},
  {CrSm8450GpuViewGmuLow, CR_SM8450_GPU_RELATION_DEDICATED, 0x03d6a000, 0x26000},
  {CrSm8450GpuViewGmuHigh, CR_SM8450_GPU_RELATION_DEDICATED, 0x03d9a000, 0x5000},
  {CrSm8450GpuViewRscc, CR_SM8450_GPU_RELATION_DEDICATED, 0x03d50000, 0x10000},
  {CrSm8450GpuViewPdc, CR_SM8450_GPU_RELATION_DEDICATED, 0x0b290000, 0x10000},
  {CrSm8450GpuViewGpucc, CR_SM8450_GPU_RELATION_BORROWED, 0x03d90000, 0xa000}
};

static BOOLEAN Buffer(CONST VOID *P, UINTN Bytes) {
  UINTN A = (UINTN)P;
  return P != NULL && !(A & 7U) && A <= MAXN - Bytes;
}
static BOOLEAN Separate(CONST VOID *A, UINTN An, CONST VOID *B, UINTN Bn) {
  UINTN X = (UINTN)A, Y = (UINTN)B;
  return !(X < Y + Bn && Y < X + An);
}
static BOOLEAN Exact(CONST CR_SM8450_GPU_VIEW_DESCRIPTOR *D) {
  if (D->View < 1 || D->View > CR_SM8450_GPU_VIEW_COUNT || !D->Length ||
      (D->Base & 3U) || (D->Length & 3U) || D->Base > MAX64 - D->Length)
    return FALSE;
  CONST CR_SM8450_GPU_VIEW_DESCRIPTOR *P = &Policy[D->View - 1U];
  return D->Base == P->Base && D->Length == P->Length && D->Relation == P->Relation;
}
static BOOLEAN ValidLayout(CONST CR_SM8450_GPU_LAYOUT *L) {
  if (L->Version != CR_SM8450_GPU_LAYOUT_VERSION ||
      L->Count != CR_SM8450_GPU_VIEW_COUNT || L->Reserved[0] || L->Reserved[1])
    return FALSE;
  for (UINT32 N = 0; N < CR_SM8450_GPU_VIEW_COUNT; ++N)
    if (L->Views[N].View != N + 1U || !Exact(&L->Views[N])) return FALSE;
  return TRUE;
}

CR_STATUS CrSm8450GpuResourcesBuild(CONST CR_SM8450_GPU_VIEW_DESCRIPTOR *D,
    UINTN Count, CR_SM8450_GPU_LAYOUT *L) {
  if (Count != CR_SM8450_GPU_VIEW_COUNT || !Buffer(D, sizeof(L->Views)) ||
      !Buffer(L, sizeof(*L)) || !Separate(D, sizeof(L->Views), L, sizeof(*L)))
    return CR_INVALID_PARAMETER;
  UINT32 Seen = 0;
  for (UINT32 N = 0; N < CR_SM8450_GPU_VIEW_COUNT; ++N) {
    if (!Exact(&D[N]) || (Seen & VIEW_BIT(D[N].View))) return CR_COMPROMISED_DATA;
    Seen |= VIEW_BIT(D[N].View);
    for (UINT32 J = 0; J < N; ++J)
      if (D[N].Base < D[J].Base + D[J].Length &&
          D[J].Base < D[N].Base + D[N].Length) return CR_COMPROMISED_DATA;
  }
  /* All validation precedes the only output mutation. Immutable input and
     disjoint output prevent normalization from modifying later input entries. */
  L->Version = CR_SM8450_GPU_LAYOUT_VERSION;
  L->Count = CR_SM8450_GPU_VIEW_COUNT;
  L->Reserved[0] = L->Reserved[1] = 0;
  for (UINT32 N = 0; N < CR_SM8450_GPU_VIEW_COUNT; ++N)
    L->Views[D[N].View - 1U] = D[N];
  return CR_SUCCESS;
}

CR_STATUS CrSm8450GpuResourcesResolve(CONST CR_SM8450_GPU_LAYOUT *L,
    CR_SM8450_GPU_NAMESPACE Ns, UINT64 Words, UINT32 Bytes,
    CR_SM8450_GPU_REGISTER_SPAN *Out) {
  if (!Buffer(L, sizeof(*L)) || !Buffer(Out, sizeof(*Out)) ||
      !Separate(L, sizeof(*L), Out, sizeof(*Out)) || !Bytes || (Bytes & 3U))
    return CR_INVALID_PARAMETER;
  if (!ValidLayout(L)) return CR_COMPROMISED_DATA;
  UINT64 Origin, Limit = 0;
  UINT32 Allowed;
  switch (Ns) {
  case CrSm8450GpuNamespaceGpu:
    Origin = 0x03d00000; Allowed = VIEW_BIT(CrSm8450GpuViewGpu); break;
  case CrSm8450GpuNamespaceGmu:
    Origin = 0x03d00000;
    Allowed = VIEW_BIT(CrSm8450GpuViewGmuLow) | VIEW_BIT(CrSm8450GpuViewGmuHigh) |
              VIEW_BIT(CrSm8450GpuViewGpucc); break;
  case CrSm8450GpuNamespaceRscc:
    Origin = 0x03d50000; Allowed = VIEW_BIT(CrSm8450GpuViewRscc); break;
  case CrSm8450GpuNamespacePdc:
    Origin = 0x0b290000; Allowed = VIEW_BIT(CrSm8450GpuViewPdc); break;
  case CrSm8450GpuNamespaceCxDebug:
    Origin = 0x03d61000; Allowed = VIEW_BIT(CrSm8450GpuViewCxDebug); break;
  case CrSm8450GpuNamespaceCxMisc:
    Origin = 0x03d9e000; Limit = 0x1000; Allowed = VIEW_BIT(CrSm8450GpuViewGmuHigh); break;
  case CrSm8450GpuNamespaceGpucc:
    Origin = 0x03d90000; Allowed = VIEW_BIT(CrSm8450GpuViewGpucc); break;
  default: return CR_INVALID_PARAMETER;
  }
  if (Words > (MAX64 >> 2)) return CR_INVALID_PARAMETER;
  UINT64 Offset = Words << 2;
  if (Offset > MAX64 - Origin || Origin + Offset > MAX64 - Bytes)
    return CR_INVALID_PARAMETER;
  if (Limit && (Offset >= Limit || Bytes > Limit - Offset)) return CR_NOT_FOUND;
  UINT64 Address = Origin + Offset;
  for (UINT32 N = 0; N < CR_SM8450_GPU_VIEW_COUNT; ++N) {
    CONST CR_SM8450_GPU_VIEW_DESCRIPTOR *V = &L->Views[N];
    if (!(Allowed & VIEW_BIT(V->View)) || Address < V->Base) continue;
    UINT64 At = Address - V->Base;
    if (At >= V->Length || Bytes > V->Length - At) continue;
    CR_SM8450_GPU_REGISTER_SPAN S;
    S.View = V->View; S.ByteOffset = (UINT32)At; S.Bytes = Bytes;
    S.Relation = V->Relation; S.PhysicalAddress = Address;
    *Out = S;
    return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}
