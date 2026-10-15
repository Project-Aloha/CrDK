/* SPDX-License-Identifier: MIT. Current-boot Android slot handoff, revision 1. */
#ifndef NH_BOOT_SLOT_H
#define NH_BOOT_SLOT_H
#include <Library/FirmwareLoaderLib/NhFs.h>

#define NH_BOOT_SLOT_VARIABLE_NAME L"CrAndroidBootSlot"
#define NH_BOOT_SLOT_VARIABLE_GUID \
  {0xa1548d27,0xe02f,0x4f32,{0x9d,0x71,0x37,0xb4,0x09,0x66,0xfa,0x42}}
#define NH_BOOT_SLOT_VARIABLE_ATTRIBUTES 6U /* BootServiceAccess | RuntimeAccess */
#define NH_BOOT_SLOT_SIGNATURE 0x5342484eU /* little-endian "NHBS" */
#define NH_BOOT_SLOT_VERSION 1U
#define NH_BOOT_SLOT_RECORD_BYTES 32U
#define NH_BOOT_SLOT_UNAVAILABLE 0U
#define NH_BOOT_SLOT_VALID 1U
#define NH_BOOT_SLOT_ABSENT 2U
#define NH_BOOT_SLOT_MALFORMED 3U
#define NH_BOOT_SLOT_CONFLICT 4U
#define NH_BOOT_SLOT_CMDLINE 1U
#define NH_BOOT_SLOT_BOOTCONFIG 2U
#define NH_BOOT_SLOT_FDT 4U
#define NH_BOOT_SLOT_SOURCES 7U
#define NH_BOOT_SLOT_INITIALIZED 1U
#define NH_BOOT_SLOT_UNKNOWN UINT32_MAX

/* Eight little-endian u32 fields, no pointers or native-size fields. The
 * producer publishes only this boot's immutable bootloader handoff. It must
 * never set NON_VOLATILE or substitute GPT priority/next-slot metadata.
 * Consumers capture once at initialization and validate before selecting a
 * partition. This is provenance metadata, not an authentication mechanism. */
typedef struct {
  uint32_t Signature, Version, Size, State;
  uint32_t Slot, Sources, Flags, Reserved;
} NH_BOOT_SLOT_RECORD;

/* Decode unaligned bytes. Missing/unavailable/absent authority never means
 * an Android firmware file is absent and never permits package fallback.
 * Slot is reset to UNKNOWN on every failure. The caller separately handles
 * variable-read errors and must not pass a fabricated zero-filled record. */
NH_STATUS NhBootSlotDecode(const void *Record, size_t Bytes,
                          uint32_t Attributes, uint32_t *Slot);
#endif
