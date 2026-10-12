/** @file
  Qualified MDSS resource/DMA/panel provider used by the display miniport.
  SPDX-License-Identifier: MIT
**/
#pragma once
#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <Library/MdssCrLib.h>
#define CR_MDSS_PLATFORM_VERSION 6
#define CR_MDSS_BOARD_HDK8450 0x84500001U
/* {25642ED3-9A78-47D6-9315-B93F79865C85} */
static const GUID GUID_CR_MDSS_PLATFORM_INTERFACE =
  {0x25642ed3,0x9a78,0x47d6,{0x93,0x15,0xb9,0x3f,0x79,0x86,0x5c,0x85}};
/* Permanent PEP endpoint, discovered uniquely; its PDO owns all pipe MMIO. */
static const GUID GUID_DEVINTERFACE_CR_MDSS_PLATFORM =
  {0x7b752354,0x6062,0x4f1e,{0xa4,0x96,0x40,0x93,0x79,0x85,0x42,0x6e}};
typedef struct {
  /* Assignment metadata only. Consumers access hardware exclusively through
     Io so permanent quarantine can close and drain every borrowed reader. */
  PVOID Dpu,Dsi;
  UINTN DpuBytes,DsiBytes;
  CR_MDSS_IO Io; /* Complete checked group + legacy callbacks, PASSIVE_LEVEL */
} CR_MDSS_PIPE_MAPPING;
typedef struct {
  PVOID Cpu;
  UINT64 Physical;
  UINTN Bytes;
  PVOID Context;
  /* Nonpaged, lock-free admission at any IRQL including HIGH_LEVEL. A TRUE
     Enter must be paired with Leave after the final memory access/barrier.
     FALSE forbids dereferencing Cpu. Both callbacks perform metadata only.
     The permanent provider closes admission and drains entered writers
     before DMA retirement or forced-assignment quarantine. */
  BOOLEAN (*Enter)(PVOID);
  VOID (*Leave)(PVOID);
} CR_MDSS_FRAMEBUFFER_MAPPING;
typedef struct {
  ULONG Size, Revision, Board;
  CR_MDSS_MODE Mode;
  UINT64 DpuPhysical, DsiPhysical, FrameBufferPhysical;
  UINTN FrameBufferBytes;
  /* Immutable provider identity for this boot. A lease cannot survive
     replacement with another provider or stale firmware DMA ownership. */
  UINT64 BootGeneration;
} CR_MDSS_PLATFORM_CONFIG;
typedef struct {
  INTERFACE Header;
  /* All operations are PASSIVE_LEVEL. Query is pure metadata/health and
     succeeds only when native firmware ownership transfer, Clock/RPMh/ICB,
     LT9611/I2C and OS DMA translation have been explicitly qualified. */
  NTSTATUS (*Query)(PVOID,CR_MDSS_PLATFORM_CONFIG *);
  NTSTATUS (*Acquire)(PVOID,CONST VOID *Owner,
                      CONST CR_MDSS_PLATFORM_CONFIG *,
                      UINT64 *DeviceAddress,UINT32 *HostClockTiming,
                      BOOLEAN *Owned);
  NTSTATUS (*PreparePanel)(PVOID,CONST VOID *Owner);
  NTSTATUS (*UnpreparePanel)(PVOID,CONST VOID *Owner);
  NTSTATUS (*Release)(PVOID,CONST VOID *Owner);
  /* Atomic transfer of the live scanout, DMA mapping and complete resource
     lease to a provider-owned POST lifetime. Success removes Owner from all
     provider records. It survives interface dereference and device removal;
     the next qualified display acquisition must take over/fence that lease.
     Failure must leave Owner's lease fully intact. */
  NTSTATUS (*TransferToPost)(PVOID,CONST VOID *Owner);
  /* Borrow permanent, assigned parent I/O after a successful Acquire.
     They stay pinned and powered through the owner's complete cleanup or
     POST transfer; the consumer must never unmap or retain them afterward.
     Never dereference Dpu/Dsi directly: provider Io gates/drains access on
     forced resource reassignment. No duplicate consumer MMIO assignment. */
  NTSTATUS (*GetPipe)(PVOID,CONST VOID *Owner,CR_MDSS_PIPE_MAPPING *);
  /* Borrow the permanent provider's sole uncached reserved framebuffer alias
     for this immutable configuration. This PASSIVE query grants no memory
     access: Enter succeeds only while the actual DMA/resource lease is live.
     Query before Acquire permits construction of the hardware core without
     touching pixels. Hold this interface reference for the complete borrow;
     consumers never map/unmap the physical framebuffer themselves. After
     Release/POST, clear the borrow before dereferencing this interface. */
  NTSTATUS (*GetFrameBuffer)(PVOID,CONST CR_MDSS_PLATFORM_CONFIG *,CR_MDSS_FRAMEBUFFER_MAPPING *);
} CR_MDSS_PLATFORM_INTERFACE;
/* Successful IRP_MN_QUERY_INTERFACE returns one owned interface reference.
   Failed partial Acquire still sets Owned. The stable Owner is the display
   adapter context; failed Release retains that owner and provider reference.
   Providers must serialize native/legacy/PEP votes with this absolute lease.
   Query has no default physical=IOVA path and cannot publish an empty lease.
   Query zeroes all padding, returns immutable configuration for this boot,
   and qualifies the permanent uncached CPU framebuffer mapping with no
   cacheable aliases. POST keeps the mapping and framebuffer geometry. */
#endif
