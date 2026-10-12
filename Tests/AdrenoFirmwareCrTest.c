/* Actual A730 parser, constructed wire images and independent span expectations.
   SPDX-License-Identifier: GPL-2.0-only */
#include <Library/AdrenoFirmwareCrLib.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) { return memset(D, V, N); }
#endif

static unsigned Checks;
#define CHECK(X) do { ++Checks; assert(X); } while (0)
static _Alignas(8) unsigned char Image[8192];
static CR_ADRENO_GMU_PLAN Gmu, Saved;
static CR_ADRENO_SQE_PLAN Sqe, SavedSqe;
static void Put(unsigned char *P, uint32_t V) {
  P[0] = V; P[1] = V >> 8; P[2] = V >> 16; P[3] = V >> 24;
}
static size_t Record(size_t At, uint32_t A, uint32_t N, uint32_t T, uint32_t V) {
  CHECK(At + 16 + N <= sizeof(Image));
  Put(Image + At, A); Put(Image + At + 4, N);
  Put(Image + At + 8, T); Put(Image + At + 12, V);
  for (uint32_t I = 0; I < N; ++I) Image[At + 16 + I] = (unsigned char)(I ^ 0x71);
  return At + 16 + N;
}
static void Before(void) { memset(&Gmu, 0x6a, sizeof(Gmu)); Saved = Gmu; }
static void Fail(const void *P, size_t N, CR_STATUS S) {
  Before(); CR_STATUS Actual = CrAdrenoGmuInspect(P, N, &Gmu);
  if (Actual != S) fprintf(stderr, "GMU expected=%llx actual=%llx size=%zu\n",
                         (unsigned long long)S, (unsigned long long)Actual, N);
  CHECK(Actual == S);
  CHECK(memcmp(&Gmu, &Saved, sizeof(Gmu)) == 0);
}
static size_t Nominal(void) {
  memset(Image, 0, sizeof(Image)); size_t N = 0;
  N = Record(N, 0x4000, 0, 1, 0x4000);
  N = Record(N, 0x60000000, 0, 7, 0x4000);
  for (uint32_t T = 2; T <= 6; ++T) N = Record(N, 0, 0, T, 0x100 + T);
  N = Record(N, 0xdeadbeef, 0, 0, 0x92);
  N = Record(N, 0, 4, 0, 0x7f);
  N = Record(N, 0x10004000, 8, 0, 0);
  N = Record(N, 0x4000, 4, 0, 0);
  N = Record(N, 0x60000000, 4, 0, 0);
  return N;
}
static void Valid(void) {
  size_t N = Nominal(); Before(); CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  CHECK(Gmu.Version == 1 && Gmu.Profile == 1 && Gmu.ImageBytes == N);
  CHECK(Gmu.RecordCount == 12 && Gmu.DataCount == 4 && Gmu.PreallocationCount == 2);
  CHECK(Gmu.TotalDataBytes == 20 && Gmu.VersionMask == 31);
  for (unsigned I = 0; I < 5; ++I) CHECK(Gmu.Versions[I] == 0x102 + I);
  CHECK(Gmu.Records[0].HeaderOffset == 0 && Gmu.Records[0].PayloadOffset == 16);
  CHECK(Gmu.Records[0].Action == CrAdrenoGmuRecordPreallocation);
  CHECK(Gmu.Records[0].Bytes == 0x4000 && Gmu.Records[0].Region == CrAdrenoGmuRegionCache);
  CHECK(Gmu.Records[1].Type == 7 && Gmu.Records[1].Region == CrAdrenoGmuRegionUncached);
  CHECK(Gmu.Records[7].Action == CrAdrenoGmuRecordZeroData && Gmu.Records[7].Value == 0x92);
  CHECK(Gmu.Records[8].Destination == 0 && Gmu.Records[8].Region == CrAdrenoGmuRegionItcm);
  CHECK(Gmu.Records[8].HeaderOffset == 128 && Gmu.Records[8].PayloadOffset == 144);
  CHECK(Gmu.Records[9].Region == CrAdrenoGmuRegionDtcm && Gmu.Records[9].Bytes == 8);
  CHECK(Gmu.Records[10].Region == CrAdrenoGmuRegionCache);
  CHECK(Gmu.Records[11].Region == CrAdrenoGmuRegionUncached);
  for (unsigned I = 12; I < CR_ADRENO_GMU_MAX_RECORDS; ++I)
    CHECK(memcmp(&Gmu.Records[I], &(CR_ADRENO_GMU_RECORD){0}, sizeof(Gmu.Records[I])) == 0);
  CHECK(Gmu.Reserved[0] == 0 && Gmu.Reserved[1] == 0 && Gmu.Reserved[2] == 0);

  unsigned char Copy[sizeof(Image) + 1]; memcpy(Copy + 1, Image, N);
  CR_ADRENO_GMU_PLAN Expected = Gmu;
  CHECK(CrAdrenoGmuInspect(Copy + 1, N, &Gmu) == CR_SUCCESS);
  CHECK(memcmp(&Expected, &Gmu, sizeof(Gmu)) == 0);
  /* Exact ends are legal; adjacent data ranges are not merged. */
  N = Record(0, 0x3ffc, 4, 0, 0); N = Record(N, 0x4000, 4, 0, 0);
  CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  CHECK(Gmu.Records[0].Region == 1 && Gmu.Records[1].Region == 3);
  N = Record(0, 0x10007ffc, 4, 0, 0); N = Record(N, 0x7ffffffc, 4, 0, 0);
  CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  /* The old DTCM numerical address lies in Gen7 I-cache space: classify it
     as cache, never falsely grant the Gen7 DTCM view. */
  N = Record(0, 0x40000, 4, 0, 0);
  CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  CHECK(Gmu.Records[0].Region == CrAdrenoGmuRegionCache);
}
static void Truncated(void) {
  size_t N = Nominal(); unsigned char Full[sizeof(Image)]; memcpy(Full, Image, N);
  for (size_t Cut = 1; Cut < N; ++Cut) {
    unsigned char *Short = malloc(Cut); CHECK(Short != NULL); memcpy(Short, Full, Cut);
    Before(); CR_STATUS S = CrAdrenoGmuInspect(Short, Cut, &Gmu);
    if (Cut == 148 || Cut == 172 || Cut == 192) {
      CHECK(S == CR_SUCCESS); CHECK(Gmu.ImageBytes == Cut);
    } else {
      CHECK(S != CR_SUCCESS); CHECK(memcmp(&Gmu, &Saved, sizeof(Gmu)) == 0);
    }
    free(Short);
  }
  Fail(NULL, N, CR_INVALID_PARAMETER); Fail(Image, 0, CR_INVALID_PARAMETER);
  unsigned char *Huge = malloc(CR_ADRENO_FIRMWARE_MAX_BYTES + 1U); CHECK(Huge != NULL);
  Fail(Huge, CR_ADRENO_FIRMWARE_MAX_BYTES + 1U, CR_BAD_BUFFER_SIZE); free(Huge);
  Fail((void *)(UINTN)((UINTN)-1 - 7), 16, CR_INVALID_PARAMETER);
  CHECK(CrAdrenoGmuInspect(Image, N, NULL) == CR_INVALID_PARAMETER);
  CHECK(CrAdrenoGmuInspect(Image, N, (void *)((unsigned char *)&Gmu + 1)) == CR_INVALID_PARAMETER);
  CHECK(CrAdrenoGmuInspect(Image, N, (void *)Image) == CR_INVALID_PARAMETER);
  CHECK(CrAdrenoGmuInspect((unsigned char *)&Gmu + 16, 16, &Gmu) == CR_INVALID_PARAMETER);
  CHECK(CrAdrenoGmuInspect(Image, N, (void *)(UINTN)((UINTN)-1 - 15)) == CR_INVALID_PARAMETER);
}
static void Malformed(void) {
  size_t N;
  const uint32_t BadA[] = {0x3ffc, 0x10007ffc, 0x00fffffc, 0x7ffffffc,
                          0xfffffffc, 0x80000000, 0xc0000000, 0x10003ffc};
  for (unsigned I = 0; I < sizeof(BadA)/sizeof(*BadA); ++I) {
    N = Record(0, BadA[I], 8, 0, 0); Fail(Image, N, CR_COMPROMISED_DATA);
  }
  N = Record(0, 1, 4, 0, 0); Fail(Image, N, CR_COMPROMISED_DATA);
  N = Record(0, 0, 3, 0, 0); Fail(Image, N, CR_COMPROMISED_DATA);
  N = Record(0, 0, 4, 0, 0); Put(Image + 4, 0xffffffff); Fail(Image, N, CR_BAD_BUFFER_SIZE);
  N = Record(0, 0, 0, 8, 0); Fail(Image, N, CR_UNSUPPORTED);
  N = Record(0, 0, 4, 1, 0); CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  N = Record(0, 0, 4, 6, 0); CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  N = Record(0, 0, 0, 0, 0); Fail(Image, N, CR_NOT_FOUND);
  N = Record(0, 0, 0, 2, 0x41); Fail(Image, N, CR_NOT_FOUND);
  N = Record(N, 0, 0, 2, 0x42); N = Record(N, 0, 4, 0, 0);
  Fail(Image, N, CR_COMPROMISED_DATA);
  N = Record(0, 0, 0, 2, 0x41); N = Record(N, 0, 0, 2, 0x41);
  N = Record(N, 0, 4, 0, 0); CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  CHECK(Gmu.VersionMask == 1 && Gmu.Versions[0] == 0x41);
  N = Record(0, 0, 4, 0, 0); N = Record(N, 0, 0, 2, 0x41);
  Fail(Image, N, CR_COMPROMISED_DATA);
  N = Record(0, 0, 8, 0, 0); N = Record(N, 4, 4, 0, 0);
  Fail(Image, N, CR_COMPROMISED_DATA);
  N = Record(0, 0x4000, 0, 1, 0); N = Record(N, 0, 4, 0, 0);
  Fail(Image, N, CR_COMPROMISED_DATA);
  N = Record(0, 0xfffffffc, 0, 7, 8); N = Record(N, 0, 4, 0, 0);
  Fail(Image, N, CR_COMPROMISED_DATA);
  N = Record(0, 0x4000, 0, 1, 0x4000); N = Record(N, 0x5000, 0, 1, 0x4000);
  N = Record(N, 0, 4, 0, 0); Fail(Image, N, CR_COMPROMISED_DATA);
  N = Record(0, 0x4000, 0, 1, 0x4000); N = Record(N, 0x4000, 0, 1, 0x4000);
  N = Record(N, 0, 4, 0, 0); CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  CHECK(Gmu.PreallocationCount == 2);
  N = 0; for (unsigned I = 0; I < CR_ADRENO_GMU_MAX_RECORDS - 1; ++I)
    N = Record(N, 0, 0, 0, 0);
  N = Record(N, 0, 4, 0, 0); CHECK(CrAdrenoGmuInspect(Image, N, &Gmu) == CR_SUCCESS);
  CHECK(Gmu.RecordCount == CR_ADRENO_GMU_MAX_RECORDS);
  N = Record(N, 4, 4, 0, 0); Fail(Image, N, CR_OUT_OF_RESOURCES);
  /* Last-record rejection must not publish an otherwise valid prefix. */
  N = Nominal(); N = Record(N, 0xc0000000, 4, 0, 0); Fail(Image, N, CR_COMPROMISED_DATA);
}
static void SqeFail(const void *P, size_t N, CR_STATUS S) {
  memset(&Sqe, 0xc7, sizeof(Sqe)); SavedSqe = Sqe;
  CHECK(CrAdrenoSqeInspect(P, N, &Sqe) == S);
  CHECK(memcmp(&Sqe, &SavedSqe, sizeof(Sqe)) == 0);
}
static void SqeCases(void) {
  memset(Image, 0, sizeof(Image)); Put(Image, 0xaabbccdd); Put(Image + 4, 0x11234123);
  CHECK(CrAdrenoSqeInspect(Image, 8, &Sqe) == CR_SUCCESS);
  CHECK(Sqe.Version == 1 && Sqe.PrefixWord == 0xaabbccdd && Sqe.CodeOffset == 4);
  CHECK(Sqe.CodeBytes == 4 && Sqe.CodeWords == 1 && Sqe.RawCodeVersion == 0x11234123);
  CHECK(Sqe.CodeVersion == 0x11234123 && Sqe.Flags == 0);
  Put(Image + 4, 0x1123400a); Put(Image + 12, 0xfabc0123);
  CHECK(CrAdrenoSqeInspect(Image, 16, &Sqe) == CR_SUCCESS);
  CHECK(Sqe.CodeBytes == 12 && Sqe.CodeWords == 3 && Sqe.CodeVersion == 0x11234bc0);
  CHECK(Sqe.RawCodeVersion == 0x1123400a && Sqe.Flags == CR_ADRENO_SQE_PATCHED_VERSION);
  unsigned char Copy[17]; memcpy(Copy + 1, Image, 16);
  SavedSqe = Sqe; CHECK(CrAdrenoSqeInspect(Copy + 1, 16, &Sqe) == CR_SUCCESS);
  CHECK(memcmp(&Sqe, &SavedSqe, sizeof(Sqe)) == 0);
  for (size_t N = 1; N < 16; ++N) {
    unsigned char *Short = malloc(N); CHECK(Short != NULL); memcpy(Short, Image, N);
    SqeFail(Short, N, CR_BAD_BUFFER_SIZE); free(Short);
  }
  SqeFail(NULL, 16, CR_INVALID_PARAMETER); SqeFail(Image, 0, CR_INVALID_PARAMETER);
  unsigned char *Huge = malloc(CR_ADRENO_FIRMWARE_MAX_BYTES + 4U); CHECK(Huge != NULL);
  SqeFail(Huge, CR_ADRENO_FIRMWARE_MAX_BYTES + 4U, CR_BAD_BUFFER_SIZE); free(Huge);
  CHECK(CrAdrenoSqeInspect(Image, 16, NULL) == CR_INVALID_PARAMETER);
  CHECK(CrAdrenoSqeInspect(Image, 16, (void *)Image) == CR_INVALID_PARAMETER);
  CHECK(CrAdrenoSqeInspect((char *)&Sqe + 4, 8, &Sqe) == CR_INVALID_PARAMETER);
  CHECK(CrAdrenoSqeInspect(Image, 16, (void *)((char *)&Sqe + 1)) == CR_INVALID_PARAMETER);
  SqeFail((void *)(UINTN)((UINTN)-1 - 7), 16, CR_INVALID_PARAMETER);
}
static void Maximum(void) {
  const size_t Payload = 0x1000000 - 0x4000;
  unsigned char *Large = malloc(Payload + 16); CHECK(Large != NULL);
  memset(Large, 0x61, Payload + 16); Put(Large, 0x4000); Put(Large + 4, Payload);
  Put(Large + 8, 0); Put(Large + 12, 0);
  CHECK(CrAdrenoGmuInspect(Large, Payload + 16, &Gmu) == CR_SUCCESS);
  CHECK(Gmu.DataCount == 1 && Gmu.TotalDataBytes == Payload);
  CHECK(Gmu.Records[0].Region == CrAdrenoGmuRegionCache);
  Put(Large + 4, Payload + 4); Fail(Large, Payload + 16, CR_BAD_BUFFER_SIZE);
  free(Large);
}
static void Mutations(void) {
  /* Deterministic hostile bytes, separately allocated at the declared extent. */
  uint32_t Seed = 0x730001;
  for (unsigned I = 0; I < 2000; ++I) {
    size_t N = 1 + (I % 255); unsigned char *P = malloc(N); CHECK(P != NULL);
    for (size_t J = 0; J < N; ++J) {
      Seed = Seed * 1664525U + 1013904223U; P[J] = (unsigned char)(Seed >> 24);
    }
    Before(); CR_STATUS S = CrAdrenoGmuInspect(P, N, &Gmu);
    CHECK(S != CR_SUCCESS); CHECK(memcmp(&Gmu, &Saved, sizeof(Gmu)) == 0);
    free(P);
  }
}
int main(void) {
  Valid(); Truncated(); Malformed(); SqeCases(); Maximum(); Mutations();
  printf("Adreno firmware actual-source PASS %u checks\n", Checks);
  return 0;
}
