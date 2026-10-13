/** Qualcomm SMEM v11/v12 reader and allocator. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define SMEM_HOST_APPS 0U
#define SMEM_HOST_ANY 0xffffffffU
#define SMEM_GLOBAL_HOST 0xfffeU
#define SMEM_HOST_COUNT 25U
#define SMEM_ITEM_FIRST_DYNAMIC 8U

typedef struct {
  UINT32 Offset;
  UINT32 Size;
  UINT32 Cacheline;
} SmemPartition;

typedef struct {
  volatile UINT8 *Base;
  UINTN Size;
  UINT64 PhysicalBase;
  UINT32 Version;
  UINT32 ItemCount;
  /* Exclusive legacy-heap bound: region, UINT32 cursor, table and every
   * nonempty partition, including partitions not visible to APPS. */
  UINTN LegacyHeapLimit;
  SmemPartition Global;
  SmemPartition Hosts[SMEM_HOST_COUNT];
} SmemContext;

/* Caller owns the mapping and context lifetime. Mapping must be coherent or
 * uncached shared normal memory. Returned data is remote-owned and may change.
 * Lookup observes publication fences. A firmware reset/reformat requires
 * reinitialization.
 * Size may be NULL; Ptr is required. Host must be ANY or below HOST_COUNT.
 * Context must not overlap the SMEM mapping. Failure clears output arguments.
 */
CR_STATUS SmemInitialize(SmemContext *Context, VOID *Base, UINTN Size,
                         UINT64 PhysicalBase);
CR_STATUS SmemGetItem(SmemContext *Context, UINT32 Host, UINT32 Item,
                      UINT32 *Size, VOID **Ptr);

/* Allocate an SMEM item in the requested host's APPS/private partition when
 * present, otherwise the v12 global partition or v11 legacy global heap.
 * Host ANY always selects the global partition/heap. A present private
 * partition is never bypassed when full. The caller MUST hold the global
 * remote SMEM hardware mutex and serialize local callers for this entire
 * call; the portable core does not acquire either lock. Metadata is written
 * before the allocation cursor/allocated flag is published, matching Linux's
 * qcom_smem_alloc() ordering. A duplicate (cached or uncached) returns CR_BUSY;
 * other failures also clear Ptr and leave shared memory untouched. New
 * payload bytes are not initialized. Initialization clears the context, so
 * callers must initialize before installing or using any allocator wrapper. */
CR_STATUS SmemAllocateItem(SmemContext *Context, UINT32 Host, UINT32 Item,
                           UINT32 Size, VOID **Ptr);

/* Qualcomm TCSR write-owner/readback mutex. One APPS provider must own this
 * context and serialize all local callers. Fields remain immutable while
 * Held is true, including after a failed release. Keep the register mapping
 * and context until release succeeds; never clear a remote owner's lock.
 * Acquire refuses a retained owner. Release retries ownership recovery and
 * accepts an already-cleared register, without requiring readback after our
 * unlock (a remote processor may immediately acquire it).
 * TimeoutUs is 1..1000000 and PollUs is 1..TimeoutUs. Windows callers must
 * remain at PASSIVE_LEVEL throughout acquisition/release. */
typedef struct {
  UINTN Address;
  UINTN Size;
  UINT32 Owner;
  UINT32 TimeoutUs;
  UINT32 PollUs;
  BOOLEAN Held;
} SmemHardwareMutex;

CR_STATUS SmemHardwareMutexAcquire(SmemHardwareMutex *Mutex);
CR_STATUS SmemHardwareMutexRelease(SmemHardwareMutex *Mutex);
