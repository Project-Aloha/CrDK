/* Independent ADSP owner; staged ABI, root publishes after integration. MIT */
#pragma once
#include <ntddk.h>
#include <Library/SubsystemCrLib.h>
#include <Interface/CrPilLoaderPublic.h>
#define WDF_SUBSYSTEM_CR_REVISION 1U
DEFINE_GUID(GUID_DEVINTERFACE_SUBSYSTEM_CR,0x70960225,0xb1a3,0x447e,0xab,0xc5,0xc4,0xec,0xd0,0x5e,0xce,0x16);
DEFINE_GUID(GUID_SUBSYSTEM_CR_INTERFACE,0xb8d23c0f,0x73df,0x40a3,0xa8,0x0e,0x4a,0x9e,0x29,0x41,0x1b,0x59);
typedef struct {
  UINT32 Size,Revision,Phase;
  UINT64 Instance,StopTicket,Sequence;
  NTSTATUS LastError;
  BOOLEAN Desired,Closing,Faulted,ProviderLost,Malformed;
  CR_SUBSYSTEM_OBSERVATION Processor;
} WDF_SUBSYSTEM_CR_STATE;
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Begin)(PVOID,PVOID Owner,CR_SUBSYSTEM_LEASE *,WDF_SUBSYSTEM_CR_STATE *);
  NTSTATUS (*State)(PVOID,PVOID Owner,CONST CR_SUBSYSTEM_LEASE *,WDF_SUBSYSTEM_CR_STATE *);
  NTSTATUS (*ExecuteCurrent)(PVOID,PVOID Owner,CONST CR_SUBSYSTEM_LEASE *,UINT64 Instance,
    UINT64 BootSerial,CR_PIL_LOADER_OPERATION,PVOID,CR_PIL_LOADER_EXECUTION *);
  NTSTATUS (*AcknowledgeLocalDrain)(PVOID,PVOID Owner,CONST CR_SUBSYSTEM_LEASE *,UINT64 StopTicket);
  NTSTATUS (*End)(PVOID,PVOID Owner,CR_SUBSYSTEM_LEASE *);
  NTSTATUS (*ReferenceDependency)(PVOID,PVOID Owner,CONST CR_SUBSYSTEM_LEASE *,PDEVICE_OBJECT *);
} WDF_SUBSYSTEM_CR_INTERFACE;
/* PASSIVE, referenced kernel interface, stable exact Owner/lease storage.
 * State/Begin snapshots are copied, not current-send authority. Execute is the
 * ONLY trusted current-instance token/copy fence, holds subsystem admission ->
 * actual PIL/origin, and requires a nonzero authenticated BootSerial. Callback
 * never waits, polls, calls providers, allocates or reacquires outer locks.
 * Its explicit Accepted/Unknown survives a later error or source loss.
 * Begin succeeds only after actual requested load+READY. No client starts or
 * restarts ADSP. State continues reporting an exact old lease during closing.
 * StopTicket is level-triggered: poll it with the already-existing transport
 * worker, close local sends/callbacks, escrow old graph/DMA, then acknowledge
 * that exact ticket or End. No event queue can lose a required invalidation.
 * End certifies ONLY caller-local drainage and never remote retirement.
 * ReferenceDependency returns one referenced actual subsystem PDO; release it
 * with ObDereferenceObject. Do not make the whole resource PEP depend on it.
 * No readiness, firmware path, restart, PAS or physical-address user IOCTL.
 * Typed outputs/lease storage require 8-byte alignment and cannot overlap
 * provider storage or any retained client lease.
 * Failed Begin with a nonempty exact lease remains owned; End it explicitly.
 * S1 lacks automatic recovery and IRQ supervision; processor READY, namespace,
 * GLINK, PD, graph, codec and DMA readiness remain separate. */
