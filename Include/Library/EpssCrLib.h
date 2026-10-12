/* Qualcomm SM8450 EPSS performance requests. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#define CR_EPSS_DOMAINS 3U
#define CR_EPSS_LUT_ROWS 40U
#define CR_EPSS_WINDOW_SIZE 0x1000U
#define CR_EPSS_ENABLE 0U
#define CR_EPSS_DOMAIN_STATE 0x20U
#define CR_EPSS_DCVS_CONTROL 0xb0U
#define CR_EPSS_FREQUENCY_LUT 0x100U
#define CR_EPSS_VOLTAGE_LUT 0x200U
#define CR_EPSS_INTERRUPT_CLEAR 0x308U
#define CR_EPSS_GT_INTERRUPT_STATUS (1U<<2)
#define CR_EPSS_PERFORMANCE_STATE 0x320U
typedef struct {
  VOID *Context;
  /* Pure current mapping/D0/exclusive-writer qualification. Hardware DCVS is
   * expected; the software owner must exclude another OS/runtime frontend.
   * All callbacks are bounded, nonpageable and externally serialized. */
  CR_STATUS (*Ready)(VOID *,BOOLEAN Write);
  CR_STATUS (*Read32)(VOID *,UINT32 Domain,UINT32 Offset,UINT32 *);
  CR_STATUS (*Write32)(VOID *,UINT32 Domain,UINT32 Offset,UINT32);
  CR_STATUS (*Barrier)(VOID *);
} CR_EPSS_IO;
typedef struct {
  UINT32 SoC,Board,Domain,CpuMask;
  UINT64 Base;
  UINT32 Length;
} CR_EPSS_DESCRIPTOR;
typedef struct {
  /* Actual source-owned acknowledged rates, immutable for this binding.
   * AlternateRateHz is GPLL0 before the source driver's divide-by-two. */
  UINT64 XoRateHz,AlternateRateHz;
} CR_EPSS_CLOCKS;
typedef struct {
  UINT32 HardwareIndex,FrequencyKhz,VoltageUv,RawFrequency,RawVoltage;
} CR_EPSS_STATE;
typedef struct {
  CR_EPSS_STATE Rows[CR_EPSS_LUT_ROWS],States[CR_EPSS_LUT_ROWS];
  UINT32 RowCount,StateCount,CpuMask,MemberCount,RawControl;
  UINT32 PendingIndex;
  CR_STATUS LastError;
  BOOLEAN PerCore,Pending,Touched,AckPending;
} CR_EPSS_DOMAIN;
typedef struct {
  CR_EPSS_IO Io;
  CR_EPSS_CLOCKS Clocks;
  CR_EPSS_DOMAIN Domains[CR_EPSS_DOMAINS];
  BOOLEAN Initialized;
} CR_EPSS;
typedef struct {UINT32 HardwareIndex,FrequencyKhz;BOOLEAN PerCore;} CR_EPSS_REQUESTED;
typedef struct {
  UINT64 ObservedHz;
  UINT32 NormalizedKhz,RequestedKhz;
  BOOLEAN Throttled;
} CR_EPSS_PRESSURE;
/* Qualification and initialization leave Out unchanged on any error. No
 * register is enabled/reprogrammed, voltage voted, or LUT fabricated. Only
 * Linux's ordinary rows (CORE_COUNT != 1) are advertised; boost is excluded.
 * Presentation is sorted by frequency, while writes use original LUT index. */
CR_STATUS CrEpssQualify(CONST CR_EPSS_DESCRIPTOR *,UINT32 Count);
CR_STATUS CrEpssInitialize(CR_EPSS *,CONST CR_EPSS_IO *,CONST CR_EPSS_CLOCKS *,CONST CR_EPSS_DESCRIPTOR *,UINT32 Count);
CR_STATUS CrEpssReady(CR_EPSS *,UINT32 Domain,BOOLEAN Write);
CR_STATUS CrEpssGetStates(CR_EPSS *,UINT32 Domain,CR_EPSS_STATE *,UINT32 Capacity,UINT32 *Count);
CR_STATUS CrEpssReadRequested(CR_EPSS *,UINT32 Domain,CR_EPSS_REQUESTED *);
/* Actual domain-state L value times the acknowledged XO rate. This is an
 * instantaneous frequency sample, never a monotonic feedback counter. */
CR_STATUS CrEpssReadFrequency(CR_EPSS *,UINT32 Domain,UINT64 *Hz);
/* Linux LMH pressure observation: floor in the complete hardware LUT (boost
 * rows included); below the lowest row use its ceiling. This transient sample
 * is not a guaranteed sustained performance limit. Outputs are atomic. */
CR_STATUS CrEpssSamplePressure(CR_EPSS *,UINT32 Domain,CR_EPSS_PRESSURE *);
/* A dedicated, masked LMH IRQ owner invokes this exact W1C acknowledgement.
 * No interrupt status read or guessed clear/readback semantics. Failed write
 * or ordering retains AckPending and requires mappings/code through retry. */
CR_STATUS CrEpssAcknowledgeLmh(CR_EPSS *,UINT32 Domain);
/* StateOrdinal refers to the sorted ordinary state array. A failed Set may
 * already have committed one/all per-core registers. PendingIndex is stored
 * before the first write; only this exact index may retry until every request
 * register reads it back. A different request returns BUSY without writes.
 * The caller must retain mappings/provider/code during pending uncertainty. */
CR_STATUS CrEpssSet(CR_EPSS *,UINT32 Domain,UINT32 StateOrdinal);
BOOLEAN CrEpssHasPending(CONST CR_EPSS *);
