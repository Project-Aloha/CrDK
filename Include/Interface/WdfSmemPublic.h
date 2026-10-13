/* Kernel SMEM provider interface. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#include <Library/smem.h>

#define WDF_SMEM_CR_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_DEVINTERFACE_SMEM_CR,0x5c1223a9,0x9708,0x4abf,0x90,0x7b,0x6a,0x7d,0x7e,0x57,0x78,0x28);
DEFINE_GUID(GUID_SMEM_CR_INTERFACE,0xc763208e,0x1186,0x4af2,0x8b,0x88,0x80,0x37,0x9d,0xa8,0xa2,0x95);

/* Initialize to zero. A successful BeginAccess owns one provider rundown
 * reference until EndAccess. All returned pointers are valid only during
 * that lease, and their remote-owned contents may change. Do not copy, move,
 * share concurrently or reuse an active lease. Finish every lease before
 * returning from a client stop/remove callback; provider D0 exit waits for
 * active leases. Hold a query-interface reference throughout use.
 * Begin/End/GetItem allow <=DISPATCH_LEVEL. AllocateItem requires PASSIVE.
 * Duplicate allocation returns STATUS_OBJECT_NAME_COLLISION; use GetItem to
 * retrieve it. A failed release can follow committed allocation: retry lookup
 * rather than assuming shared memory stayed unchanged. */
typedef struct { PVOID Provider; } WDF_SMEM_CR_ACCESS;
typedef NTSTATUS (*SMEM_CR_BEGIN)(PVOID,WDF_SMEM_CR_ACCESS *);
typedef NTSTATUS (*SMEM_CR_END)(PVOID,WDF_SMEM_CR_ACCESS *);
typedef NTSTATUS (*SMEM_CR_GET)(PVOID,CONST WDF_SMEM_CR_ACCESS *,UINT32,UINT32,UINT32 *,PVOID *);
typedef NTSTATUS (*SMEM_CR_ALLOCATE)(PVOID,CONST WDF_SMEM_CR_ACCESS *,UINT32,UINT32,UINT32,PVOID *);
typedef struct {
  INTERFACE Header;
  SMEM_CR_BEGIN BeginAccess;
  SMEM_CR_END EndAccess;
  SMEM_CR_GET GetItem;
  SMEM_CR_ALLOCATE AllocateItem;
} WDF_SMEM_CR_INTERFACE;
