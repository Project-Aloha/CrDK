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
#define SMMU_ACCESS_READ     1U
#define SMMU_ACCESS_WRITE    2U

typedef struct {
  UINT32 (*Read32)(VOID *Cookie, UINT64 Address);
  VOID (*Write32)(VOID *Cookie, UINT64 Address, UINT32 Value);
  VOID (*Write64)(VOID *Cookie, UINT64 Address, UINT64 Value);
  /* Clean CPU cache lines to the point of coherency, then complete stores. */
  VOID (*Publish)(VOID *Cookie, VOID *Address, UINTN Size);
  VOID (*DelayUs)(VOID *Cookie, UINT32 Delay);
  VOID *Cookie;
} SmmuIoOps;

typedef struct {
  UINT64 Base;
  UINT64 ContextBase;
  UINT32 PageSize;
  UINT16 BankCount;
  UINT16 Stage2BankCount;
  UINT16 StreamCount;
  UINT8 AddressBits;
  UINT8 AddressEncoding;
  SmmuIoOps Io;
} SmmuDevice;

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
  BOOLEAN Active;
  BOOLEAN Failed;
} SmmuDomain;

/* Adopt an enabled non-secure SMMU without resetting global state. A disabled
 * SMMU is enabled with unmatched streams faulting only if all streams/banks
 * are unused. This never resets a live display/storage context. */
CR_STATUS SmmuProbe(SmmuDevice *Device, UINT64 Base, UINT64 Size,
                    CONST SmmuIoOps *Io);

/* Caller must quiesce the stream before attaching. Never takes an active
 * translated stream or a wildcard match belonging to another client.
 * Tables is a zeroable, physically contiguous SMMU_TABLE_PAGES allocation.
 * After any hardware write, keep it reserved until the SMMU is reset.
 */
CR_STATUS SmmuAttach(SmmuDevice *Device, SmmuDomain *Domain, UINT16 Sid,
                     VOID *Tables, UINT64 TableAddress);

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
 * bank are restored byte-for-byte to their pre-attach state.  The caller must
 * have stopped the DMA agent before calling this function.
 */
CR_STATUS SmmuDetach(SmmuDomain *Domain);
