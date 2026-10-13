/* Independent read-only SMP2P observer. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_SMP2P_HOST 2U
#define CR_SMP2P_ITEM 443U
#define CR_SMP2P_MAX_ENTRIES 16U
#define CR_SMP2P_HEADER_BYTES 20U
#define CR_SMP2P_ITEM_BYTES 340U
#define CR_SMP2P_MAX_READS 180U
#define CR_SMP2P_JOURNAL_ENTRIES 16U
#define CR_SMP2P_FATAL 1U
#define CR_SMP2P_READY 2U
#define CR_SMP2P_HANDOVER 4U
#define CR_SMP2P_STOP_ACK 8U

typedef enum {
  CrSmp2pValid=0, CrSmp2pMissing, CrSmp2pTransient, CrSmp2pMalformed,
  CrSmp2pUnsupported, CrSmp2pSourceLost, CrSmp2pReadFailure
} CR_SMP2P_OUTCOME;
typedef struct { CONST VOID *Address; UINTN Size; } CR_SMP2P_VIEW;
typedef struct {
  VOID *Context;
  CR_STATUS (*Probe)(VOID *Context, CR_SMP2P_VIEW *View);
  CR_STATUS (*Read32)(VOID *Context, CONST CR_SMP2P_VIEW *View,
                     UINT32 Offset, UINT32 *Value);
  VOID (*Fence)(VOID *Context);
  UINT64 Incarnation;
  UINT32 Host, Item;
} CR_SMP2P_IO;
typedef struct {
  UINTN Self;
  CONST VOID *Source;
  UINT64 Incarnation, CaptureSerial, EventSequence;
  CR_SMP2P_VIEW View;
  UINT32 Header[5], ConfirmHeader[5], Value, EntryIndex, Reads;
  UINT32 Outcome;
  CR_STATUS Error;
  BOOLEAN Stable;
} CR_SMP2P_SNAPSHOT;
typedef struct {
  UINTN Self;
  CR_SMP2P_IO Io;
  UINT64 LastIncarnation, CaptureSerial;
  CR_SMP2P_SNAPSHOT Receipt;
  CR_SMP2P_SNAPSHOT *ExactSnapshot;
  VOID *Journal;
  BOOLEAN Attached, Exhausted, Lost;
} CR_SMP2P_SOURCE;
typedef struct {
  VOID *Journal;
  CONST VOID *Owner;
  UINT64 Token, Incarnation, ControllerSerial, StartActionSerial;
} CR_SMP2P_LEASE;
#define CR_SMP2P_FAULT_FATAL          0x0001U
#define CR_SMP2P_FAULT_READY_LOST     0x0002U
#define CR_SMP2P_FAULT_RESTART        0x0004U
#define CR_SMP2P_FAULT_IDENTITY       0x0008U
#define CR_SMP2P_FAULT_MISSING        0x0010U
#define CR_SMP2P_FAULT_MALFORMED      0x0020U
#define CR_SMP2P_FAULT_UNSUPPORTED    0x0040U
#define CR_SMP2P_FAULT_SOURCE_LOST    0x0080U
#define CR_SMP2P_FAULT_READ           0x0100U
#define CR_SMP2P_FAULT_EXHAUSTED      0x0200U
typedef struct {
  UINT64 Serial, Token, Incarnation, ControllerSerial, StartActionSerial;
  UINT64 ImageSerial, BindingSerial, BootSerial, EventSequence, CaptureSerial;
  UINT32 Outcome, FaultReasons, PreviousValue, Value, Rising, Falling, Header[5];
  CR_STATUS Error;
} CR_SMP2P_EVENT;
typedef struct {
  UINT64 Token, Incarnation, ControllerSerial, StartActionSerial;
  UINT64 ImageSerial, BindingSerial, BootSerial;
  UINT64 EventSequence, ConsumedEventSequence, CaptureSerial, BindCaptureSerial;
  UINT32 Value, FaultReasons;
  CR_STATUS Error;
  BOOLEAN Armed, Pending, Stable, Bound, FreshAfterBind, ReadyObserved, Faulted;
} CR_SMP2P_OBSERVATION;
typedef struct {
  UINTN Self;
  UINT64 NextToken, Sequence, LastIncarnation, LastController, LastStartAction;
  UINT64 LastImage, LastBinding, LastBoot, LastRecordedCapture;
  CR_SMP2P_SOURCE *Source;
  CR_SMP2P_LEASE *ExactLease;
  CONST VOID *Owner;
  CR_SMP2P_OBSERVATION State;
  CR_SMP2P_SNAPSHOT Previous;
  CR_SMP2P_EVENT Events[CR_SMP2P_JOURNAL_ENTRIES];
  BOOLEAN HavePrevious, SeenReady, Exhausted;
} CR_SMP2P_JOURNAL;

/* All storage is zero initialized once, permanent and unmoved. Caller provides
 * PASSIVE serialization for source/journal and all provider operations. Source
 * Attach accepts only a strictly increasing caller-owned access incarnation;
 * that number is NOT a provider generation, image identity or reboot proof.
 * Probe uses an already retained SMEM access lease; Address is only the actual
 * item address, never dereferenced here or treated as incarnation authority.
 * Probe/Read32 return exact CR_SUCCESS, CR_NOT_FOUND for an absent item,
 * CR_NOT_READY/CR_ABORT for a lost lease, or an explicit read error. Read32 must
 * refuse accesses outside View.Size and must not retain output pointers.
 * Fence orders actual coherent shared-memory reads on the target. Callbacks
 * cannot reenter this source/journal. No API allocates, sends or writes SMEM.
 *
 * Capture makes at most 180 aligned word reads in two header-bracketed passes.
 * Equal samples are observed stability, NOT atomicity or protection from ABA
 * changes entirely between reads. The ISR event fence belongs to the adapter.
 * Capture returns a diagnostic snapshot on read/format non-success (parameter,
 * detached and exhausted-counter failures do not produce one); Record accepts
 * only that source's exact latest, unchanged output storage once. Copies,
 * stale snapshots and overlapping storage are rejected without mutation.
 * Record success means recorded, including recorded fault diagnostics; callers
 * inspect Observe rather than interpreting it as a ready indication.
 * SourceLost is sticky in the journal. Missing before bind/READY and Transient
 * preserve Pending: caller must not complete/rearm an unconsumed IRQ from them.
 *
 * Arm precedes LoadAndStart. Bind accepts only actual loader receipt IDs for
 * that exact controller/start action, preserves prior faults, and requires a
 * new Capture before FreshAfterBind. Observation grants no readiness, image,
 * retirement, restart or DMA authority. A caller combines it with its actual
 * loader result and exact atomic event fence. Close certifies only local
 * observer detachment; caller must already drain route callbacks/worker use.
 * Journal entries are immutable until bounded-ring eviction; ReadEvent returns
 * NOT_FOUND for an evicted serial. Sequence/token/capture counters never wrap.
 */
CR_STATUS CrSmp2pSourceInit(CR_SMP2P_SOURCE *Source);
CR_STATUS CrSmp2pSourceAttach(CR_SMP2P_SOURCE *Source, CONST CR_SMP2P_IO *Io);
CR_STATUS CrSmp2pSourceDetach(CR_SMP2P_SOURCE *Source);
CR_STATUS CrSmp2pCapture(CR_SMP2P_SOURCE *Source, UINT64 EventSequence,
                       CR_SMP2P_SNAPSHOT *Snapshot);
CR_STATUS CrSmp2pJournalInit(CR_SMP2P_JOURNAL *Journal);
CR_STATUS CrSmp2pArm(CR_SMP2P_JOURNAL *Journal, CONST VOID *Owner,
                    CR_SMP2P_SOURCE *Source, UINT64 ControllerSerial,
                    UINT64 StartActionSerial, UINT64 PreStartEventSequence,
                    CR_SMP2P_LEASE *Lease);
CR_STATUS CrSmp2pRecord(CR_SMP2P_JOURNAL *Journal, CONST VOID *Owner,
                       CONST CR_SMP2P_LEASE *Lease,
                       CONST CR_SMP2P_SNAPSHOT *Snapshot);
CR_STATUS CrSmp2pBind(CR_SMP2P_JOURNAL *Journal, CONST VOID *Owner,
                     CONST CR_SMP2P_LEASE *Lease, UINT64 ControllerSerial,
                     UINT64 StartActionSerial, UINT64 ImageSerial,
                     UINT64 BindingSerial, UINT64 BootSerial);
CR_STATUS CrSmp2pObserve(CONST CR_SMP2P_JOURNAL *Journal, CONST VOID *Owner,
                        CONST CR_SMP2P_LEASE *Lease,
                        CR_SMP2P_OBSERVATION *Observation);
CR_STATUS CrSmp2pReadEvent(CONST CR_SMP2P_JOURNAL *Journal, UINT64 Serial,
                          CR_SMP2P_EVENT *Event);
CR_STATUS CrSmp2pClose(CR_SMP2P_JOURNAL *Journal, CONST VOID *Owner,
                      CR_SMP2P_LEASE *Lease);
