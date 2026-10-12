/* Actual pure resolver with boundary and independent composition expectations.
   SPDX-License-Identifier: MIT */
#include <Library/Sm8450GpuResourcesCrLib.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned Checks, Permutations;
#define CHECK(X) do { ++Checks; assert(X); } while (0)
static CONST CR_SM8450_GPU_VIEW_DESCRIPTOR Expected[7] = {
  {1, 1, 0x03d00000, 0x40000}, {2, 1, 0x03d61000, 0x800},
  {3, 1, 0x03d6a000, 0x26000}, {4, 1, 0x03d9a000, 0x5000},
  {5, 1, 0x03d50000, 0x10000}, {6, 1, 0x0b290000, 0x10000},
  {7, 2, 0x03d90000, 0xa000}
};
static CR_SM8450_GPU_VIEW_DESCRIPTOR D[7];
static CR_SM8450_GPU_LAYOUT L, Saved;
static CR_SM8450_GPU_REGISTER_SPAN S, SavedSpan;
static void Nominal(void) {
  memcpy(D, Expected, sizeof(D));
  CHECK(CrSm8450GpuResourcesBuild(D, 7, &L) == CR_SUCCESS);
  CHECK(L.Version == 1 && L.Count == 7 && !L.Reserved[0] && !L.Reserved[1]);
  CHECK(!memcmp(L.Views, Expected, sizeof(Expected)));
}
static void BuildFail(const CR_SM8450_GPU_VIEW_DESCRIPTOR *P, size_t Count, CR_STATUS Status) {
  memset(&L, 0x92, sizeof(L)); Saved = L;
  CHECK(CrSm8450GpuResourcesBuild(P, Count, &L) == Status);
  CHECK(!memcmp(&L, &Saved, sizeof(L)));
}
static void Fail(CR_SM8450_GPU_NAMESPACE Ns, uint64_t Word, uint32_t Bytes, CR_STATUS Status) {
  memset(&S, 0x71, sizeof(S)); SavedSpan = S;
  CHECK(CrSm8450GpuResourcesResolve(&L, Ns, Word, Bytes, &S) == Status);
  CHECK(!memcmp(&S, &SavedSpan, sizeof(S)));
}
static void Hit(CR_SM8450_GPU_NAMESPACE Ns, uint64_t Word, uint32_t Bytes,
                unsigned View, uint32_t Offset, uint64_t Address) {
  memset(&S, 0x71, sizeof(S));
  CHECK(CrSm8450GpuResourcesResolve(&L, Ns, Word, Bytes, &S) == CR_SUCCESS);
  CHECK(S.View == View && S.ByteOffset == Offset && S.Bytes == Bytes);
  CHECK(S.PhysicalAddress == Address && S.Relation == (View == 7 ? 2U : 1U));
}
static void Permute(unsigned N) {
  if (N == 7) {
    CHECK(CrSm8450GpuResourcesBuild(D, 7, &L) == CR_SUCCESS);
    CHECK(!memcmp(L.Views, Expected, sizeof(Expected)));
    CHECK(L.Version == 1 && L.Count == 7 && !L.Reserved[0] && !L.Reserved[1]);
    ++Permutations; return;
  }
  for (unsigned J = N; J < 7; ++J) {
    CR_SM8450_GPU_VIEW_DESCRIPTOR T = D[N]; D[N] = D[J]; D[J] = T;
    Permute(N + 1);
    T = D[N]; D[N] = D[J]; D[J] = T;
  }
}
static void Composition(void) {
  Nominal(); Permute(0); CHECK(Permutations == 5040);
  for (unsigned I = 0; I < 7; ++I) {
    memcpy(D, Expected, sizeof(D)); D[I].Base += 4; BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I].Base |= 1; BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I].Length -= 4; BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I].Length += 4; BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I].Length = 0; BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I].Base = UINT64_MAX - 3;
    BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I].Relation ^= 3; BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I].View = 0; BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I].View = 8; BuildFail(D, 7, CR_COMPROMISED_DATA);
    memcpy(D, Expected, sizeof(D)); D[I] = D[(I + 1) % 7]; BuildFail(D, 7, CR_COMPROMISED_DATA);
  }
  memcpy(D, Expected, sizeof(D)); D[2].Length = 0x35000;
  BuildFail(D, 7, CR_COMPROMISED_DATA); /* Broad Linux GMU overlaps GPUCC. */
  memcpy(D, Expected, sizeof(D)); D[2].Base = 0x03d68000; D[2].Length = 0x37000;
  BuildFail(D, 7, CR_COMPROMISED_DATA); /* Android broad convention not this profile. */
  memcpy(D, Expected, sizeof(D)); D[3].Base = 0x03d9e000; D[3].Length = 0x1000;
  BuildFail(D, 7, CR_COMPROMISED_DATA); /* CX-misc is a slice, not another view. */
  memcpy(D, Expected, sizeof(D)); D[1].Length = 0x1000;
  BuildFail(D, 7, CR_COMPROMISED_DATA); /* Page rounding does not extend authority. */
  memcpy(D, Expected, sizeof(D)); D[5].Base = 0x0b220000; D[5].Length = 0x30000;
  BuildFail(D, 7, CR_COMPROMISED_DATA); /* GPIO's PDC is not GMU-PDC. */
  memcpy(D, Expected, sizeof(D)); D[6].Relation = 1;
  BuildFail(D, 7, CR_COMPROMISED_DATA); /* GPUCC cannot become another dedicated map. */
  BuildFail(D, 6, CR_INVALID_PARAMETER); BuildFail(D, 8, CR_INVALID_PARAMETER);
  BuildFail(NULL, 7, CR_INVALID_PARAMETER);
}
static void Boundaries(void) {
  Nominal();
  struct { unsigned Ns, View; uint64_t Start; uint32_t Length, Offset; } Cases[] = {
    {1, 1, 0, 0x40000, 0}, {2, 3, 0x6a000 / 4, 0x26000, 0},
    {2, 7, 0x90000 / 4, 0xa000, 0}, {2, 4, 0x9a000 / 4, 0x5000, 0},
    {3, 5, 0, 0x10000, 0}, {4, 6, 0, 0x10000, 0},
    {5, 2, 0, 0x800, 0}, {6, 4, 0, 0x1000, 0x4000}, {7, 7, 0, 0xa000, 0}
  };
  for (unsigned I = 0; I < sizeof(Cases) / sizeof(Cases[0]); ++I) {
    unsigned V = Cases[I].View; uint64_t A = Expected[V - 1].Base + Cases[I].Offset;
    Hit(Cases[I].Ns, Cases[I].Start, 4, V, Cases[I].Offset, A);
    Hit(Cases[I].Ns, Cases[I].Start, Cases[I].Length, V, Cases[I].Offset, A);
    uint32_t End = Cases[I].Length - 4;
    Hit(Cases[I].Ns, Cases[I].Start + End / 4, 4, V, Cases[I].Offset + End, A + End);
    Fail(Cases[I].Ns, Cases[I].Start + End / 4, 8, CR_NOT_FOUND);
  }
  /* A new DWORD at either contiguous GMU/GPUCC boundary resolves a different
     view; a two-DWORD access spanning the boundary remains invalid. */
  Hit(2, 0x8fffc / 4, 4, 3, 0x25ffc, 0x03d8fffc);
  Hit(2, 0x90000 / 4, 4, 7, 0, 0x03d90000);
  Fail(2, 0x8fffc / 4, 8, CR_NOT_FOUND);
  Hit(2, 0x99ffc / 4, 4, 7, 0x9ffc, 0x03d99ffc);
  Hit(2, 0x9a000 / 4, 4, 4, 0, 0x03d9a000);
  Fail(2, 0x99ffc / 4, 8, CR_NOT_FOUND);
  Fail(2, 0x69ffc / 4, 4, CR_NOT_FOUND); Fail(2, 0x9f000 / 4, 4, CR_NOT_FOUND);
  Fail(1, 0x6a000 / 4, 4, CR_NOT_FOUND); Fail(2, 0, 4, CR_NOT_FOUND);
  Fail(2, 0x50000 / 4, 4, CR_NOT_FOUND); /* RSCC requires its own namespace. */
  Fail(2, (0x0b290000 - 0x03d00000) / 4, 4, CR_NOT_FOUND);
  Fail(5, 0x800 / 4, 4, CR_NOT_FOUND); Fail(6, 0x1000 / 4, 4, CR_NOT_FOUND);
  Fail(3, 0x10000 / 4, 4, CR_NOT_FOUND); Fail(4, 0x10000 / 4, 4, CR_NOT_FOUND);
  Fail(1, 0x40000 / 4, 4, CR_NOT_FOUND); Fail(7, 0xa000 / 4, 4, CR_NOT_FOUND);
  Fail(2, 0x6a000 / 4, 0x26004, CR_NOT_FOUND);
  for (unsigned Ns = 1; Ns <= 7; ++Ns) {
    Fail(Ns, 0, 0, CR_INVALID_PARAMETER); Fail(Ns, 0, 1, CR_INVALID_PARAMETER);
    Fail(Ns, 0, UINT32_MAX, CR_INVALID_PARAMETER);
    Fail(Ns, UINT64_MAX, 4, CR_INVALID_PARAMETER);
    Fail(Ns, UINT64_MAX >> 2, 4, CR_INVALID_PARAMETER);
    Fail(Ns, (UINT64_MAX - 0x03d00000 + 1) / 4, 4, CR_INVALID_PARAMETER);
  }
  Fail(0, 0, 4, CR_INVALID_PARAMETER); Fail(8, 0, 4, CR_INVALID_PARAMETER);
}
static void Malformed(void) {
  Nominal(); CR_SM8450_GPU_LAYOUT Good = L;
  /* Every normalized byte participates in validation; none is a grant token. */
  for (size_t I = 0; I < sizeof(L); ++I) {
    L = Good; ((unsigned char *)&L)[I] ^= 0x80;
    Fail(1, 0, 4, CR_COMPROMISED_DATA);
  }
  L = Good; Saved = L;
  CHECK(CrSm8450GpuResourcesBuild(L.Views, 7, &L) == CR_INVALID_PARAMETER);
  CHECK(!memcmp(&L, &Saved, sizeof(L)));
  CHECK(CrSm8450GpuResourcesResolve(&L, 1, 0, 4, (void *)&L.Views[1]) == CR_INVALID_PARAMETER);
  CHECK(!memcmp(&L, &Saved, sizeof(L)));
  _Alignas(8) unsigned char Bytes[sizeof(L) + 8]; memcpy(Bytes + 1, &L, sizeof(L));
  memset(&S, 0x71, sizeof(S)); SavedSpan = S;
  CHECK(CrSm8450GpuResourcesResolve((void *)(Bytes + 1), 1, 0, 4, &S) == CR_INVALID_PARAMETER);
  CHECK(!memcmp(&S, &SavedSpan, sizeof(S)));
  CHECK(CrSm8450GpuResourcesResolve(&L, 1, 0, 4, (void *)(Bytes + 1)) == CR_INVALID_PARAMETER);
  BuildFail((void *)(Bytes + 1), 7, CR_INVALID_PARAMETER);
  BuildFail((void *)(UINTPTR_MAX & ~(uintptr_t)7), 7, CR_INVALID_PARAMETER);
  CHECK(CrSm8450GpuResourcesBuild(D, 7, NULL) == CR_INVALID_PARAMETER);
  CHECK(CrSm8450GpuResourcesBuild(D, 7, (void *)(UINTPTR_MAX & ~(uintptr_t)7)) == CR_INVALID_PARAMETER);
  CHECK(CrSm8450GpuResourcesResolve(&L, 1, 0, 4, NULL) == CR_INVALID_PARAMETER);
  CHECK(CrSm8450GpuResourcesResolve(NULL, 1, 0, 4, &S) == CR_INVALID_PARAMETER);
  CHECK(CrSm8450GpuResourcesResolve(&L, 1, 0, 4, (void *)(UINTPTR_MAX & ~(uintptr_t)7)) == CR_INVALID_PARAMETER);
}
int main(void) {
  Composition(); Boundaries(); Malformed();
  printf("SM8450 GPU resource resolver PASS %u checks, %u descriptor permutations\n", Checks, Permutations);
  return 0;
}
