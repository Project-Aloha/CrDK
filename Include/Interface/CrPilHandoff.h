/* Native->Windows final PIL ownership record. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>

#define CR_BUS_PIL_HANDOFF_SIGNATURE 0x484c4950ULL /* PILH, little endian */
#define CR_BUS_PIL_HANDOFF_REVISION 1ULL
#define CR_BUS_PIL_HANDOFF_SIZE 1024ULL
#define CR_BUS_PIL_HANDOFF_PLATFORM 0x84500001ULL /* SM8450 HDK, exact target */
#define CR_BUS_PIL_HANDOFF_FINAL_EBS 2ULL
#define CR_BUS_PIL_PAS_OWNED_SHUTDOWN 1ULL
#define CR_BUS_PIL_NATIVE_CONDUIT_SMC 1ULL
#define CR_BUS_PIL_ARC_MANAGED_AGGREGATE 1ULL
#define CR_BUS_PIL_ARC_UNSYNCHRONIZED_BOOT_FLOOR 2ULL
#define CR_BUS_PIL_HANDOFF_ARC_COUNT 6U
/* OEM ACPI locator: little-endian PILF, revision1, OEMID "ALOHA ",
 * OEMTableID "PILHDK84". ACPI checksum covers this complete packed68byte table.
 * RecordPhysical points to independently OS-excluded reserved/NVS memory;
 * RecordGeneration must match the final immutable ownership record. */
#define CR_BUS_PIL_LOCATOR_SIGNATURE 0x464c4950U
#pragma pack(push,1)
typedef struct {
  UINT32 Signature,Length;
  UINT8 Revision,Checksum,OemId[6],OemTableId[8];
  UINT32 OemRevision,CreatorId,CreatorRevision;
  UINT64 RecordPhysical,RecordLength,RecordGeneration,Reserved;
} CR_BUS_PIL_LOCATOR;
#pragma pack(pop)
CR_STATIC_ASSERT(sizeof(CR_BUS_PIL_LOCATOR)==68,"PIL ACPI locator wire size");
typedef struct {
  UINT8 Name[8]; /* NUL padding, or an exact eight-byte command name. */
  UINT64 Address,Corner,Provenance,LevelCount;
  UINT16 Levels[32]; /* LE16 hardware table, zero padding after LevelCount. */
  UINT64 Reserved;
} CR_BUS_PIL_HANDOFF_ARC;
typedef struct {
  UINT64 Signature,Revision,Size,Platform;
  UINT64 Generation,PublishSequence,Phase,PasDisposition;
  UINT64 NativeExitStatus,ShutdownX0,ShutdownX1,PendingFid,PendingCookie,OwnedMask;
  UINT64 NativeCallbacks,NativeIrqs,FirmwareEl,NativeConduit,NativeConvention;
  UINT64 CarveoutBase,CarveoutSize,MemoryDescriptorBase,MemoryDescriptorSize;
  UINT64 MemoryDescriptorType,MemoryDescriptorAttributes;
  UINT64 QmpBase,QmpSize,QmpOwners,QmpPendingBytes;
  /* APSS MCORE state and AOP's acknowledgement: link offsets0x24/0x28,
     channel offsets0x2c/0x30. Not AOP's independent UCORE state0x0c/0x14. */
  UINT64 QmpLocalLink,QmpRemoteLink,QmpLocalChannel,QmpRemoteChannel;
  UINT64 IpccSend,IpccOwners,IpccPendingSend,ArcCount;
  CR_BUS_PIL_HANDOFF_ARC Arc[CR_BUS_PIL_HANDOFF_ARC_COUNT];
  UINT64 Reserved[6];
  UINT64 Checksum; /* low32 CRC32 of bytes0..1015, high32 zero */
} CR_BUS_PIL_HANDOFF;
CR_STATIC_ASSERT(sizeof(CR_BUS_PIL_HANDOFF_ARC)==112,"PIL ARC handoff wire size");
CR_STATIC_ASSERT(sizeof(CR_BUS_PIL_HANDOFF)==CR_BUS_PIL_HANDOFF_SIZE,"PIL handoff wire size");

/* Native PILCrDxe produces this only after final EBS retirement. The root
   OEM ACPI descriptor must identify a firmware-allocated reserved/NVS normal
   memory region and its exact bounds. This is not a registry/IOCTL assertion
   channel. CRC/seqlock detect corruption/torn writes, not authenticity.
   All fields are little endian. Allocate/describe before ExitBootServices;
   final native publication performs no allocation or memory-map mutation.
   Producer holds PublishSequence odd while updating, computes Checksum with
   the intended even sequence, fences, then writes that final even sequence.
   Unknown or failed evidence must remain zero/incomplete, never converted to
   a successful disposition. Generation changes for every producer lifetime.

   PAS_OWNED_SHUTDOWN means this firmware owned PAS1 and observed successful
   shutdown transport AND service after draining its exact continuation. A
   borrowed/live image, a failed startup with unknown remote state, and Stop
   succeeding before PAS ownership are not stopped proof. NativeExitStatus
   reflects the complete native cleanup, with zero OwnedMask, active callbacks
   and IRQ owners. QMP has no pending request; local/remote link and channel
   pairs must agree (up or down) before exclusive ownership transfer. All IPCC
   writers/interrupt callbacks must have retired, not just the PIL callback.

   MemoryDescriptor* records the FINAL reserved-memory descriptor covering
   the carveout; it must survive the OS loader's memory exclusion. The table
   descriptor separately reserves this record itself. FirmwareEl records an
   actual successful PAS caller EL (1 or2), NativeConduit records SMC #0 and
   NativeConvention records the observed standard SIP base0x42000000 or
   0x02000000. These are native observations, not a prediction of Windows EL
   or hypervisor policy. Windows independently checks its actual EL1 and
   probes convention, call availability and PAS1 support without modifying PAS.

   The complete six-resource managed ARC inventory is recorded, including
   foreign native demand; other ARC addresses remain untouched. ManagedAggregate
   provenance1 comes from the sealed native owner manager, not a last TCS command,
   register observation, or the PIL instance's three votes. An explicitly
   acknowledged highest-corner unsynchronized boot-floor owner is labeled
   provenance2. Every power corner then equals the final valid table index and
   XO equals3. It dominates unknown earlier APSS demand without claiming to
   recover earlier owners or to synchronize every OS consumer.
   The Windows ARC provider matches names/addresses/tables and retains imported
   baseline owners before accepting new demands. UNKNOWN provenance rejects
   the handoff. Windows shared IPCC send serialization is a separate runtime
   grant and must be established before this record can enable PIL.
   No PRDY/write gate is derived from this structure without these consumers. */
