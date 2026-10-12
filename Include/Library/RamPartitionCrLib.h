/** Qualcomm SMEM usable RAM table parser. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_RAM_PARTITION_SMEM_ITEM 402U
#define CR_RAM_PARTITION_MAGIC1 0x9da5e0a8U
#define CR_RAM_PARTITION_MAGIC2 0xaf9ec4e2U
#define CR_RAM_PARTITION_MAX_ENTRIES 32U
#define CR_RAM_PARTITION_MAX_WIRE_SIZE (24U + 72U * CR_RAM_PARTITION_MAX_ENTRIES)
#define CR_RAM_PARTITION_SDRAM 14U
#define CR_RAM_PARTITION_SYSTEM_MEMORY 1U
#define CR_RAM_PARTITION_DEFAULT_DOMAIN 0U
#define CR_RAM_PARTITION_APPS_DOMAIN 1U
#define CR_RAM_PARTITION_MODEM_DOMAIN 2U
#define CR_RAM_PARTITION_READWRITE 1U

typedef struct {
  /* A fixed-width name, which may occupy all 16 bytes without a terminator. */
  CHAR8 Name[16];
  UINT64 Base;
  UINT64 Length;
  UINT64 AvailableLength;
  UINT32 Attribute;
  UINT32 Category;
  UINT32 Domain;
  UINT32 Type;
  UINT32 NumPartitions;
  UINT32 HardwareInfo;
  /* Only version 3 defines these fields. Versions 0..2 return zero. */
  UINT32 HighestBankBit;
  UINT32 MinPasrSize;
} CR_RAM_PARTITION_ENTRY;

typedef struct {
  UINT32 Version;
  UINT32 EntryCount;
  UINT32 BankCount;
  UINT32 Reserved;
  UINT64 PhysicalBytes;
  UINT64 AvailableBytes;
  /* Default, APPS and shared APPS|MODEM system SDRAM, sorted by Base.
     Gaps and adjacent banks are preserved. Physical lengths are not grants
     to use unavailable tails. Other table categories/domains are excluded. */
  CR_RAM_PARTITION_ENTRY Banks[CR_RAM_PARTITION_MAX_ENTRIES];
} CR_RAM_PARTITION_TABLE;

CR_STATIC_ASSERT(sizeof(CR_RAM_PARTITION_ENTRY) == 72,
                 "RAM normalized entry layout");
CR_STATIC_ASSERT(sizeof(CR_RAM_PARTITION_TABLE) == 2336,
                 "RAM normalized table layout");

/* Parse a caller-owned stable copy of the little-endian SMEM item 402.
   The caller retains the SMEM mapping/lease while obtaining that copy; this
   function never writes remote memory, allocates, maps RAM, or changes GCD.
   Supports only proven versions 0, 1, 2, 3 with entry sizes 56, 64, 72, 72.
   Version 2+ AvailableLength is authoritative, including zero: never replace
   it with Length. The caller must subtract every firmware/board reservation.
   All selected ranges are page-aligned, nonwrapping and physically disjoint;
   unavailable tails participate in overlap checks. Read-only attributes are
   reported for caller policy, rather than converted into writable RAM.
   Out must be aligned and disjoint from the entire input range. On any
   failure (including no selected banks) it is unchanged. Trailing SMEM allocation
   padding is accepted and not interpreted as additional partitions. */
CR_STATUS CrRamPartitionParse(CONST VOID *Bytes, UINTN Size,
                             CR_RAM_PARTITION_TABLE *Out);
