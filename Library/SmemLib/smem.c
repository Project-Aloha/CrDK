/** SMEM v11/v12 reader and allocator. SPDX-License-Identifier: MIT
 * Copyright (c) 2025-2026, Project Aloha Authors.
 * Independently implemented from the shared-memory format.
 */
#include <Library/smem.h>
#include <oskal/cr_debug.h>

#define HEADER_SIZE 8400U
#define TABLE_SIZE 4096U
#define TOC_MAGIC 0x434f5424U
#define PART_MAGIC 0x54525024U
#define INFO_MAGIC 0x49494953U
#define SMEM_PRIVATE_CANARY 0xa5a5U
#define SMEM_ITEM_LAST_FIXED SMEM_ITEM_FIRST_DYNAMIC

/* Qualcomm SMEM is a little-endian shared-memory ABI.  The live publication
 * words below are naturally aligned UINT32 stores by the remote allocator;
 * read each one with a single volatile load so a concurrent update cannot be
 * observed as four torn byte loads.  This library is intentionally not
 * supported on a big-endian build. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "SMEM requires a little-endian target"
#endif
#if defined(__AARCH64EB__) || defined(__ARM_BIG_ENDIAN)
#error "SMEM requires a little-endian target"
#endif

STATIC UINT32 Read32(volatile UINT8 *P)
{
  /* SMEM is little-endian and some firmware tables are not naturally
   * aligned. Byte loads keep the reader defined on ARM and host fixtures. */
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) |
         ((UINT32)P[3] << 24);
}
STATIC UINT32 Read32Aligned(volatile UINT8 *P)
{
  /* Every caller proves P is 32-bit aligned before reaching this helper. */
  return *(volatile UINT32 *)(VOID *)P;
}
STATIC UINT16 Read16(volatile UINT8 *P)
{
  return (UINT16)P[0] | ((UINT16)P[1] << 8);
}
STATIC VOID Acquire(VOID)
{
#ifdef _KERNEL_MODE
  KeMemoryBarrier();
#else
  MemoryFence();
#endif
}
STATIC VOID Release(VOID)
{
#ifdef _KERNEL_MODE
  KeMemoryBarrier();
#else
  MemoryFence();
#endif
}
STATIC VOID Write16(volatile UINT8 *P, UINT16 Value)
{
  P[0] = (UINT8)Value;
  P[1] = (UINT8)(Value >> 8);
}
STATIC VOID Write32Aligned(volatile UINT8 *P, UINT32 Value)
{
  /* Publication words and legacy TOC fields are naturally aligned.  Keep the
   * store single-copy atomic on the target; the release fence before the
   * publication store prevents readers from observing a partial entry. */
  *(volatile UINT32 *)(VOID *)P = Value;
}
STATIC BOOLEAN Align8(UINT32 Size, UINT32 *Aligned)
{
  if (Aligned == NULL || Size > 0xfffffff8U) return FALSE;
  *Aligned = (Size + 7U) & ~7U;
  return TRUE;
}
STATIC BOOLEAN Fits(UINTN Offset, UINTN Length, UINTN Limit)
{
  return Offset <= Limit && Length <= Limit - Offset;
}
STATIC CR_STATUS PartitionCheck(SmemContext *C, SmemPartition *P,
                                 UINT16 H0, UINT16 H1)
{
  volatile UINT8 *B = C->Base + P->Offset;
  UINT32 U, K;
  UINT32 Magic = Read32(B);
  UINT16 ActualH0 = Read16(B + 4), ActualH1 = Read16(B + 6);
  UINT32 ActualSize = Read32(B + 8);
  U = Read32Aligned(B + 12);
  K = Read32Aligned(B + 16);
  Acquire();
  log_info("SMEM partition header offset=0x%x magic=0x%08x size=0x%x",
          P->Offset, Magic, ActualSize);
  log_info("SMEM partition hosts=%u:%u expected=%u:%u expected-size=0x%x",
          ActualH0, ActualH1, H0, H1, P->Size);
  log_info("SMEM partition cacheline=0x%x free-uncached=0x%x free-cached=0x%x",
          P->Cacheline, U, K);
  if (Magic != PART_MAGIC || ActualH0 != H0 || ActualH1 != H1 ||
      ActualSize != P->Size) {
    log_err("SMEM partition header mismatch offset=0x%x", P->Offset);
    return CR_DEVICE_ERROR;
  }
  /* Linux qcom_smem_partition_header() checks only the uncached cursor.
   * Cached metadata is needed only after an uncached lookup misses. Check
   * its bounds/alignment there, before reading any cached entry. */
  if (P->Size < 32 || U < 32 || U > P->Size) {
    log_err("SMEM partition uncached cursor out of range offset=0x%x "
            "uncached=0x%x size=0x%x", P->Offset, U, P->Size);
    return CR_DEVICE_ERROR;
  }
  return CR_SUCCESS;
}

CR_STATUS SmemInitialize(SmemContext *C, VOID *Base, UINTN Size,
                         UINT64 PhysicalBase)
{
  UINTN T, I, J;
  UINT32 N, Initialized, Reserved, Version, TableMagic, TableVersion;
  SmemContext Empty = {0};
  if (C == NULL) return CR_INVALID_PARAMETER;
  *C = Empty;
  if (Base == NULL || Size < HEADER_SIZE || ((UINTN)Base & 3) ||
      (Size & 3) || ((UINT64)PhysicalBase & 3) ||
      Size > (UINTN)-1 - (UINTN)Base ||
      (UINT64)Size > (UINT64)-1 - PhysicalBase)
    return CR_INVALID_PARAMETER;
  C->Base = Base;
  C->Size = Size;
  C->PhysicalBase = PhysicalBase;
  C->LegacyHeapLimit = Size < 0xffffffffU ? Size : 0xffffffffU;
  log_info("SMEM init begin base=%p physical=0x%llx size=0x%llx", Base,
          PhysicalBase, (UINT64)Size);
  Initialized = Read32Aligned(C->Base + 192);
  Acquire();
  Reserved = Read32(C->Base + 204);
  Version = Read32(C->Base + 92);
  T = Size - TABLE_SIZE;
  TableMagic = Read32(C->Base + T);
  TableVersion = Read32(C->Base + T + 4);
  N = Read32(C->Base + T + 8);
  /* Print raw values even when the first header check fails. Keep each
   * message short enough for the serial DebugLib's 256-byte buffer. */
  log_info("SMEM init header initialized=0x%08x reserved=0x%08x SBL-version=0x%08x",
          Initialized, Reserved, Version);
  log_info("SMEM init heap free-offset=0x%x available=0x%x",
          Read32Aligned(C->Base + 196), Read32Aligned(C->Base + 200));
  log_info("SMEM init ptable offset=0x%llx magic=0x%08x version=%u entries=%u",
          (UINT64)T, TableMagic, TableVersion, N);
  if (Initialized != 1 || Reserved != 0) {
    log_err("SMEM init rejected SBL header");
    goto Corrupt;
  }
  C->Version = Version >> 16;
  C->ItemCount = 512;
  if (C->Version != 11 && C->Version != 12) {
    log_err("SMEM init unsupported version=%u", C->Version);
    *C = Empty;
    return CR_UNSUPPORTED;
  }
  if (TableMagic != TOC_MAGIC) {
    log_warn("SMEM init ptable magic absent");
    if (C->Version == 11) {
      log_info("SMEM init v11 legacy heap accepted without ptable");
      return CR_SUCCESS;
    }
    goto Corrupt;
  }
  if (TableVersion != 1) {
    log_err("SMEM init unsupported ptable version=%u", TableVersion);
    goto Corrupt;
  }
  if (N > (TABLE_SIZE - 32) / 48) {
    log_err("SMEM init ptable entry count out of range entries=%u", N);
    goto Corrupt;
  }
  if (T < C->LegacyHeapLimit) C->LegacyHeapLimit = T;
  for (I = 0; I < N; ++I) {
    volatile UINT8 *E = C->Base + T + 32 + I * 48;
    SmemPartition P;
    SmemPartition *Dest = NULL;
    UINT16 H0 = Read16(E + 12), H1 = Read16(E + 14);
    P.Offset = Read32(E); P.Size = Read32(E + 4);
    P.Cacheline = Read32(E + 16);
    log_info("SMEM ptable entry=%u offset=0x%x size=0x%x flags=0x%x",
            (UINT32)I, P.Offset, P.Size, Read32(E + 8));
    log_info("SMEM ptable hosts=%u:%u cacheline=0x%x", H0, H1, P.Cacheline);
    /* Like Linux, skip empty slots and the offset-zero legacy-area entry. */
    if (P.Offset == 0 || P.Size == 0) continue;
    /* The v11 heap grows upward before the private partitions. Lookup only
     * maps APPS partitions, but allocation must protect every partition and
     * the table, including memory owned by other remote host pairs. This
     * conservative bound also blocks writes for malformed overlapping
     * foreign entries without rejecting otherwise usable read-only data. */
    if (P.Offset < C->LegacyHeapLimit) C->LegacyHeapLimit = P.Offset;
    if (C->Version == 12 && H0 == SMEM_GLOBAL_HOST && H1 == SMEM_GLOBAL_HOST)
      Dest = &C->Global;
    else if (H0 == SMEM_HOST_APPS && H1 < SMEM_HOST_COUNT) Dest = &C->Hosts[H1];
    else if (H1 == SMEM_HOST_APPS && H0 < SMEM_HOST_COUNT) Dest = &C->Hosts[H0];
    /* Linux only maps the global partition and partitions involving the
     * local APPS host. Ignore other host pairs, even if their entries are
     * stale or use a layout this firmware reader cannot map. */
    if (Dest == NULL) continue;
    log_info("SMEM selected entry=%u kind=%u", (UINT32)I,
            Dest == &C->Global ? 1U : 2U);
    if ((P.Offset & 3) != 0) {
      log_err("SMEM selected partition offset is not 32-bit aligned "
              "entry=%u offset=0x%x", (UINT32)I, P.Offset);
      goto Corrupt;
    }
    if (P.Offset < (C->Version == 11 ? HEADER_SIZE : 208U) ||
        !Fits(P.Offset, P.Size, T) || P.Size < 32) {
      log_err("SMEM selected partition bounds invalid entry=%u offset=0x%x "
              "size=0x%x ptable-offset=0x%llx",
              (UINT32)I, P.Offset, P.Size, (UINT64)T);
      goto Corrupt;
    }
    for (J = 0; J < I; ++J) {
      volatile UINT8 *Prev = C->Base + T + 32 + J * 48;
      UINT16 PH0 = Read16(Prev + 12), PH1 = Read16(Prev + 14);
      UINT32 O = Read32(Prev), S = Read32(Prev + 4);
      BOOLEAN Relevant = (C->Version == 12 && PH0 == SMEM_GLOBAL_HOST &&
                          PH1 == SMEM_GLOBAL_HOST) ||
                         (PH0 == SMEM_HOST_APPS && PH1 < SMEM_HOST_COUNT) ||
                         (PH1 == SMEM_HOST_APPS && PH0 < SMEM_HOST_COUNT);
      if (Relevant && O && S && P.Offset < (UINT64)O + S &&
          O < (UINT64)P.Offset + P.Size) {
        log_err("SMEM selected partition overlap entry=%u previous=%u",
                (UINT32)I, (UINT32)J);
        goto Corrupt;
      }
    }
    if (Dest->Size) {
      log_err("SMEM duplicate selected partition entry=%u hosts=%u:%u",
              (UINT32)I, H0, H1);
      goto Corrupt;
    }
    if (CR_ERROR(PartitionCheck(C, &P, H0, H1))) {
      log_err("SMEM selected partition validation failed entry=%u", (UINT32)I);
      goto Corrupt;
    }
    *Dest = P;
  }
  I = T + 32 + (UINTN)N * 48;
  if (C->Version == 12) {
    if (!C->Global.Size) {
      log_err("SMEM init v12 missing global partition");
      goto Corrupt;
    }
    if (Fits(I, 18, Size) && Read32(C->Base + I) == INFO_MAGIC) {
      C->ItemCount = Read16(C->Base + I + 16);
      log_info("SMEM init SIII item-count=%u", C->ItemCount);
      if (!C->ItemCount) {
        log_warn("SMEM init invalid zero SIII item-count; using legacy 512");
        C->ItemCount = 512;
      }
    } else {
      log_warn("SMEM init SIII absent or invalid; using legacy item-count=512");
    }
  }
  log_info("SMEM init complete version=%u item-count=%u global-size=0x%x",
          C->Version, C->ItemCount, C->Global.Size);
  return CR_SUCCESS;
Corrupt:
  log_err("SMEM init failed status=DeviceError");
  *C = Empty;
  return CR_DEVICE_ERROR;
}

STATIC CR_STATUS GetPartition(SmemContext *C, SmemPartition *P, UINT32 Item,
                               UINT32 *Size, VOID **Ptr)
{
  volatile UINT8 *B = C->Base + P->Offset;
  UINTN U = Read32Aligned(B + 12), K = Read32Aligned(B + 16), Pos, Step;
  Acquire();
  if (P->Size < 32 || U < 32 || U > P->Size)
    return CR_DEVICE_ERROR;
  for (Pos = 32; Pos < U;) {
    UINT32 Len;
    UINT16 Pad, HPad;
    UINTN Data;
    if (!Fits(Pos, 16, U) || Read16(B + Pos) != 0xa5a5) return CR_DEVICE_ERROR;
    Len = Read32(B + Pos + 4); Pad = Read16(B + Pos + 8);
    HPad = Read16(B + Pos + 10);
    Data = Pos + 16 + HPad;
    if (Pad > Len || Data < Pos || !Fits(Data, Len, U))
      return CR_DEVICE_ERROR;
    if (Read16(B + Pos + 2) == Item) {
      *Size = Len - Pad; *Ptr = (VOID *)(B + Data); return CR_SUCCESS;
    }
    Pos = Data + Len;
  }
  /* Uncached items do not depend on the cached list or its alignment. Linux
   * also checks the cached cursor only after exhausting uncached items. */
  if (K < 32 || K > P->Size) {
    log_err("SMEM lookup cached cursor out of range offset=0x%x cached=0x%llx size=0x%x",
            P->Offset, (UINT64)K, P->Size);
    return CR_DEVICE_ERROR;
  }
  if (K == P->Size) return CR_NOT_FOUND;
  /* ALIGN(sizeof(entry), cacheline) assumes a nonzero power of two. Never
   * guess a cached-entry stride when the table does not supply one. */
  if (P->Cacheline == 0 || (P->Cacheline & (P->Cacheline - 1)) != 0) {
    log_err("SMEM lookup invalid cached alignment offset=0x%x cacheline=0x%x",
            P->Offset, P->Cacheline);
    return CR_DEVICE_ERROR;
  }
  Step = P->Cacheline < 16 ? 16 : P->Cacheline;
  Pos = P->Size;
  while (Pos > K) {
    UINTN H, Data;
    UINT32 Len;
    UINT16 Pad;
    if (Step > Pos - K) return CR_DEVICE_ERROR;
    H = Pos - Step;
    if (Read16(B + H) != 0xa5a5) return CR_DEVICE_ERROR;
    Len = Read32(B + H + 4); Pad = Read16(B + H + 8);
    if (Len > H - K || Pad > Len) return CR_DEVICE_ERROR;
    Data = H - Len;
    if (Data & 7) return CR_DEVICE_ERROR;
    if (Read16(B + H + 2) == Item) {
      *Size = Len - Pad; *Ptr = (VOID *)(B + Data); return CR_SUCCESS;
    }
    Pos = Data;
  }
  return CR_NOT_FOUND;
}

STATIC SmemPartition *SelectPartition(SmemContext *C, UINT32 Host)
{
  if (Host != SMEM_HOST_ANY && Host >= SMEM_HOST_COUNT) return NULL;
  if (Host < SMEM_HOST_COUNT && C->Hosts[Host].Size)
    return &C->Hosts[Host];
  if (C->Version == 12 && C->Global.Size)
    return &C->Global;
  return NULL;
}

STATIC CR_STATUS AllocatePartition(SmemContext *C, SmemPartition *P,
                                   UINT32 Item, UINT32 Size, VOID **Ptr)
{
  volatile UINT8 *B;
  UINT32 ExistingSize;
  VOID *Existing;
  UINT32 DataSize;
  UINTN U, K, AllocationSize, Data;
  CR_STATUS Status;

  B = C->Base + P->Offset;
  U = Read32Aligned (B + 12);
  K = Read32Aligned (B + 16);
  Acquire ();
  if (P->Size < 32 || (U & 7) != 0 || (K & 3) != 0 || U < 32 ||
      U > P->Size || K > P->Size || U > K) {
    log_err ("SMEM allocate invalid cursors offset=0x%x uncached=0x%llx "
             "cached=0x%llx size=0x%x", P->Offset, (UINT64)U,
             (UINT64)K, P->Size);
    return CR_DEVICE_ERROR;
  }
  /* Reuse the reader's complete validation of the existing linked list.  A
   * malformed list is never extended, since doing so would publish an
   * allocation that remote readers cannot safely walk. */
  ExistingSize = 0;
  Existing = NULL;
  Status = GetPartition (C, P, Item, &ExistingSize, &Existing);
  if (Status == CR_SUCCESS) {
    return CR_BUSY;
  }
  if (Status != CR_NOT_FOUND) {
    return Status;
  }
  if (Item > 0xffffU || !Align8 (Size, &DataSize)) {
    return CR_INVALID_PARAMETER;
  }

  AllocationSize = (UINTN)DataSize + 16U;
  if (AllocationSize < DataSize || !Fits (U, AllocationSize, K)) {
    log_err ("SMEM allocate out of space item=%u size=0x%x offset=0x%x "
             "uncached=0x%llx cached=0x%llx", Item, Size, P->Offset,
             (UINT64)U, (UINT64)K);
    return CR_OUT_OF_RESOURCES;
  }
  Data = U + 16U;
  /* The data is deliberately left untouched, matching Linux's allocator.
   * Consumers own initialization; only the private-entry metadata is part of
   * the SMEM publication protocol. */
  Write16 (B + U, SMEM_PRIVATE_CANARY);
  Write16 (B + U + 2, (UINT16)Item);
  Write32Aligned (B + U + 4, DataSize);
  Write16 (B + U + 8, (UINT16)(DataSize - Size));
  Write16 (B + U + 10, 0);
  Write32Aligned (B + U + 12, 0);
  Release ();
  Write32Aligned (B + 12, (UINT32)(U + AllocationSize));
  *Ptr = (VOID *)(B + Data);
  log_info ("SMEM allocate partition item=%u size=0x%x offset=0x%x "
           "entry=0x%llx data=0x%llx next=0x%llx", Item, Size, P->Offset,
           (UINT64)U, (UINT64)Data, (UINT64)(U + AllocationSize));
  return CR_SUCCESS;
}

STATIC CR_STATUS AllocateLegacy(SmemContext *C, UINT32 Item, UINT32 Size,
                                VOID **Ptr)
{
  volatile UINT8 *E;
  UINT32 DataSize;
  UINT32 FreeOffset;
  UINT32 Available;

  if (Item > 0xffffU || !Align8 (Size, &DataSize))
    return CR_INVALID_PARAMETER;
  E = C->Base + 208U + (UINTN)Item * 16U;
  if (!Fits (208U + (UINTN)Item * 16U, 16U, C->Size)) {
    return CR_INVALID_PARAMETER;
  }
  if (Read32Aligned (E) != 0)
    return CR_BUSY;
  FreeOffset = Read32Aligned (C->Base + 196U);
  Available = Read32Aligned (C->Base + 200U);
  Acquire ();
  /* Validate the entire advertised free range, not just this request. The
   * next cursor is UINT32 on the wire even on 64-bit hosts, and corrupt
   * cursors must never send clients into the TOC, a partition or the ptable. */
  if (FreeOffset < HEADER_SIZE || (FreeOffset & 7U) != 0 ||
      !Fits (FreeOffset, Available, C->LegacyHeapLimit) ||
      Available > 0xffffffffU - FreeOffset) {
    log_err ("SMEM allocate legacy invalid heap free=0x%x available=0x%x "
             "limit=0x%llx", FreeOffset, Available,
             (UINT64)C->LegacyHeapLimit);
    return CR_DEVICE_ERROR;
  }
  if (DataSize > Available) {
    log_err ("SMEM allocate legacy out of space item=%u size=0x%x "
             "free=0x%x available=0x%x region=0x%llx", Item, Size,
             FreeOffset, Available, (UINT64)C->Size);
    return CR_OUT_OF_RESOURCES;
  }
  Write32Aligned (E + 4, FreeOffset);
  Write32Aligned (E + 8, DataSize);
  Write32Aligned (E + 12, 0);
  Release ();
  Write32Aligned (E, 1);
  Write32Aligned (C->Base + 196U, FreeOffset + DataSize);
  Write32Aligned (C->Base + 200U, Available - DataSize);
  *Ptr = (VOID *)(C->Base + FreeOffset);
  log_info ("SMEM allocate legacy item=%u size=0x%x offset=0x%x next=0x%x",
           Item, Size, FreeOffset, FreeOffset + DataSize);
  return CR_SUCCESS;
}

CR_STATUS SmemAllocateItem(SmemContext *C, UINT32 Host, UINT32 Item,
                           UINT32 Size, VOID **Ptr)
{
  SmemPartition *P;

  if (Ptr != NULL) *Ptr = NULL;
  if (C == NULL || Ptr == NULL || C->Base == NULL ||
      (Host != SMEM_HOST_ANY && Host >= SMEM_HOST_COUNT) ||
      Item < SMEM_ITEM_LAST_FIXED || Item >= C->ItemCount)
    return CR_INVALID_PARAMETER;

  P = SelectPartition (C, Host);
  if (P != NULL)
    return AllocatePartition (C, P, Item, Size, Ptr);
  if (C->Version == 11)
    return AllocateLegacy (C, Item, Size, Ptr);
  return CR_UNSUPPORTED;
}

CR_STATUS SmemGetItem(SmemContext *C, UINT32 Host, UINT32 Item,
                      UINT32 *Size, VOID **Ptr)
{
  volatile UINT8 *E;
  UINT32 Offset, Length, Aux, IgnoredSize;
  if (Size == NULL) Size = &IgnoredSize;
  if (Size != NULL) *Size = 0;
  if (Ptr != NULL) *Ptr = NULL;
  if (C == NULL || Size == NULL || Ptr == NULL || C->Base == NULL ||
      (Host != SMEM_HOST_ANY && Host >= SMEM_HOST_COUNT)) return CR_INVALID_PARAMETER;
  if (Item >= C->ItemCount) return CR_INVALID_PARAMETER;
  if (Host < SMEM_HOST_COUNT && C->Hosts[Host].Size)
    return GetPartition(C, &C->Hosts[Host], Item, Size, Ptr);
  if (C->Version == 12) return GetPartition(C, &C->Global, Item, Size, Ptr);
  E = C->Base + 208 + (UINTN)Item * 16;
  if (Read32Aligned(E) == 0) return CR_NOT_FOUND;
  Acquire();
  Offset = Read32(E + 4); Length = Read32(E + 8);
  Aux = Read32(E + 12) & 0xfffffffcU;
  /* The SMEM global-entry aux_base field is a 32-bit physical-region
   * selector.  Linux compares it with the low 32 bits of the region's
   * physical base; retaining the full UINT64 comparison rejects valid
   * layouts when DRAM is above the 4 GiB boundary. */
  if (Aux != 0 && Aux != (UINT32)C->PhysicalBase) return CR_UNSUPPORTED;
  if (!Fits(Offset, Length, C->Size)) return CR_DEVICE_ERROR;
  *Size = Length; *Ptr = (VOID *)(C->Base + Offset);
  return CR_SUCCESS;
}
