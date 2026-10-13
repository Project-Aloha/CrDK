/** A730 mainline-family HFI performance records. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/RpmhArcVoteLib.h>

#define CR_ADRENO_POWER_VERSION 1U
#define CR_ADRENO_POWER_A730_CHIP_ID 0x07030001U
/* Actual A6xx/A7xx HFI perf message capacities, including the off row. */
#define CR_ADRENO_POWER_MAX_GX 16U
#define CR_ADRENO_POWER_MAX_CX 4U
#define CR_ADRENO_POWER_SECONDARY_MX 1U
#define CR_ADRENO_POWER_SECONDARY_GMXC 2U

typedef struct {
  UINT64 FrequencyHz;
  UINT32 Level;             /* RPMh virtual corner, never microvolts. */
  UINT32 Reserved;
} CR_ADRENO_POWER_OPP;

typedef struct {
  UINT32 Version, ChipId, GpuCount, GmuCount;
  UINT32 InitialGpuIndex, GmuBootIndex;
  UINT32 GmuBootHz, HubBootHz;
  CR_ADRENO_POWER_OPP Gpu[CR_ADRENO_POWER_MAX_GX];
  CR_ADRENO_POWER_OPP Gmu[CR_ADRENO_POWER_MAX_CX];
} CR_ADRENO_POWER_PROFILE;

/* Field order is the A730 HFI perf record order: vote, ACD, kHz. */
typedef struct { UINT32 Vote, Acd, FrequencyKHz; } CR_ADRENO_POWER_GX;
typedef struct { UINT32 Vote, FrequencyKHz; } CR_ADRENO_POWER_CX;
typedef struct {
  UINT32 Version, Size, ChipId, GxCount, CxCount;
  UINT32 InitialGpuIndex, GmuBootIndex, GxSecondary;
  CR_ADRENO_POWER_GX Gx[CR_ADRENO_POWER_MAX_GX];
  CR_ADRENO_POWER_CX Cx[CR_ADRENO_POWER_MAX_CX];
} CR_ADRENO_POWER_PLAN;
CR_STATIC_ASSERT(sizeof(CR_ADRENO_POWER_OPP) == 16, "Adreno OPP size");
CR_STATIC_ASSERT(sizeof(CR_ADRENO_POWER_PROFILE) == 352, "Adreno profile size");
CR_STATIC_ASSERT(sizeof(CR_ADRENO_POWER_GX) == 12, "Adreno GX HFI size");
CR_STATIC_ASSERT(sizeof(CR_ADRENO_POWER_CX) == 8, "Adreno CX HFI size");
CR_STATIC_ASSERT(sizeof(CR_ADRENO_POWER_PLAN) == 256, "Adreno power plan size");

/* Pure immutable source profile: all12 SM8450 mainline GPU OPPs + off, both
 * GMU OPPs + off. Source initializes GPU index to highest OPP, not350MHz;
 * source resume programs GMU200MHz/hub150MHz. This is a reference inventory,
 * not board speed-bin qualification or permission to select any frequency. */
CONST CR_ADRENO_POWER_PROFILE *CrAdrenoSm8450PowerProfile(VOID);

/* Pure, allocation/IO-free A730 mainline-family ARC/HFI record builder.
 * Inputs are immutable snapshots retained through the entire call. Resources
 * are normalized CR_RPMH_ARC_RESOURCE records from CrRpmhArcQualify/Describe;
 * this function independently validates their names, addresses and level data.
 * Gfx/Cx/Mx are required; NULL Gmxc explicitly means confirmed absent. Provider
 * not-ready/deferred/error must be handled before this call, not mapped to NULL.
 * A supplied malformed Gmxc fails, never falls back to Mx. All distinct rails
 * must have distinct canonical ARC addresses. No CmdDB pointer escapes.
 *
 * Row0 must be off(0Hz/level0); active Hz values strictly increase, levels may
 * repeat but cannot decrease or be zero. All frequencies must be exact kHz and
 * fit HFI's32-bit kHz field. Unused rows and Reserved fields must be zero.
 * Primary picks first corner >= requested level and fails if none. Secondary
 * picks first >= REQUESTED level or saturates at last nonzero entry. This is
 * Linux semantics; Android Gen7 instead compares to the selected primary
 * corner. Vote=(primary_corner<<16)|(secondary_index<<8)|primary_index.
 * GX Acd isFFFFFFFF including off, per the source perf message. Off vote is0.
 *
 * Profile is naturally8-byte aligned, resources/output4-byte aligned. Output
 * must be disjoint from every complete input object; errors leave it unchanged.
 * No HFI header/sequence/queue, bandwidth table, dependency-vote message, RPMh
 * write, firmware compatibility, power/stop/SCM/domain authority or readiness is
 * supplied. The consumer must qualify actual target/firmware and serialize HFI
 * transport under its own execution lifetime. Plan is pointer-free; reserved
 * and unused output slots are zero. */
CR_STATUS CrAdrenoPowerBuild(CONST CR_ADRENO_POWER_PROFILE *Profile,
    CONST CR_RPMH_ARC_RESOURCE *Gfx, CONST CR_RPMH_ARC_RESOURCE *Cx,
    CONST CR_RPMH_ARC_RESOURCE *Mx, CONST CR_RPMH_ARC_RESOURCE *Gmxc,
    CR_ADRENO_POWER_PLAN *Plan);
