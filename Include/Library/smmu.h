/** @file
 *  SMMUv2 stage-1 DMA domains. Register definitions were checked against
 *  Linux drivers/iommu/arm/arm-smmu at 40288c920.
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define SMMU_PAGE_SIZE       4096U
#define SMMU_IOVA_BASE       0x10000000U
#define SMMU_IOVA_SIZE       0x04000000U
#define SMMU_TABLE_PAGES     34U
#define SMMU_TRANSLATION_BITS_MAX 39U /* Three 4 KiB levels, starting at L1. */
#define SMMU_ACCESS_READ     1U
#define SMMU_ACCESS_WRITE    2U
#define SMMU_STOPPED_STREAM_REVISION 1U
typedef CR_STATUS (*SmmuVerifyStopped)(VOID *Context);

typedef struct {
  UINT32 (*Read32)(VOID *Cookie, UINT64 Address);
  VOID (*Write32)(VOID *Cookie, UINT64 Address, UINT32 Value);
  VOID (*Write64)(VOID *Cookie, UINT64 Address, UINT64 Value);
  /* Clean CPU cache lines to the point of coherency, then complete stores. */
  VOID (*Publish)(VOID *Cookie, VOID *Address, UINTN Size);
  VOID (*DelayUs)(VOID *Cookie, UINT32 Delay);
  VOID *Cookie;
} SmmuIoOps;

typedef enum {
  SmmuAttachResourceNone,
  SmmuAttachResourceStream,
  SmmuAttachResourceBank,
  SmmuAttachResourceAsid
} SmmuAttachResource;

typedef struct {
  UINT16 Sid;
  UINT16 Mask; /* Ignored SID bits, as in the public qcom,actlr DT property. */
  UINT32 Value;
} SmmuActlrSetting;

typedef enum {
  SmmuProbeUninitialized,
  SmmuProbeFailed,
  SmmuProbeReady
} SmmuProbeState;

typedef struct {
  SmmuProbeState ProbeState;
  UINT64 Base;
  UINT64 ContextBase;
  UINT32 PageSize;
  UINT16 BankCount;
  UINT16 Stage2BankCount;
  UINT16 StreamCount;
  /* Stage-1 input/IOVA size (ID2.UBS-derived). */
  UINT8 InputAddressBits;
  UINT8 InputAddressEncoding;
  /* Configured input width, capped to the three-level table geometry. */
  UINT8 TranslationAddressBits;
  /* Stage-1 output/IPA size (ID2.IAS-derived), used for TCR2.PASIZE and
   * translated physical-address bounds. */
  UINT8 Stage1OutputAddressBits;
  UINT8 Stage1OutputAddressEncoding;
  /* Table-walker physical output size (ID2.OAS-derived), used for page-table
   * addresses and the CPU allocation aperture. */
  UINT8 AddressBits;
  UINT8 AddressEncoding;
  CONST SmmuActlrSetting *ActlrSettings;
  UINTN ActlrSettingCount;
  BOOLEAN SyncFailed; /* No further TLB commands after a controller timeout. */
  /* Target-specific readback quirk. Only explicitly documented bits may
   * differ after an SCTLR write; architectural M/CFRE/AFE/TRE/CFIE remain
   * mandatory. SM8450 drops S1_ASIDPNE (bit 12) on this hardware revision. */
  UINT32 SctlrReadbackIgnoreMask;
  BOOLEAN ReservedBanks[256];
  BOOLEAN ReservedStreams[128];
  BOOLEAN ReservedAsids[256];
  SmmuAttachResource LastAttachResource;
  SmmuIoOps Io;
  /* Monotonic until real controller reset; also fences stale domain copies. */
  BOOLEAN PhysicalExitBanks[256];
  /* An explicitly acquired firmware stream never restores its original route,
   * including through stale domain copies. Old/new banks remain reserved. */
  BOOLEAN InheritedStreams[128];
} SmmuDevice;

typedef enum {
  SmmuDomainExitNormal = 0,
  SmmuDomainExitPending,
  SmmuDomainExitPhysical
} SmmuDomainExitState;

typedef struct {
  SmmuDevice *Device;
  UINT64 *Tables;
  UINT64 TableAddress;
  UINT64 BankBase;
  UINT16 Bank;
  UINT16 Stream;
  UINT16 Asid;
  UINT16 Sid;
  UINT32 OriginalSmr;
  UINT32 OriginalS2cr;
  UINT32 OriginalSctlr;
  UINT32 OriginalCbar;
  UINT32 OriginalCba2r;
  UINT32 OriginalTcr2;
  UINT32 OriginalTcr;
  UINT64 OriginalTtbr0;
  UINT64 OriginalTtbr1;
  UINT32 OriginalMair0;
  UINT32 OriginalMair1;
  UINT32 OriginalActlr;
  BOOLEAN OwnsActlr;
  BOOLEAN Active;
  BOOLEAN Failed;
  SmmuDomainExitState ExitState;
  /* Fixed by the attach entrypoint before any context programming. Never
   * change this policy while active; MAIR readback/evidence must agree. */
  BOOLEAN CacheCoherent;
  BOOLEAN InheritedStreamOwned;
} SmmuDomain;

/* Pointer-free observation of one owned translation. A successful query
 * verifies the live stream/context registers and every page in this span.
 * The caller serializes domain/table writers; this is evidence, not a new
 * ownership lease, and grants no OS access to firmware tables or memory. */
typedef struct {
  UINT64 Base, BankBase, Tables, TableBytes;
  UINT64 Iova, Physical, Bytes, Ttbr0, Ttbr1;
  UINT32 Smr, S2cr, Sctlr, Cbar, Cba2r, Tcr, Tcr2;
  UINT32 Mair0, Mair1, Actlr, SctlrIgnoreMask, Access;
  UINT16 Sid, Bank, Stream, Asid;
} SmmuMappingEvidence;
CR_STATIC_ASSERT(sizeof(SmmuMappingEvidence) == 128, "SMMU evidence layout");

CR_STATUS SmmuQueryMappingEvidence(SmmuDomain *Domain, UINT64 Iova,
    UINT64 Physical, UINTN Pages, UINT32 Access, SmmuMappingEvidence *Evidence);
/* Read-only observation of a revoked range, with exact owned register/table
   qualification. Success has Physical=0, Access=0 and all requested PTEs zero. */
CR_STATUS SmmuQueryRevokedMapping(SmmuDomain *Domain, UINT64 Iova,
    UINTN Pages, SmmuMappingEvidence *Evidence);

/* Read-only fault/configuration/page-table diagnostics for an owned domain,
 * including a failed domain. Never acknowledge FSR, resume stalled traffic,
 * invalidate translations or change ownership. Caller serializes writers.
 * Success means the snapshot was emitted, not that DMA is healthy. */
CR_STATUS SmmuDumpFaultState(SmmuDomain *Domain);

/* Adopt an enabled non-secure SMMU without resetting global state.
 * Call once on a zero-initialized software device; reprobe is not timeout
 * recovery and must not discard live or failed ownership. A disabled
 * SMMU is enabled with unmatched streams faulting only if all streams/banks
 * are unused. This never resets a live display/storage context. */
CR_STATUS SmmuProbe(SmmuDevice *Device, UINT64 Base, UINT64 Size,
                    CONST SmmuIoOps *Io);

/* Caller must quiesce the stream before attaching. Never takes a valid
 * stream or a wildcard match belonging to another client. A disabled,
 * unreferenced context bank may retain configuration from its previous owner;
 * it can be reused, with its configuration saved for detach. Enabled banks,
 * stream-bound banks, stage-2 banks and the final bypass bank are excluded.
 * Domain must be zero-initialized or successfully detached; an active or
 * failed domain cannot be attached again. Tables is a zeroable, physically
 * contiguous SMMU_TABLE_PAGES allocation.
 * After any hardware write, keep it reserved until the SMMU is reset.
 */
CR_STATUS SmmuAttach(SmmuDevice *Device, SmmuDomain *Domain, UINT16 Sid,
                     VOID *Tables, UINT64 TableAddress);

/* Same ownership/rollback requirements as SmmuAttach, for an explicitly
 * qualified coherent DMA stream. Sets only this acquired context's MAIR
 * attribute 0 to inner/outer Write-Back Read/Write-Allocate (0xff), with
 * inner-shareable normal-memory pages. The caller qualifies the physical
 * interconnect and stream; no global coherency policy is inferred or changed.
 */
CR_STATUS SmmuAttachCoherent(SmmuDevice *Device, SmmuDomain *Domain, UINT16 Sid,
                             VOID *Tables, UINT64 TableAddress);

/* Explicit owner handoff, never a relaxation of ordinary Attach. The adapter
 * must qualify the target and producer; VerifyStopped proves reset/queues/HCE
 * stopped with clocks available and is repeated before writes and route commit.
 * A unique exact translating inherited SMR is kept valid, its original bank is
 * left untouched, and S2CR is retargeted only to a fresh verified M=1 bank with
 * empty private tables after own-context synchronization. Wildcards, duplicate
 * matches, shared/architecturally reserved or native-owned banks are rejected.
 * Once any hardware programming starts all ownership/tables remain pinned on
 * failure. An acquired inherited stream cannot detach or exit to physical DMA;
 * block/revoke retains its enabled translation and exact valid route. */
CR_STATUS SmmuAcquireStoppedStream(SmmuDevice *Device, SmmuDomain *Domain,
    UINT16 Sid, VOID *Tables, UINT64 TableAddress, BOOLEAN Coherent,
    SmmuVerifyStopped VerifyStopped, VOID *Context);

/* IOVA and physical ranges must be page aligned. A write permission also
 * grants read on the Arm stage-1 AP encoding. Access=0 revokes access.
 * The caller owns IOVA allocation and serialization. A failed TLB sync
 * poisons the domain; backing memory must remain pinned in that case.
 */
CR_STATUS SmmuSetMapping(SmmuDomain *Domain, UINT64 Iova, UINT64 Physical,
                         UINTN Pages, UINT32 Access);
CR_STATUS SmmuBlock(SmmuDomain *Domain);

/*
 * Detach a domain which was created by SmmuAttach().  The stream and context
 * bank configuration are restored to their saved state with verified writes.
 * The caller must have stopped the DMA agent before calling this function.
 * Failure retains the bank/stream reservation and poisons the domain; its
 * tables must stay pinned. A TLB timeout also blocks all domains on the device.
 */
CR_STATUS SmmuDetach(SmmuDomain *Domain);

/* Terminal physical-address DMA handoff for one exact owned stream. The caller
 * must serialize all SMMU writers and prove the DMA agent and firmware writers
 * stopped. The entire owned domain must already be revoked; this function
 * verifies that state, exclusivity, and completes own-context maintenance.
 * It preserves the exact translating S2CR and S1-translation/S2-bypass CBAR,
 * and disables only this context's SCTLR (Linux Qualcomm bypass mechanism).
 * No global or other-client state is changed. PhysicalExitBanks and ExitState
 * become terminal before the first maintenance write. Failure retains all
 * ownership and memory; success transfers physical DMA behavior to the OS,
 * not a restricted page grant. Neither result permits map/block/detach/reuse
 * or a revoked-mapping query, including through a stale copied domain.
 */
CR_STATUS SmmuReleaseToPhysicalAtExit(SmmuDomain *Domain);

/* Same terminal ownership and stopped-producer requirements as above, for
 * an explicitly qualified coherent physical-DMA consumer (for example an OS
 * device advertising ACPI _CCA=1). Uses the SM8450 Android coherent-bypass
 * SCTLR attributes: read/write allocate, outer shareable, inner/outer WB,
 * memory-type override, with translation disabled. Exact readback is required.
 * This is a per-context attribute override, not a global bypass or an IOVA
 * grant. The caller must qualify the device/interconnect's coherency support;
 * the function does not discover or enable coherency outside the SMMU.
 */
CR_STATUS SmmuReleaseToCoherentPhysicalAtExit(SmmuDomain *Domain);
