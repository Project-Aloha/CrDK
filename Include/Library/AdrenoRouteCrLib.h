/* Exact HDK8450 stopped GPU route facts. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_ADRENO_ROUTE_VERSION 1U
#define CR_ADRENO_ROUTE_CP_COUNT 4U
#define CR_ADRENO_ROUTE_GMU_COUNT 2U
#define CR_ADRENO_ROUTE_IRQ_COUNT 25U
#define CR_ADRENO_ROUTE_MAX_IORT 65536U
#define CR_ADRENO_ROUTE_SMMU_BASE 0x03da0000ULL
#define CR_ADRENO_ROUTE_SMMU_BYTES 0x40000U
#define CR_ADRENO_ROUTE_CP_PATH "\\_SB_.GPU0"
#define CR_ADRENO_ROUTE_GMU_PATH "\\_SB_.GMU0"
typedef struct {UINT32 InputId,StreamId;} CR_ADRENO_ROUTE_MAPPING;
typedef struct {
 UINT32 Version,Size,IortBytes,IortCrc32,SmmuOffset,SmmuIdentifier;
 UINT64 SmmuBase;
 UINT32 SmmuBytes,GlobalIrq,ContextCount,Reserved;
 UINT32 ContextIrqs[CR_ADRENO_ROUTE_IRQ_COUNT];
 CR_ADRENO_ROUTE_MAPPING Cp[CR_ADRENO_ROUTE_CP_COUNT],Gmu[CR_ADRENO_ROUTE_GMU_COUNT];
} CR_ADRENO_ROUTE_FACTS;
/* Pure bounded decoder of installed standard IORT rev5. Supports this profile's
 * ITS/named/SMMUv2 nodes only; no packed Qualcomm-private InputBase decoding.
 * Exact GPU namespace, complete physical aliases, one shared Adreno SMMU and
 * contiguous per-PDO input groups. Input IDs are never SIDs or context banks.
 * Fact output is not a HAL/domain/stop grant. Unchanged on any failure.
 * Source and output must be valid disjoint CPU buffers; no allocation/IO. */
CR_STATUS CrAdrenoRouteParseIort(CONST VOID *,UINT32,CR_ADRENO_ROUTE_FACTS *);
CR_STATUS CrAdrenoRouteValidateFacts(CONST CR_ADRENO_ROUTE_FACTS *);
/* QueryInputMappings elements are reduced to their UINT32 InputMappingId by
 * the real WDM adapter only after exact returned byte/element checks. */
CR_STATUS CrAdrenoRouteCheckIds(CONST CR_ADRENO_ROUTE_FACTS *,BOOLEAN Gmu,
 CONST UINT32 *Ids,UINT32 Count);
UINT32 CrAdrenoRouteCrc32(CONST VOID *,UINT32);
