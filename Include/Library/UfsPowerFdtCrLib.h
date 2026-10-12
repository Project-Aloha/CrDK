/* SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_UFS_POWER_FDT_VERSION 1U
#define CR_UFS_POWER_FDT_MAX (2U * 1024U * 1024U)
#define CR_UFS_POWER_RESOURCE_SIZE 16U
#define CR_UFS_POWER_SUPPLY_COUNT 6U

typedef enum {
  CrUfsPowerVcc = 0, CrUfsPowerVccq = 1, CrUfsPowerVccq2 = 2,
  CrUfsPowerVddHba = 3, CrUfsPowerVddaPhy = 4, CrUfsPowerVddaPll = 5
} CR_UFS_POWER_ROLE;
typedef enum {
  CrUfsPowerAbsentInherited = 0, CrUfsPowerRpmh = 1,
  CrUfsPowerFixedAlwaysOn = 2, CrUfsPowerGdsc = 3
} CR_UFS_POWER_KIND;

#define CR_UFS_POWER_RANGE_PRESENT       (1U << 0)
#define CR_UFS_POWER_INIT_PRESENT        (1U << 1)
#define CR_UFS_POWER_BOOT_ON             (1U << 2)
#define CR_UFS_POWER_ALWAYS_ON           (1U << 3)
#define CR_UFS_POWER_CONSUMER_FIXED      (1U << 4)
#define CR_UFS_POWER_CONSUMER_ALWAYS_ON  (1U << 5)
#define CR_UFS_POWER_CONSUMER_RANGE      (1U << 6)
#define CR_UFS_POWER_VCC_1P8             (1U << 7)
#define CR_UFS_POWER_VCC_LOW_VOLTAGE     (1U << 8)
#define CR_UFS_POWER_MAX_UA_PRESENT      (1U << 9)
#define CR_UFS_POWER_VOTE_SET_PRESENT    (1U << 10)

/* Facts only: InitUv is a DT initialization request, not a measured boot
 * voltage. MinUv/MaxUv and ConsumerMinUv/MaxUv remain separate constraints.
 * No selected voltage, inferred default, DT pointer, or writable resource. */
typedef struct {
  UINT32 Kind, Flags, Phandle;
  CHAR8 Resource[CR_UFS_POWER_RESOURCE_SIZE];
  UINT32 MinUv, MaxUv, InitUv, ConsumerMinUv, ConsumerMaxUv, MaxUa, VoteSet;
  UINT64 ProviderBase, ProviderSize; /* GDSC only; zero for other kinds. */
} CR_UFS_POWER_SUPPLY;

typedef struct {
  UINT32 Version, Size;
  UINT64 HostBase, HostSize, PhyBase, PhySize;
  CR_UFS_POWER_SUPPLY Supplies[CR_UFS_POWER_SUPPLY_COUNT];
  /* Optional Android qcom,vccq-parent-supply. Absence remains Kind=0;
   * policy validates its existing topology vote against these facts. */
  CR_UFS_POWER_SUPPLY VccqParent;
} CR_UFS_POWER_FDT;

/* Caller supplies readable immutable memory for FdtSize bytes. No allocation
 * or hardware access. Out is unchanged on all errors and may not overlap Fdt.
 * CR_NOT_FOUND: valid tree has no enabled UFS host.
 * CR_COMPROMISED_DATA: malformed or ambiguous data.
 * CR_UNSUPPORTED: valid unsupported binding/provider/address translation.
 * Accepts exactly one enabled supported UFS host and its linked enabled PHY.
 * Missing host supplies are explicit inherited facts; PHY supplies required. */
CR_STATUS CrUfsPowerFdtParse(CONST VOID *Fdt, UINTN FdtSize,
                           CR_UFS_POWER_FDT *Out);
