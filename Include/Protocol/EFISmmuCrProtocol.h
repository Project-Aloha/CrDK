/**
  Crane SMMU management protocol.

  The protocol is deliberately separate from EDKII_IOMMU_PROTOCOL.  PCIe
  cold-init owns stream quiescing and calls Attach() before it releases PERST
  or enables bus mastering.  BlockSegment() is the shutdown hand-off and must
  be called after the PCIe endpoint has stopped issuing DMA.

  Bdf == 0xFFFF identifies an on-chip client (UFS/GPI/MDSS). BlockSegment()
  releases only PCI domains, never these on-chip domains. This revision has
  no per-SID detach method. Unmap() revokes a mapping, leaving its domain
  attached. Neither operation releases an inherited firmware-owned stream.

  SPDX-License-Identifier: MIT
**/
#ifndef EFI_SMMU_CR_PROTOCOL_H_
#define EFI_SMMU_CR_PROTOCOL_H_

#include <Uefi.h>
#include <Library/smmu.h>
#include <Library/SmmuDisplayCrLib.h>

#define EFI_SMMU_CR_PROTOCOL_GUID                                             \
  { 0x4c0eced0, 0x2f84, 0x4e6f, { 0xa3, 0xf0, 0x20, 0x1f, 0xa9, 0x55, 0x4e, 0x31 } }

#define EFI_SMMU_CR_PROTOCOL_REVISION  0x0001000BULL
#define EFI_SMMU_CR_DISPLAY_OBSERVATION_REVISION 0x00010009ULL
#define EFI_SMMU_CR_PRESERVE_MAPPING_REVISION  0x00010001ULL
#define EFI_SMMU_CR_EXIT_EVIDENCE_REVISION 0x00010002ULL
#define EFI_SMMU_CR_EXIT_REVOCATION_REVISION 0x00010003ULL
#define EFI_SMMU_CR_EXIT_DOMAIN_REVISION 0x00010004ULL
#define EFI_SMMU_CR_EXIT_USB_PHYSICAL_REVISION 0x00010005ULL
#define EFI_SMMU_CR_EXIT_UFS_COHERENT_REVISION 0x00010006ULL
#define EFI_SMMU_CR_FAULT_DIAGNOSTIC_REVISION 0x00010007ULL
#define EFI_SMMU_CR_STOPPED_UFS_REVISION 0x00010008ULL
#define EFI_SMMU_CR_STOPPED_USB_REVISION 0x0001000AULL
#define EFI_SMMU_CR_MAX_EVIDENCE_STREAMS 32U

extern EFI_GUID  gEfiSmmuCrProtocolGuid;

typedef struct _EFI_SMMU_CR_PROTOCOL EFI_SMMU_CR_PROTOCOL;

#include <Protocol/EFISmmuCrDisplay.h>

typedef
EFI_STATUS
(EFIAPI *EFI_SMMU_CR_ATTACH)(
  IN EFI_SMMU_CR_PROTOCOL  *This,
  IN UINT16                 Segment,
  IN UINT16                 Bdf,
  IN UINT16                 Sid
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SMMU_CR_BLOCK_SEGMENT)(
  IN EFI_SMMU_CR_PROTOCOL  *This,
  IN UINT16                 Segment
  );

/**
  Retain a reserved, read-only mapping for framebuffer scanout after
  ExitBootServices. Mapping is a token from this producer's IOMMU Map().
  The entire backing page range must be GCD Reserved. The caller must retain
  the reserved backing allocation until OS display takeover.
  No write/common-buffer or bounce mappings may be retained. Unmap() removes
  this policy along with the mapping. At exit all other mappings in each
  retained stream are revoked; a failed revoke causes the stream to be blocked.
**/
typedef
EFI_STATUS
(EFIAPI *EFI_SMMU_CR_PRESERVE_READ_ONLY_MAPPING)(
  IN EFI_SMMU_CR_PROTOCOL *This,
  IN VOID                *Mapping
  );

typedef struct {
  UINT32 Size, Revision, MappingId, StreamCount;
  UINT64 HostAddress, DeviceAddress, NumberOfBytes;
  UINT64 PhysicalBase, IovaBase, PageBytes;
  SmmuMappingEvidence Streams[EFI_SMMU_CR_MAX_EVIDENCE_STREAMS];
} EFI_SMMU_CR_MAPPING_EVIDENCE;

/* This pure query succeeds only after this producer's entire EBS pass has
   finished and every surviving grant for the exact Map token is verified.
   It performs no allocation, mapping change or cache/TLB operation. The
   output does not grant an OS permission to map or release these resources. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_QUERY_EXIT_MAPPING)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN VOID *Mapping,
  OUT EFI_SMMU_CR_MAPPING_EVIDENCE *Evidence);

/* Completion is separate from entering EBS: observers run outside the lock
   after every domain has been preserved/blocked or its failure recorded.
   A consumer must still query its own mapping; global success alone is not
   a per-stream grant. Register before EBS. Unregister with Callback=NULL
   before owner unload; replacing a registered owner's callback is forbidden.
   Callbacks may query evidence but must not allocate/free or mutate DMA,
   except the explicit board-selected USB/UFS physical exit transitions. */
typedef VOID (EFIAPI *EFI_SMMU_CR_EXIT_OBSERVER)(IN VOID *Context, IN EFI_STATUS Status);
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_SET_EXIT_OBSERVER)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN VOID *Owner,
  IN EFI_SMMU_CR_EXIT_OBSERVER Callback OPTIONAL, IN VOID *Context OPTIONAL);

/* Called before any EBS translation revocation and outside the SMMU lock.
   An owner freezes software work and stops transient DMA here. No allocation,
   mapping changes or boot-service lifetime changes are permitted. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_EXIT_PREPARE)(IN VOID *Context);
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_SET_EXIT_PREPARE_OBSERVER)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN VOID *Owner,
  IN EFI_SMMU_CR_EXIT_PREPARE Callback OPTIONAL, IN VOID *Context OPTIONAL);

/* Pure full-domain revocation evidence after the completed EBS pass. The
   exact native Segment/Bdf/Sid must still identify this producer's owned,
   non-preserved domain. Verifies all64MiB of its IOVA aperture have zero
   leaves, completed TLBI and unchanged live stream/context ownership. No Map
   token is required: USB rings have many write/common-buffer mappings, and a
   single read-only mapping could not prove their complete revocation.
   Missing, inherited, poisoned or partially retained domains are failures,
   never an empty-success acknowledgment. This does not prove the controller
   or its software writers stopped and grants no OS DMA ownership. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_QUERY_EXIT_REVOKED_DOMAIN)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN UINT16 Segment, IN UINT16 Bdf, IN UINT16 Sid,
  OUT SmmuMappingEvidence *Evidence);

/* Legacy HDK8450 USB debug transport uses physical DMA addresses and has no
   HAL/IORT mapper. This optional, board-selected exception is callable only
   from the completed EBS observer pass. The USB owner must first freeze/drain
   every writer and verify device-mode halt and disabled interrupts. It then
   releases only the producer's exact on-chip SID0 context to physical DMA.
   The producer revalidates the entire revoked, exclusive domain under its lock;
   it retains all pages/reservations and does not touch global SMMU controls.
   This ends native mapping/revocation authority: USBH must not advertise this
   as a revoked domain available for the normal CR Windows takeover. Disabled
   board policy, earlier/later calls and foreign ownership fail without writes. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_RELEASE_USB_TO_PHYSICAL_AT_EXIT)(
  IN EFI_SMMU_CR_PROTOCOL *This);

/* The legacy HDK8450 DSDT declares UFS0._CCA=1 and has no OS IORT route.
   After UFS software admission is closed and HCE, both request lists,
   doorbells and interrupts are stopped, release only its owned SID0xe0 to
   coherent physical DMA. Callable only during the completed EBS observer
   pass, with explicit board policy. The provider independently checks the
   stopped HCI and full revoked/exclusive domain; it retains all native
   backing and verifies the cache/shareability override. This is not a HAL
   domain grant and must never be selected with the Windows CR IORT profile. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_RELEASE_UFS_TO_COHERENT_PHYSICAL_AT_EXIT)(
  IN EFI_SMMU_CR_PROTOCOL *This);

/* Before controller reset/revocation, emit a read-only snapshot of the exact
   owned native identity and its live DMA spans. Includes failed domains.
   No allocation, writes, cache/TLB maintenance or fault acknowledgement.
   Available before EBS only. Success indicates capture, not DMA health. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_DUMP_FAULT)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN UINT16 Segment, IN UINT16 Bdf, IN UINT16 Sid);

/* Target-qualified UFS only, implicit segment0/BDFffff. Caller serializes the
   reset/clock/HCI producer and proves reset-low/completed core reset through
   VerifyStopped. Adapter independently reads IE/HCE/run-stop/doorbells twice.
   Qualification repeats before writes, before exact S2CR commit and afterward.
   An inherited stream never restores its old route or enters physical DMA;
   acquired ownership/tables remain pinned. Repeat calls check stopped state
   before returning the same existing identity without any route writes. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_UFS_VERIFY_STOPPED)(IN VOID *Context);
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_ACQUIRE_UFS_STREAM)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN UINT64 HciBase, IN UINT16 Sid,
  IN EFI_SMMU_CR_UFS_VERIFY_STOPPED VerifyStopped, IN VOID *Context);

/* Explicit SM8250/8350 USB stopped acquisition, segment0/BDFffff. The exact
 * native controller owner proves completed wrapper reset, live prerequisites
 * and exclusion of every former child/software writer through VerifyStopped.
 * It is a pure non-reentrant callback; it must not call any SMMU service.
 * The producer independently checks the exact target's DRD IP/host mode,
 * halted xHCI with RUN/INTE clear, function run/interrupt/endpoint enables
 * clear, and retired event-ring0. These checks repeat before private-bank
 * writes, immediately before S2CR commit and after it. Any postwrite loss
 * pins the tables/route and poisons retries. A successful domain retains
 * the exact callback/context identity; a different owner cannot reuse it.
 * An inherited stream never
 * restores a former route or enters physical DMA through this API. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_USB_VERIFY_STOPPED)(IN VOID *Context);
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_ACQUIRE_USB_STREAM)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN UINT64 ControllerBase, IN UINT16 Sid,
  IN EFI_SMMU_CR_USB_VERIFY_STOPPED VerifyStopped, IN VOID *Context);

/* Pure observation of the exact inherited HDK nonsecure display set. Refuses
   native-owned or overlapping/shared/nested routes; validates the complete
   range in the target's permanent splash reservation. TableRead must only
   access retained CPU-mapped reserved memory and never call SMMU services.
   Does not attach, map, invalidate, acknowledge faults or grant DMA ownership. */
typedef EFI_STATUS (EFIAPI *EFI_SMMU_CR_QUERY_INHERITED_DISPLAY)(
  IN EFI_SMMU_CR_PROTOCOL *This, IN UINT64 Device, IN UINT64 Bytes,
  IN CR_SMMU_DISPLAY_TABLE_READ TableRead, IN VOID *Context,
  OUT CR_SMMU_DISPLAY_OBSERVATION *Observation);

struct _EFI_SMMU_CR_PROTOCOL {
  UINT64                    Revision;
  EFI_SMMU_CR_ATTACH        Attach;
  EFI_SMMU_CR_BLOCK_SEGMENT BlockSegment;
  EFI_SMMU_CR_PRESERVE_READ_ONLY_MAPPING PreserveReadOnlyMapping;
  EFI_SMMU_CR_QUERY_EXIT_MAPPING QueryExitMapping;
  EFI_SMMU_CR_SET_EXIT_OBSERVER SetExitObserver;
  /* Revoked read-only Map token: checks every original stream even when the
     software grant was cleared during EBS. Streams have Access/Physical zero.
     Missing tokens and failed queries are never revocation acknowledgements. */
  EFI_SMMU_CR_QUERY_EXIT_MAPPING QueryExitRevokedMapping;
  EFI_SMMU_CR_SET_EXIT_PREPARE_OBSERVER SetExitPrepareObserver;
  EFI_SMMU_CR_QUERY_EXIT_REVOKED_DOMAIN QueryExitRevokedDomain;
  EFI_SMMU_CR_RELEASE_USB_TO_PHYSICAL_AT_EXIT ReleaseUsbToPhysicalAtExit;
  EFI_SMMU_CR_RELEASE_UFS_TO_COHERENT_PHYSICAL_AT_EXIT ReleaseUfsToCoherentPhysicalAtExit;
  EFI_SMMU_CR_DUMP_FAULT DumpFault;
  EFI_SMMU_CR_ACQUIRE_UFS_STREAM AcquireUfsStream;
  EFI_SMMU_CR_QUERY_INHERITED_DISPLAY QueryInheritedDisplay;
  EFI_SMMU_CR_ACQUIRE_USB_STREAM AcquireUsbStream;
  EFI_SMMU_CR_ACQUIRE_DISPLAY_GROUP AcquireDisplayGroup;
  EFI_SMMU_CR_BIND_DISPLAY_FRAME BindDisplayFrame;
  EFI_SMMU_CR_ALLOCATE_DISPLAY_COMMAND AllocateDisplayCommand;
  EFI_SMMU_CR_DISPLAY_BUFFER_OPERATION PrepareDisplayBuffer;
  EFI_SMMU_CR_DISPLAY_BUFFER_OPERATION CheckDisplayBuffer;
  EFI_SMMU_CR_QUERY_DISPLAY_BUFFER QueryDisplayBuffer;
  EFI_SMMU_CR_DISPLAY_BUFFER_OPERATION PreserveDisplayFrame;
  EFI_SMMU_CR_DISPLAY_BUFFER_OPERATION ReleaseDisplayBuffer;
};

#endif
