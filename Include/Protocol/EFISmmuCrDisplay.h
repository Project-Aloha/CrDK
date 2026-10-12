/* Typed native display ownership, separate from ordinary EFI DMA mapping.
 * SPDX-License-Identifier: MIT */
#ifndef EFI_SMMU_CR_DISPLAY_H_
#define EFI_SMMU_CR_DISPLAY_H_
#include <Uefi.h>
typedef struct _EFI_SMMU_CR_PROTOCOL EFI_SMMU_CR_PROTOCOL;

#define EFI_SMMU_CR_DISPLAY_OWNERSHIP_REVISION 0x0001000BULL
#define EFI_SMMU_CR_DISPLAY_BUFFER_INFO_REVISION 1U

/* Stable, initially zeroed caller storage. Copies cannot operate on the
 * original producer journal; generations never identify another lifetime. */
typedef struct { VOID *Token; UINT64 Generation; } EFI_SMMU_CR_DISPLAY_GROUP;
typedef struct { VOID *Token; UINT64 Generation; } EFI_SMMU_CR_DISPLAY_LEASE;
typedef enum { CrSmmuDisplayFrame, CrSmmuDisplayCommand } EFI_SMMU_CR_DISPLAY_KIND;

#define CR_SMMU_DISPLAY_ALLOCATED 1U
#define CR_SMMU_DISPLAY_CPU_READY 2U
#define CR_SMMU_DISPLAY_MAPPING 4U
#define CR_SMMU_DISPLAY_READY 8U
#define CR_SMMU_DISPLAY_CLEANUP_ONLY 16U
#define CR_SMMU_DISPLAY_ALLOCATION_UNCERTAIN 32U
#define CR_SMMU_DISPLAY_BORROWED_FRAME 64U
#define CR_SMMU_DISPLAY_OWNER_READY 128U
typedef struct {
  UINT32 Size, Revision, Kind, Flags;
  UINT64 Generation, OwnerGeneration;
  EFI_PHYSICAL_ADDRESS HostAddress, DeviceAddress;
  UINTN Bytes, PageBytes;
  UINT32 GrantedMask;
  EFI_STATUS LastStatus;
} EFI_SMMU_CR_DISPLAY_BUFFER_INFO;

/* The owner retains its display mutex and sealed software writers through
 * acquisition. Callback proves the exact incarnation's interrupt/writer
 * admission and physical DPU/VBIF/DSI stop. It runs outside the SMMU lock,
 * may not reenter SMMU services, and accepts only synchronous EFI_SUCCESS.
 * Producer inventories exclusive inherited masked routes/banks/ASIDs,
 * preserves observed globals and retires the group without freeing old
 * backing. Any partial effect pins the journal and excludes other owners.
 * The same exact owner may retry retained stages after fresh stop proof;
 * TLB timeout or poisoned new domains permanently retain failed ownership. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_DISPLAY_VERIFY_STOPPED)(
  IN VOID *Context, IN UINT64 OwnerGeneration);
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_ACQUIRE_DISPLAY_GROUP)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN UINT32 Soc, IN UINT64 OwnerGeneration,
  IN EFI_SMMU_CR_DISPLAY_VERIFY_STOPPED VerifyStopped, IN VOID *Context,
  IN OUT EFI_SMMU_CR_DISPLAY_GROUP *Group);

/* Bind only the existing reserved CPU framebuffer. Physical must equal the
 * retained CPU mapping on this identity-mapped adapter, be page aligned and
 * cover the exact caller-owned reserved allocation. Caller independently
 * excludes aliases/writers and retains these pages across OS takeover.
 * Producer owns mapping/cache journals and never frees this backing. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_BIND_DISPLAY_FRAME)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN EFI_SMMU_CR_DISPLAY_GROUP *Group,
  IN VOID *CpuAddress, IN EFI_PHYSICAL_ADDRESS Physical, IN UINTN Bytes,
  IN OUT EFI_SMMU_CR_DISPLAY_LEASE *Lease);
/* Command pages are separately producer-owned BSData, below4GiB and WC. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_ALLOCATE_DISPLAY_COMMAND)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN EFI_SMMU_CR_DISPLAY_GROUP *Group,
  IN UINTN Bytes, IN OUT EFI_SMMU_CR_DISPLAY_LEASE *Lease);
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_DISPLAY_BUFFER_OPERATION)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN EFI_SMMU_CR_DISPLAY_GROUP *Group,
  IN OUT EFI_SMMU_CR_DISPLAY_LEASE *Lease);
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_QUERY_DISPLAY_BUFFER)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN EFI_SMMU_CR_DISPLAY_GROUP *Group,
  IN EFI_SMMU_CR_DISPLAY_LEASE *Lease,
  OUT EFI_SMMU_CR_DISPLAY_BUFFER_INFO *Info);
/* Query is a locked software journal, without MMIO, cache or stop callbacks.
 * OWNER_READY means the immutable target, exact stable owner, retired group
 * and all four current producer domains remain admitted and unpoisoned.
 * The owner may use this with READY/CPU_READY/MAPPING, complete grants and
 * LastStatus for admission between full Check boundaries while serializing
 * its writers and every typed mutation. Ordinary DMA APIs cannot alter the
 * typed resources. Query does not prove fresh hardware, so full Check is
 * required at pipeline start, interrupt admission, publish and exit freeze.
 * A failed journal remains queryable with OWNER_READY absent. */
/* Prepare grants READ only to exactly the qualified display group. Check
 * proves fresh live routes/leaves/cache attributes, including no other SID
 * grant. Preserve is frame-only and requires Reserved backing. Release must
 * follow independent DMA stop and CPU-writer exclusion; failed revocation
 * retains every mapping/allocation journal. Ordinary Map/Unmap/SetAttribute
 * cannot manipulate typed resources. Successful command release frees its
 * owned pages; successful frame release retains the caller's reserved pages.
 * Check and Query remain read-only during the producer's exit-prepare
 * observers, before revocation. All mutators remain prohibited throughout
 * EBS; Check and Query are prohibited after exit-prepare completes. */
#endif
