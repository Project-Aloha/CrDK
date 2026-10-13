/* Requested image mechanism; subsystem policy is a separate owner. MIT.
 * S1 ABI: actual module query publication; not exported/deployed yet. */
#pragma once
#include <Interface/CrPilOriginPublic.h>
#define CR_PIL_LOADER_REVISION 1U
#define CR_PIL_LOADER_HEADER_PART MAXULONG
DEFINE_GUID(GUID_PIL_LOADER_CR_INTERFACE,0xd82ee4ba,0xc456,0x46d4,0xbf,0x12,0x95,0x3a,0x70,0xdd,0x4d,0xb9);

typedef struct {PVOID Provider;UINT64 Token;} CR_PIL_LOADER_CONTROLLER;
typedef struct {PVOID Provider;UINT64 ControllerToken,Token;} CR_PIL_LOADER_IMAGE;
typedef struct {
  INTERFACE Header;
  UINT64 PackageSerial; /* Immutable package identity; never boot authority. */
  NTSTATUS (*CheckImmutable)(PVOID,UINT64 PackageSerial);
  NTSTATUS (*GetPartSize)(PVOID,UINT64 PackageSerial,UINT32 Part,UINT64 *Bytes);
  NTSTATUS (*ReadPart)(PVOID,UINT64 PackageSerial,UINT32 Part,UINT64 Offset,
                       PVOID Destination,ULONG Bytes,ULONG *ReadBytes);
} CR_PIL_LOADER_SOURCE;
/* Referenced kernel package reader selected by SubsystemCr, never an IOCTL
 * callback/path. HEADER_PART is MDT/MBN; split indexes are 0..99. All parts
 * must remain immutable through the reference: open handles deny WRITE and
 * DELETE sharing before LoadAndStart. Methods are synchronous PASSIVE under
 * PIL Operations, never call Subsystem/GLINK/PIL or borrow Destination after
 * return. Only exact STATUS_SUCCESS permits output consumption. A nonfinal
 * effect keeps the exact image/source journal; there is no completion ABI. */
typedef struct {
  UINT32 Size,Revision;
  UINT64 ImageSerial,BindingSerial,BootSerial;
  BOOLEAN Loaded,AuthCompleted,ReadyObserved,Inherited,Owned,AdmissionOpen;
  BOOLEAN RecoveryRequired,RetainedEffects,MechanismRetired;
  NTSTATUS LastError;
  CR_PIL_SHUTDOWN_EVIDENCE Shutdown;
} CR_PIL_LOADER_OBSERVATION;
/* Caller initializes Size/Revision to exact v1 values. On exact SUCCESS,
 * ImageSerial matches the image's token and BindingSerial is the real binding;
 * BootSerial may be zero for an inherited/unqualified running observation.
 * Only Execute requires a nonzero qualified current boot. Retire success
 * zeroes Image but returns MechanismRetired for its original ImageSerial;
 * failure keeps the exact nonzero receipt and real partial observation.
 * Invalid preflight leaves output unchanged; after an exact receipt is accepted,
 * Query/Retire fill a versioned current observation even on physical failure.
 * Only exact status success qualifies the semantic outcome. */
typedef CR_PIL_AUDIO_EXECUTION CR_PIL_LOADER_EXECUTION;
typedef CR_PIL_AUDIO_OPERATION CR_PIL_LOADER_OPERATION;
typedef struct {
  INTERFACE Header;
  NTSTATUS (*BeginController)(PVOID,PVOID Owner,CR_PIL_LOADER_CONTROLLER *);
  NTSTATUS (*LoadAndStart)(PVOID,PVOID Owner,CONST CR_PIL_LOADER_CONTROLLER *,
                          CONST CR_PIL_LOADER_SOURCE *ExactSource,CR_PIL_LOADER_IMAGE *,
                          CR_PIL_LOADER_OBSERVATION *);
  NTSTATUS (*QueryImage)(PVOID,PVOID Owner,CONST CR_PIL_LOADER_CONTROLLER *,
                       CONST CR_PIL_LOADER_IMAGE *,CR_PIL_LOADER_OBSERVATION *);
  NTSTATUS (*ExecuteCurrent)(PVOID,PVOID Owner,CONST CR_PIL_LOADER_CONTROLLER *,
                            CONST CR_PIL_LOADER_IMAGE *,UINT64 ExpectedBootSerial,
                            CR_PIL_LOADER_OPERATION,PVOID,CR_PIL_LOADER_EXECUTION *);
  NTSTATUS (*RetireImage)(PVOID,PVOID Owner,CONST CR_PIL_LOADER_CONTROLLER *,
                        CR_PIL_LOADER_IMAGE *,CR_PIL_LOADER_OBSERVATION *);
  NTSTATUS (*EndController)(PVOID,PVOID Owner,CR_PIL_LOADER_CONTROLLER *);
} WDF_PIL_LOADER_CR_INTERFACE;
/* PASSIVE, one exclusive controller; Owner/controller/image/source storage is
 * exact and unmoved while owned. Commit controller/image receipts before first
 * possible effects. Nonzero receipts on failure remain owned; copies cannot
 * act or retire. Query refs pin native storage/code, not WDF child objects.
 * Begin qualifies the existing fixed board target; no caller PAS/address grant.
 * D0 only prepares service and never loads. Only caller-requested LoadAndStart
 * loads; no autonomous restart. RetireImage retries actual PAS continuation,
 * shutdown, QMP/votes/metadata/binding retirement and never discards uncertainty.
 * A terminal Shutdown alone is separate from MechanismRetired. Clear image only
 * after full actual retirement, retain source through partial work; End refuses
 * an image or retained effect. D0Exit cannot replace a requested image stop.
 * ExecuteCurrent is a private trusted bounded token/copy closure under PIL->
 * origin; no outer lock, provider, wait, allocation, poll, dispatch or SMC.
 * Accepted/Unknown persists even when status/source loss occurs after entry.
 * No graph/DMA, GLINK/PD readiness or inherited authentication is certified. */
