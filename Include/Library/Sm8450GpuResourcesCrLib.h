/** SM8450 A730 register layout arithmetic; no mapping or access authority.
    SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_SM8450_GPU_LAYOUT_VERSION 1U
#define CR_SM8450_GPU_VIEW_COUNT 7U

typedef enum {
  CrSm8450GpuViewGpu = 1,
  CrSm8450GpuViewCxDebug = 2,
  CrSm8450GpuViewGmuLow = 3,
  CrSm8450GpuViewGmuHigh = 4,
  CrSm8450GpuViewRscc = 5,
  CrSm8450GpuViewPdc = 6,
  CrSm8450GpuViewGpucc = 7
} CR_SM8450_GPU_VIEW;

/* Intended composition only: neither value proves ownership or a live map. */
#define CR_SM8450_GPU_RELATION_DEDICATED 1U
#define CR_SM8450_GPU_RELATION_BORROWED 2U

typedef struct {
  UINT32 View;
  UINT32 Relation;
  UINT64 Base;
  UINT64 Length;
} CR_SM8450_GPU_VIEW_DESCRIPTOR;

typedef struct {
  UINT32 Version;
  UINT32 Count;
  UINT32 Reserved[2];
  /* Canonical order, indexed by View - 1. */
  CR_SM8450_GPU_VIEW_DESCRIPTOR Views[CR_SM8450_GPU_VIEW_COUNT];
} CR_SM8450_GPU_LAYOUT;

typedef enum {
  /* msm GPU word offsets: origin 03d00000, within GPU only. */
  CrSm8450GpuNamespaceGpu = 1,
  /* GMU word offsets ALSO have origin 03d00000, not GMU-low.
     May resolve GMU-low, borrowed GPUCC, or GMU-high. */
  CrSm8450GpuNamespaceGmu = 2,
  CrSm8450GpuNamespaceRscc = 3,    /* origin 03d50000 */
  CrSm8450GpuNamespacePdc = 4,     /* origin 0b290000 */
  CrSm8450GpuNamespaceCxDebug = 5, /* origin 03d61000 */
  CrSm8450GpuNamespaceCxMisc = 6,  /* origin 03d9e000, GMU-high slice */
  CrSm8450GpuNamespaceGpucc = 7    /* origin 03d90000, borrowed */
} CR_SM8450_GPU_NAMESPACE;

typedef struct {
  UINT32 View;
  UINT32 ByteOffset;
  UINT32 Bytes;
  UINT32 Relation;
  UINT64 PhysicalAddress;
} CR_SM8450_GPU_REGISTER_SPAN;

CR_STATIC_ASSERT(sizeof(CR_SM8450_GPU_VIEW_DESCRIPTOR) == 24, "GPU view layout");
CR_STATIC_ASSERT(sizeof(CR_SM8450_GPU_LAYOUT) == 184, "GPU layout size");
CR_STATIC_ASSERT(sizeof(CR_SM8450_GPU_REGISTER_SPAN) == 24, "GPU span layout");

/* Validate exactly seven caller-owned immutable descriptors, in any order,
   then normalize into Layout. Exact mainline SM8450 extents are required:
   GPU03d00000/40000, CX-debug03d61000/800, GMU-low03d6a000/26000,
   GMU-high03d9a000/5000, RSCC03d50000/10000, PDC0b290000/10000,
   and borrowed GPUCC03d90000/a000. All other views are dedicated. The broad
   GMU03d6a000/35000 and a duplicate CX-misc map are deliberately rejected.
   Page rounding must never enlarge these admitted register extents.
   Descriptors/Layout must be naturally aligned and disjoint. Invalid inputs
   leave Layout unchanged. No allocation, pointer, MMIO or readiness grant. */
CR_STATUS CrSm8450GpuResourcesBuild(
  CONST CR_SM8450_GPU_VIEW_DESCRIPTOR *Descriptors, UINTN Count,
  CR_SM8450_GPU_LAYOUT *Layout);

/* Revalidate the complete canonical layout on each call. Resolve a nonempty
   DWORD-aligned byte span using the exact source word-offset namespace above.
   Shift/add and whole-span bounds are checked; a span crossing views is never
   split, even where the physical views are adjacent. CX-misc is bounded to
   its 1000-byte slice and returns GMU-high + offset4000, not another map.
   Invalid layout: COMPROMISED_DATA; bad arguments/arithmetic: INVALID_PARAMETER;
   unrepresented, wrong-namespace or cross-view span: NOT_FOUND.
   Span must be naturally aligned and disjoint from Layout. Every failure
   leaves it unchanged. Layout storage is immutable for the entire call.

   The result describes arithmetic only. Actual checked IO must separately
   retain exact assigned mappings, powered access, native-writer retirement
   and a counted operation admission. Borrowed GPUCC access must serialize
   with its sole clock/reset owner. This API cannot prove any of those facts. */
CR_STATUS CrSm8450GpuResourcesResolve(CONST CR_SM8450_GPU_LAYOUT *Layout,
  CR_SM8450_GPU_NAMESPACE Namespace, UINT64 WordOffset, UINT32 Bytes,
  CR_SM8450_GPU_REGISTER_SPAN *Span);
