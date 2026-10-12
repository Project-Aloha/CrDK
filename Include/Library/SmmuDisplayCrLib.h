/* Pure inherited SM8250/SM8350 display translation observation.
 * Register and long-descriptor definitions: public Linux arm-smmu and
 * io-pgtable-arm. No register/table writes or translation ownership transfer.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/smmu.h>
#define CR_SMMU_DISPLAY_MAX_STREAMS 4U
typedef struct {
  UINT32 Smr, S2cr, Sctlr, Cbar, Cba2r, Tcr, Tcr2, Actlr;
  UINT64 Ttbr0, Ttbr1;
  UINT16 Index, Bank;
  UINT32 Reserved;
} CR_SMMU_DISPLAY_STREAM;
typedef struct {
  UINT32 Size, Revision, Count, Reserved;
  UINT64 DeviceAddress, PhysicalAddress, Bytes;
  CR_SMMU_DISPLAY_STREAM Streams[CR_SMMU_DISPLAY_MAX_STREAMS];
} CR_SMMU_DISPLAY_OBSERVATION;
/* Callback only reads CPU-accessible, retained reserved table memory. It must
 * decline unreserved/secure/unmapped backing and positive pending statuses.
 * No allocation, SMMU callback, table repair or mapping changes under the
 * producer's lock. All returned backing remains reserved through OS takeover.
 */
typedef CR_STATUS (*CR_SMMU_DISPLAY_TABLE_READ)(VOID *, UINT64, UINT64 *);
CR_STATUS CrSmmuDisplayObserve(SmmuDevice *, UINT64 Device, UINT64 Bytes,
    UINT64 ReservedBase, UINT64 ReservedBytes, CR_SMMU_DISPLAY_TABLE_READ,
    VOID *Context, CR_SMMU_DISPLAY_OBSERVATION *);
