/* Qualified audio service on the permanent PEP ADSP edge. GPL-2.0-only */
#pragma once
#include <Interface/WdfGlinkServicePublic.h>
#define WDF_AUDIO_GPR_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_AUDIO_GPR_CR_INTERFACE,0x479799ec,0x1492,0x40b4,0xa4,0xcc,0x74,0x27,0x99,0x94,0x10,0x39);
typedef CR_GLINK_SERVICE_LEASE CR_AUDIO_GPR_LEASE;
typedef struct { UINT64 Channel; UINT32 Adsp,AudioPd; } CR_AUDIO_GPR_EPOCH;
typedef struct {
 CR_AUDIO_GPR_EPOCH Epoch;
 UINT64 NotifierIncarnation;
 UINT32 AudioPdState,NotifierNode,NotifierPort;
 BOOLEAN AdspReady,ChannelOnline,AudioPdKnown,AudioPdReady;
 BOOLEAN NamespaceQualified,Faulted,Quarantined,More;
 NTSTATUS LastError;
} CR_AUDIO_GPR_INFORMATION;
typedef enum { CrAudioGprNotAccepted,CrAudioGprAccepted,CrAudioGprUnknown } CR_AUDIO_GPR_ACCEPTANCE;
typedef struct { NTSTATUS Status; CR_AUDIO_GPR_ACCEPTANCE Acceptance; } CR_AUDIO_GPR_SEND_RESULT;
typedef CR_STATUS (*CR_AUDIO_GPR_RECEIVE)(PVOID,CONST UINT8 *,UINTN,CONST CR_AUDIO_GPR_EPOCH *);
typedef VOID (*CR_AUDIO_GPR_STATE_CALLBACK)(PVOID,CONST CR_AUDIO_GPR_INFORMATION *);
typedef struct {
 UINT32 IntentCount,IntentBytes;
 VOID *RxStorage;UINTN RxBytes;
 VOID *TxStorage;UINTN TxBytes;
 CR_AUDIO_GPR_RECEIVE Receive;CR_AUDIO_GPR_STATE_CALLBACK State;PVOID Context;
} CR_AUDIO_GPR_CONFIG;
typedef struct {
 INTERFACE Header;
 NTSTATUS (*Begin)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_CONFIG *,CR_GLINK_SERVICE_WAKE,PVOID,CR_AUDIO_GPR_LEASE *);
 NTSTATUS (*End)(PVOID,PVOID Owner,CR_AUDIO_GPR_LEASE *);
 NTSTATUS (*Pump)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,CR_AUDIO_GPR_INFORMATION *);
 NTSTATUS (*GetState)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,CR_AUDIO_GPR_INFORMATION *);
 NTSTATUS (*Reopen)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *);
 NTSTATUS (*ReserveSpfToken)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,CONST CR_AUDIO_GPR_EPOCH *,UINT32 *Token);
 NTSTATUS (*Send)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,CONST CR_AUDIO_GPR_EPOCH *,CONST VOID *,UINT32,CR_AUDIO_GPR_SEND_RESULT *);
 NTSTATUS (*GetDependencies)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,PDEVICE_OBJECT Out[2]);
} WDF_AUDIO_GPR_INTERFACE;
/* One subscription uses the existing named slot and physical lease. All calls
 * are PASSIVE and externally serialized by the client worker. Receive/State run
 * synchronously within B->G serialization and may only copy/enqueue bounded data;
 * they must not reenter any provider. Wake only schedules work (<=DISPATCH).
 * Config and its nonpaged buffers/context remain live through exact successful
 * End, including a failed Begin with a retained lease. Copied leases have no
 * authority. End drains callbacks and copied transport bytes, NOT DSP graphs or
 * DMA. A session failure must keep pumping valid PDR indication ACKs.
 *
 * Send validates the full epoch inside the physical owner lock. Exact success
 * copies the whole packet before returning Accepted; NotAccepted accepts no
 * application bytes. Unknown must retain a command as possibly accepted. Source
 * borrowing always ends before return. Accepted is queued, not DSP completion.
 * Tokens belong to permanent source port3, burn before Send, never reset across
 * lease/provider/PD/ADSP lifetimes and never wrap. Only the currently reserved
 * token may send one matching SPF query; no raw graph packet is admitted by v1.
 *
 * Transport readiness is independent of NamespaceQualified. That field is an
 * observation, not a transferable grant. Each reservation and packet copy must
 * reenter the permanent owner's exact current Subsystem/PIL/origin closure;
 * missing or inherited authority remains blocked regardless of ready snapshots.
 * Graph port0x2010, endpoint admission and old-command retirement are separate
 * future authority contracts; this interface cannot claim playback readiness. */
