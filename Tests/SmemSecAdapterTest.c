/** UEFI SMEM memory-map adapter regression. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Library/SmemSecLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapHelperLib.h>

VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...)
{
  (void)Level;
  (void)Format;
}

static UINT32 Heap[0x4000 / 4];
static ARM_MEMORY_REGION_DESCRIPTOR_EX Region;
static EFI_STATUS LookupStatus;

EFI_STATUS EFIAPI LocateMemoryMapAreaByName (
  CHAR8 *Name, ARM_MEMORY_REGION_DESCRIPTOR_EX *Result)
{
  assert(strcmp(Name, "SMEM") == 0);
  if (EFI_ERROR(LookupStatus)) return LookupStatus;
  *Result = Region;
  return EFI_SUCCESS;
}

VOID *EFIAPI ZeroMem(VOID *Buffer, UINTN Length)
{
  return memset(Buffer, 0, Length);
}

VOID EFIAPI MemoryFence(VOID)
{
  __sync_synchronize();
}

int main(void)
{
  SmemLibContext Context;
  SmemLibContext Empty = {0};
  VOID *Item = (VOID *)1;
  UINT32 Size = 99;

  assert(SmemLibInit(NULL) == EFI_INVALID_PARAMETER);
  memset(&Context, 0xa5, sizeof(Context));
  LookupStatus = EFI_NOT_FOUND;
  assert(SmemLibInit(&Context) == EFI_NOT_FOUND);
  assert(memcmp(&Context, &Empty, sizeof(Context)) == 0);

  LookupStatus = EFI_SUCCESS;
  Region.Address = MAX_UINTN - 16;
  Region.Length = 0x4000;
  assert(SmemLibInit(&Context) == EFI_INVALID_PARAMETER);
  Region.Address = (UINTN)Heap;
  Region.Length = sizeof(Heap);
  Heap[192 / 4] = 1;
  Heap[92 / 4] = 11U << 16;
  /* A v11 heap without an optional private partition table. */
  Heap[(208 + 137 * 16) / 4] = 1;
  Heap[(208 + 137 * 16 + 4) / 4] = 0x3000;
  Heap[(208 + 137 * 16 + 8) / 4] = 8;
  assert(SmemLibInit(&Context) == EFI_SUCCESS);
  assert(SmemGetItemPtr(&Context, SMEM_HOST_ANY, 137, &Size, &Item) == EFI_SUCCESS);
  assert(Item == (UINT8 *)Heap + 0x3000 && Size == 8);
  assert(SmemGetItemPtr(&Context, SMEM_HOST_ANY, 137, NULL, &Item) == EFI_SUCCESS);
  assert(SmemGetItemPtr(&Context, SMEM_HOST_ANY, 138, &Size, &Item) == EFI_NOT_FOUND);
  assert(Item == NULL && Size == 0);
  puts("SMEM SEC adapter: region errors, overflow, legacy API and core lookup passed");
  return 0;
}
