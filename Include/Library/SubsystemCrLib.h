/* Independent remote-processor policy. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#include <Library/PILCrLib.h>

#define CR_SUBSYSTEM_CLIENTS 8U
typedef enum { CrSubsystemStopped=0, CrSubsystemStarting, CrSubsystemRunning,
  CrSubsystemClosing, CrSubsystemRetiring, CrSubsystemFaulted } CR_SUBSYSTEM_PHASE;
typedef enum { CrSubsystemNoAction=0, CrSubsystemBeginController,
  CrSubsystemStartImage, CrSubsystemQueryImage, CrSubsystemRetireImage,
  CrSubsystemEndController } CR_SUBSYSTEM_ACTION_KIND;
typedef struct { VOID *Provider; UINT64 Token,Instance; } CR_SUBSYSTEM_LEASE;
typedef struct {
  UINT64 ImageSerial,BindingSerial,BootSerial;
  BOOLEAN ControllerOwned,ImageOwned,Loaded,Authenticated,Ready,Inherited;
  BOOLEAN Owned,AdmissionOpen,RecoveryRequired,RetainedEffects,MechanismRetired;
  /* Copied evidence only; PAS completion is not graph or DMA retirement. */
  CR_PIL_SHUTDOWN_EVIDENCE Shutdown;
  BOOLEAN ReceiptMalformed;
} CR_SUBSYSTEM_OBSERVATION;
typedef struct { UINT64 Serial; UINT32 Kind; } CR_SUBSYSTEM_ACTION;
typedef struct {
  CONST VOID *Owner; CR_SUBSYSTEM_LEASE *ExactLease;
  UINT64 Token,Instance,DrainTicket;
} CR_SUBSYSTEM_CLIENT;
typedef struct {
  UINTN Self;
  UINT64 NextToken,NextAction,Instance,StopTicket,Sequence;
  CR_SUBSYSTEM_ACTION Pending;
  CR_SUBSYSTEM_OBSERVATION Observation;
  CR_SUBSYSTEM_CLIENT Clients[CR_SUBSYSTEM_CLIENTS];
  UINT32 Phase;
  CR_STATUS LastError;
  BOOLEAN Desired,Closing,Faulted,ProviderLost,Malformed,Exhausted;
} CR_SUBSYSTEM;

/* Zero-init exactly once, permanent unmoved storage. All entry points require
 * external PASSIVE serialization. Next journals before the adapter calls the
 * actual loader OUTSIDE that lock; Complete returns under the lock. A single
 * pending operation excludes another worker operation. Begin/Check never wait
 * for starting/stopping; they fail before taking a lease. Execute callers hold
 * this admission serialization through the loader's exact-current closure.
 * No operation here allocates, calls providers or certifies physical effects.
 * Adapter observations derive from exact retained loader receipts; no public
 * readiness setters are exposed by the Windows driver. */
CR_STATUS CrSubsystemInit(CR_SUBSYSTEM *C);
CR_STATUS CrSubsystemDemand(CR_SUBSYSTEM *C,BOOLEAN Desired);
CR_STATUS CrSubsystemProviderLost(CR_SUBSYSTEM *C);
CR_STATUS CrSubsystemNext(CR_SUBSYSTEM *C,CR_SUBSYSTEM_ACTION *Action);
CR_STATUS CrSubsystemComplete(CR_SUBSYSTEM *C,CONST CR_SUBSYSTEM_ACTION *Action,
                            CR_STATUS Result,CONST CR_SUBSYSTEM_OBSERVATION *Observation);
CR_STATUS CrSubsystemBegin(CR_SUBSYSTEM *C,CONST VOID *Owner,CR_SUBSYSTEM_LEASE *Lease);
BOOLEAN CrSubsystemOwns(CR_SUBSYSTEM *C,CONST VOID *Owner,CONST CR_SUBSYSTEM_LEASE *Lease);
CR_STATUS CrSubsystemCheck(CR_SUBSYSTEM *C,CONST VOID *Owner,CONST CR_SUBSYSTEM_LEASE *Lease,
                         UINT64 ExpectedInstance,UINT64 ExpectedBootSerial);
CR_STATUS CrSubsystemDrain(CR_SUBSYSTEM *C,CONST VOID *Owner,CONST CR_SUBSYSTEM_LEASE *Lease,
                         UINT64 ExactStopTicket);
/* End attests only caller-local submissions/callback drainage, never remote
 * graph or DMA retirement. It is legal during closing without a prior Drain.
 * The caller must escrow unresolved remote resources under the old instance. */
CR_STATUS CrSubsystemEnd(CR_SUBSYSTEM *C,CONST VOID *Owner,CR_SUBSYSTEM_LEASE *Lease);
BOOLEAN CrSubsystemHasClients(CONST CR_SUBSYSTEM *C);
BOOLEAN CrSubsystemCanRelease(CONST CR_SUBSYSTEM *C);

/* Private policy source reports a qualified remote fault, preserving all
 * pending actions/images/clients. Distinct from loss of provider identity. */
CR_STATUS CrSubsystemRemoteFault(CR_SUBSYSTEM *,CR_STATUS);
