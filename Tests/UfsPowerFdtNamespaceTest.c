/* SPDX-License-Identifier: MIT */
/* Include the actual private namespace validator without a firmware test API.
 * The wrappers count libfdt traversals, but always execute the real reader.
 * Timing is deliberately not an assertion: cache-off SEC timing cannot be
 * inferred from cached host timing. Whole-tree rescans are deterministic.
 */
#include <Library/UfsPowerFdtCrLib.h>
#include <Library/FdtLib.h>
#include <Library/BaseMemoryLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t ParentCalls, FirstSubnodeCalls, NextSubnodeCalls;
static size_t NextNodeCalls, FirstPropertyCalls, NextPropertyCalls;

static INT32 CountParent(CONST VOID *F, INT32 N) {
  ++ParentCalls;
  return FdtParentOffset(F, N);
}
static INT32 CountFirstSubnode(CONST VOID *F, INT32 N) {
  ++FirstSubnodeCalls;
  return FdtFirstSubnode(F, N);
}
static INT32 CountNextSubnode(CONST VOID *F, INT32 N) {
  ++NextSubnodeCalls;
  return FdtNextSubnode(F, N);
}
static INT32 CountNextNode(CONST VOID *F, INT32 N, INT32 *D) {
  ++NextNodeCalls;
  return FdtNextNode(F, N, D);
}
static INT32 CountFirstProperty(CONST VOID *F, INT32 N) {
  ++FirstPropertyCalls;
  return FdtFirstPropertyOffset(F, N);
}
static INT32 CountNextProperty(CONST VOID *F, INT32 N) {
  ++NextPropertyCalls;
  return FdtNextPropertyOffset(F, N);
}

#define FdtParentOffset CountParent
#define FdtFirstSubnode CountFirstSubnode
#define FdtNextSubnode CountNextSubnode
#define FdtNextNode CountNextNode
#define FdtFirstPropertyOffset CountFirstProperty
#define FdtNextPropertyOffset CountNextProperty
#ifndef NAMESPACE_SOURCE
#define NAMESPACE_SOURCE "../Library/UfsPowerFdtCrLib/UfsPowerFdt.c"
#endif
#include NAMESPACE_SOURCE
#undef FdtParentOffset
#undef FdtFirstSubnode
#undef FdtNextSubnode
#undef FdtNextNode
#undef FdtFirstPropertyOffset
#undef FdtNextPropertyOffset

VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) {
  return memmove(D, S, N);
}
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) {
  return memcmp(A, B, N);
}

static int Check(CONST char *Path, CONST char *Expected, int Performance) {
  FILE *File = fopen(Path, "rb");
  if (!File) return 2;
  if (fseek(File, 0, SEEK_END)) { fclose(File); return 2; }
  long Length = ftell(File);
  if (Length <= 0) { fclose(File); return 2; }
  rewind(File);
  VOID *Fdt = malloc((size_t)Length);
  if (!Fdt) { fclose(File); return 2; }
  size_t Read = fread(Fdt, 1, (size_t)Length, File);
  fclose(File);
  if (Read != (size_t)Length || fdt_check_full(Fdt, (size_t)Length)) {
    free(Fdt);
    return 2;
  }
  size_t Nodes = 0, Properties = 0;
  int Depth = 0, N = -1;
  while ((N = FdtNextNode(Fdt, N, &Depth)) >= 0 && Depth >= 0) {
    ++Nodes;
    for (int P = FdtFirstPropertyOffset(Fdt, N); P >= 0;
         P = FdtNextPropertyOffset(Fdt, P)) ++Properties;
  }
  ParentCalls = FirstSubnodeCalls = NextSubnodeCalls = 0;
  NextNodeCalls = FirstPropertyCalls = NextPropertyCalls = 0;
  CR_STATUS Status = Namespace(Fdt);
  CR_STATUS Want;
  if (!strcmp(Expected, "success")) Want = CR_SUCCESS;
  else if (!strcmp(Expected, "unsupported")) Want = CR_UNSUPPORTED;
  else if (!strcmp(Expected, "bad")) Want = CR_COMPROMISED_DATA;
  else { free(Fdt); return 2; }
  printf("%s nodes=%zu properties=%zu status=%zx expected=%zx "
         "parent=%zu first_subnode=%zu next_subnode=%zu next_node=%zu "
         "first_property=%zu next_property=%zu\n", Path, Nodes, Properties,
         (size_t)Status, (size_t)Want, ParentCalls, FirstSubnodeCalls,
         NextSubnodeCalls, NextNodeCalls, FirstPropertyCalls, NextPropertyCalls);
  free(Fdt);
  if (Status != Want) {
    fprintf(stderr, "SEMANTIC_REGRESSION: %s\n", Path);
    return 1;
  }
  if (Performance && (Status != CR_SUCCESS || ParentCalls || FirstSubnodeCalls ||
      NextSubnodeCalls || NextNodeCalls > Nodes + 1 ||
      FirstPropertyCalls > Nodes || NextPropertyCalls > Properties)) {
    fprintf(stderr, "PERF_REGRESSION: namespace repeated a tree/property traversal: %s\n", Path);
    return 1;
  }
  return 0;
}

int main(int Argc, char **Argv) {
  if (Argc < 4 || (Argc - 1) % 3) return 2;
  for (int I = 1; I < Argc; I += 3) {
    int Result = Check(Argv[I], Argv[I + 1], !strcmp(Argv[I + 2], "performance"));
    if (Result) return Result;
  }
  printf("PASS namespace %d cases\n", (Argc - 1) / 3);
  return 0;
}
