/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Crane contributors.
 * Protocol provenance: README.md in Library/QteeDiscoveryCrLib.
 */
#ifndef CR_QTEE_DISCOVERY_H
#define CR_QTEE_DISCOVERY_H
#include <oskal/cr_types.h>
#define CR_QTEE_PAGE 4096u
#define CR_QTEE_BYTES (2u * CR_QTEE_PAGE)
#define CR_QTEE_OBJECTS 8u
/* This is an INTERNAL resident-provider ABI, never an IOCTL/user request. */
typedef enum { CrQteeOk, CrQteeInvalid, CrQteeBusy, CrQteeUnavailable,
  CrQteeObjectError, CrQteeCancelled, CrQteeRetained, CrQteeNotIssued } CR_QTEE_STATUS;
typedef enum { CrQteeBridgeEnable, CrQteeBridgeCreate, CrQteeInvoke,
  CrQteeBridgeDelete } CR_QTEE_OPERATION;
typedef struct {
  void *Provider;
  UINT64 Generation;
  UINT8 *Buffer; /* Exact contiguous, nonpaged, privately owned 8 KiB. */
  UINT64 Physical;
  UINT32 Bytes;
  /* Runtime qualification, not a caller assertion or Android-DT inference. */
  UINT32 Profile; /* 1: SMC64, explicit HLOS VMID3/TZ RW, modern invoke. */
} CR_QTEE_LEASE;
typedef struct {
  CR_QTEE_OPERATION Operation;
  UINT32 Owner, Service, Command, ArgInfo;
  UINT64 Args[4];
} CR_QTEE_CALL;
typedef struct { UINT64 X[4]; } CR_QTEE_REPLY;
typedef struct {
  void *Context;
  /* Acquire pins permanent provider/code, memory and GLOBAL SCM owner. Even
     failure must return the exact non-NULL Provider if a partial pin exists. */
  CR_QTEE_STATUS (*Acquire)(void *, CR_QTEE_LEASE *);
  CR_QTEE_STATUS (*Check)(void *, const CR_QTEE_LEASE *);
  /* Sync implements the qualified cache/barrier contract for BOTH pages. */
  CR_QTEE_STATUS (*Sync)(void *, const CR_QTEE_LEASE *, int ToSecure);
  /* The sole SCM backend consumes only these fixed typed operations. Exact
     Ok means the submitted call retired, with an actual raw reply. NotIssued is allowed ONLY for terminal capability rejection before any
     bridge operation; no secure effect or pending continuation exists. Any other
     result may have effects: caller retains the lease/entire journal. The
     backend owns INTERRUPTED resume, x6 and global exclusivity; no bypass. */
  CR_QTEE_STATUS (*Execute)(void *, const CR_QTEE_LEASE *,
    const CR_QTEE_CALL *, CR_QTEE_REPLY *);
  /* Exact Ok removes the final pin; failure MUST retain the exact lease. */
  CR_QTEE_STATUS (*Release)(void *, CR_QTEE_LEASE *);
  int (*Cancelled)(void *);
  /* Close the already recorded global owner by exact lease STORAGE identity,
     even when its mutable fields fail validation. No release or token repair. */
  void (*Quarantine)(void *, const CR_QTEE_LEASE *);
} CR_QTEE_IO;
typedef struct { UINT32 Handle; UINT8 Live; } CR_QTEE_OBJECT;
typedef struct {
  UINT32 Revision;
  UINT8 Hdcp2p2Present, HdcpSrmPresent;
  INT32 ObjectResult;
} CR_QTEE_DISCOVERY_RESULT;
typedef struct CR_QTEE_DISCOVERY {
  const struct CR_QTEE_DISCOVERY *Identity;
  CR_QTEE_IO Io;
  CR_QTEE_LEASE Lease, OriginalLease;
  CR_QTEE_OBJECT Objects[CR_QTEE_OBJECTS];
  UINT32 ObjectCount;
  UINT8 Active, Busy, Closed, Created, Cancelled;
  UINT64 BridgeHandle;
  CR_QTEE_CALL LastCall;
  CR_QTEE_REPLY LastReply;
  UINT8 Expected[CR_QTEE_PAGE];
  UINT32 OutputOffset;
  CR_QTEE_DISCOVERY_RESULT Result;
} CR_QTEE_DISCOVERY;
/* Initialize only fresh zero-filled permanent state, no in-place reset/reinit.
   Calls are serialized by the trusted resident host. No external mutable
   state or user pointers may alias the state or the acquired DMA pages. */
CR_QTEE_STATUS CrQteeInitialize(CR_QTEE_DISCOVERY *, const CR_QTEE_IO *);
CR_QTEE_STATUS CrQteeDiscover(CR_QTEE_DISCOVERY *, CR_QTEE_DISCOVERY_RESULT *);
/* Retry only a failed provider-pin release or partial Acquire with no wire
   effect. Closed/uncertain operations cannot be guessed complete. */
CR_QTEE_STATUS CrQteeRetire(CR_QTEE_DISCOVERY *);
#endif
