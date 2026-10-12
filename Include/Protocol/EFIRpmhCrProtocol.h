/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#include <oskal/cr_types.h>
#include <Library/rpmh.h>
#include <Library/RpmhArcVoteLib.h>

#define EFI_RPMH_CR_PROTOCOL_REVISION 0x0000000000010005

#define EFI_RPMH_CR_PROTOCOL_GUID                                              \
  {0x481e2514, 0xc4a9, 0x49a8, {0xaf, 0x78, 0x7a, 0x07, 0x77, 0xe9, 0xa0, 0x78}}

extern EFI_GUID                      gEfiRpmhCrProtocolGuid;
typedef struct _EFI_RPMH_CR_PROTOCOL EFI_RPMH_CR_PROTOCOL;

/* Raw active-set commands are submitted synchronously through Crane's local
   RPMh TCS driver. Callers must use the high-level rail helpers for shared
   voltage/mode/enable ownership. */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_WRITE)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN RpmhTcsCmd *TcsCmd, IN UINT32 NumCmds);

/* FALSE releases one acquired reference; the final reference also releases
   voltage/mode votes. Retry a failed release before removing prerequisites.
   New consumers must use AcquireVreg and release only when Owned is TRUE.
   The legacy setters/enable sequence cannot identify a consumer whose setup
   failed before acquiring a shared reference. Other Crane rail references
   retain their votes; this API does not use NPA clients. */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_ENABLE_VREG)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name, IN BOOLEAN Enable);

typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_SET_VREG_VOLTAGE)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name,
    IN CONST UINT32 VoltageMv);

typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_SET_VREG_MODE)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name,
    IN CONST UINT8 Mode);

/* Atomically reserve one reference before programming a previously idle rail.
   Owned is TRUE even on a partial programming failure: release it with FALSE.
   A rejected shared request returns Owned=FALSE and leaves existing votes alone.
   Shared consumers must request identical voltage and mode. */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_ACQUIRE_VREG)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name,
    IN UINT32 VoltageMv, IN UINT8 Mode, OUT BOOLEAN *Owned);

#define EFI_RPMH_CR_ARC_SNAPSHOT_REVISION 1U
#define EFI_RPMH_CR_ARC_RESOURCE_COUNT 6U
#define EFI_RPMH_CR_ARC_FLOOR_ACKNOWLEDGED 1ULL
#define EFI_RPMH_CR_ARC_CONSUMERS_UNSYNCHRONIZED 2ULL
#define EFI_RPMH_CR_ARC_REGISTRY_SEALED 4ULL
typedef struct {
  UINT32 Size,Revision,Count,OwnerCount;
  UINT64 Flags;
  CR_RPMH_ARC_RESOURCE Resources[EFI_RPMH_CR_ARC_RESOURCE_COUNT];
  UINT32 Corners[EFI_RPMH_CR_ARC_RESOURCE_COUNT];
} EFI_RPMH_CR_ARC_SNAPSHOT;
/* Stable caller identity; active-set hardware INDEX, zero withdraws only this
 * owner. Fixed storage is reserved before publication. Failures retain exact
 * logical demand/slot for retry. The private acknowledged boot-floor owner
 * remains at the highest valid power corner and XO3 until reboot; it preserves
 * unsynchronized boot demand, without claiming to recover earlier owners. */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_VOTE_ARC)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN VOID *StableOwner,
    IN UINT32 Address, IN UINT32 Corner);
/* Pure final seal after all consumers drain. Only the permanent boot-floor
 * owner may remain; every aggregate must be acknowledged and nonpending, and
 * the local RSC has no outstanding transaction. Success blocks further ARC
 * mutation and copies exact six-resource metadata/aggregate. Failure preserves
 * Out and leaves cleanup retry possible. No allocation or MMIO during seal. */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_SEAL_ARC_SNAPSHOT)(
    IN EFI_RPMH_CR_PROTOCOL *This, OUT EFI_RPMH_CR_ARC_SNAPSHOT *Out);
/* Retry and qualify the provider's acknowledged boot floor before a new
 * hardware consumer starts. Publication alone does not prove the original
 * floor request succeeded. Does not add an owner or change a sealed record. */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_ENSURE_BOOT_FLOOR)(
    IN EFI_RPMH_CR_PROTOCOL *This);

/* A provider-owned baseline, separate from ordinary consumer references.
 * Exact repeated name/voltage/mode is idempotent; conflicting requests fail.
 * Final success acknowledges the complete parent/child vote. Failure retains
 * private partial ownership for retry; it never adds a second pending owner.
 * This UEFI baseline remains asserted at EBS and is not a Windows handoff. */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_ENSURE_RETAINED_VREG)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name,
    IN UINT32 VoltageMv, IN UINT8 Mode);

/* Revision 1.5: retain enable only on a supplier whose voltage/mode is
 * inherited. No voltage/mode write or later withdrawal is authorized.
 * Idempotent on success; an uncertain enable remains protected for retry. */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_ENSURE_RETAINED_VREG_ENABLED)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name);

typedef struct _EFI_RPMH_CR_PROTOCOL {
  UINT64                       Revision;
  EFI_RPMH_CR_WRITE            RpmhWrite;
  EFI_RPMH_CR_ENABLE_VREG      RpmhEnableVreg;
  EFI_RPMH_CR_SET_VREG_VOLTAGE RpmhSetVregVoltage;
  EFI_RPMH_CR_SET_VREG_MODE    RpmhSetVregMode;
  EFI_RPMH_CR_ACQUIRE_VREG     AcquireVreg;
  EFI_RPMH_CR_VOTE_ARC         VoteArc;
  EFI_RPMH_CR_SEAL_ARC_SNAPSHOT SealArcSnapshot;
  EFI_RPMH_CR_ENSURE_BOOT_FLOOR EnsureBootFloor;
  EFI_RPMH_CR_ENSURE_RETAINED_VREG EnsureRetainedVreg;
  EFI_RPMH_CR_ENSURE_RETAINED_VREG_ENABLED EnsureRetainedVregEnabled;
} EFI_RPMH_CR_PROTOCOL;
