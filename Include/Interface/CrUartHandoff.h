/* Final native UART writer retirement evidence. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <stddef.h>

#define CR_UART_HANDOFF_SIGNATURE 0x48545255ULL /* URTH */
#define CR_UART_HANDOFF_REVISION 1ULL
#define CR_UART_HANDOFF_SIZE 1024U
#define CR_UART_HANDOFF_PLATFORM 0x84500001ULL
#define CR_UART_HANDOFF_BASE 0x0099c000ULL
#define CR_UART_HANDOFF_SPAN 0x4000ULL
#define CR_UART_HANDOFF_IMAGES 32U
#define CR_UART_HANDOFF_RETIRED 1ULL
#define CR_UART_HANDOFF_UNCERTAIN 2ULL
#define CR_UART_IMAGE_NO_SERIAL 0U
#define CR_UART_IMAGE_NULL_SERIAL 1U
#define CR_UART_IMAGE_GATED_SERIAL 2U
#define CR_UART_IMAGE_UNKNOWN 3U
#define CR_UART_HANDOFF_LINK_COVERAGE 1U
#define CR_UART_HANDOFF_LOADED_COVERAGE 2U
#define CR_UART_HANDOFF_CLOSED 4U
#define CR_UART_HANDOFF_HARDWARE_RETIRED 8U
#define CR_UART_HANDOFF_RESERVED_MEMORY 16U
#define CR_UART_HANDOFF_ALL_FLAGS 31U

typedef struct {
  UINT8 Guid[16]; /* EFI GUID byte layout, not printable UUID byte order. */
  UINT32 SerialKind, Reserved;
} CR_UART_FIRMWARE_IMAGE;
typedef struct {
  UINT8 Guid[16];
  UINT32 SerialKind, PeBytes, PeChecksum, Reserved;
} CR_UART_FIRMWARE_BUILD_IMAGE;

/* Immutable generated linked-runtime inventory. Digest covers runtime PE and
 * selected actual archives, independent of the publisher binary/FV hash: a
 * whole-FV digest would create a self-referential build. The generator verifies
 * every selected SerialPortLib; separate reviewed source audit must exclude
 * direct runtime MMIO writers and calls into expired boot-code providers.
 * This is trusted build data, not an OS-supplied assertion or authentication. */
typedef struct {
  UINT32 Revision, Count;
  UINT8 Digest[32];
  CR_UART_FIRMWARE_BUILD_IMAGE Images[CR_UART_HANDOFF_IMAGES];
} CR_UART_FIRMWARE_COVERAGE;

#define CR_UART_LOCATOR_SIGNATURE 0x46545255U /* URTF */
#pragma pack(push,1)
typedef struct {
  UINT32 Signature, Length;
  UINT8 Revision, Checksum, OemId[6], OemTableId[8];
  UINT32 OemRevision, CreatorId, CreatorRevision;
  UINT32 WireRevision;
  UINT64 RecordPhysical;
  UINT32 RecordLength, Reserved;
  UINT64 RecordGeneration;
} CR_UART_HANDOFF_LOCATOR;
#pragma pack(pop)

typedef struct {
  UINT64 Signature, Revision, Size, Platform;
  UINT64 Generation, PublishSequence, Phase, NativeExitStatus;
  UINT64 UartBase, UartSize;
  UINT8 CoverageDigest[32];
  UINT64 BuildRuntimeCount, LoadedRuntimeCount, LoadedGatedCount;
  UINT64 RecordDescriptorBase, RecordDescriptorSize;
  UINT64 RecordDescriptorType, RecordDescriptorAttributes;
  UINT32 GateState, Active, Bindings, RuntimeBindings;
  UINT32 Protocol, GeniStatus, TxWords, RxWords, RxDepth;
  UINT32 MasterIrqEnable, SlaveIrqEnable, SeIrqEnable, GsiEventEnable;
  UINT32 DmaMode, DmaTxIrqEnable, DmaRxIrqEnable, Reserved0;
  UINT32 Flags, Reserved1, Reserved2;
  CR_UART_FIRMWARE_IMAGE Images[CR_UART_HANDOFF_IMAGES];
  UINT64 Checksum; /* low32 CRC32 bytes0..1015; high32 zero. */
} CR_UART_HANDOFF;
CR_STATIC_ASSERT(sizeof(CR_UART_FIRMWARE_IMAGE)==24,"UART image wire size");
CR_STATIC_ASSERT(sizeof(CR_UART_FIRMWARE_BUILD_IMAGE)==32,"UART build image size");
CR_STATIC_ASSERT(sizeof(CR_UART_HANDOFF_LOCATOR)==64,"UART locator wire size");
CR_STATIC_ASSERT(offsetof(CR_UART_HANDOFF_LOCATOR,WireRevision)==36,"UART locator revision offset");
CR_STATIC_ASSERT(offsetof(CR_UART_HANDOFF_LOCATOR,RecordPhysical)==40,"UART locator record offset");
CR_STATIC_ASSERT(offsetof(CR_UART_HANDOFF_LOCATOR,RecordGeneration)==56,"UART locator generation offset");
CR_STATIC_ASSERT(sizeof(CR_UART_HANDOFF)==CR_UART_HANDOFF_SIZE,"UART record wire size");
CR_STATIC_ASSERT(offsetof(CR_UART_HANDOFF,Images)==248,"UART image offset");
CR_STATIC_ASSERT(offsetof(CR_UART_HANDOFF,Checksum)==1016,"UART CRC offset");

/* Firmware-time-only inactive producer. Prepare record/map/handle storage
 * before EBS; validate final reserved/NVS descriptor, close common admission,
 * validate every actual loaded runtime GUID and synchronously retire GENI.
 * A failing/unknown witness is an UNCERTAIN diagnostic, never a grant. Actual
 * Core EBS group fires only after successful memory-map termination, so a
 * failed stale-MapKey attempt must not close the pre-EBS console.
 *
 * This record proves native retirement. Windows independently validates OS
 * reserved-RAM exclusion, assigned SE7 resources, real PEP clock/ICB/TLMM/D0
 * leases, UART loaded protocol, idle/FIFO/IRQ state before changing hardware.
 * No new UART firmware loader or source/power claim is implied. CRC detects
 * corruption/torn writes, not malicious firmware. Boot-only images cannot be
 * post-EBS writers under the UEFI code lifetime contract. Unknown *runtime*
 * loaded images are rejected even when absent from the original firmware FV. */
