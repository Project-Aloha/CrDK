/* Named service on the permanent PEP's shared ADSP edge. GPL-2.0-only */
#pragma once
#include <wdm.h>
#include <ucsi/Glink.h>

#define WDF_GLINK_SERVICE_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_GLINK_SERVICE_CR_INTERFACE,0x634118f5,0xb997,0x4eb4,0x97,0x64,0x82,0x04,0xac,0x71,0xc5,0xd8);
typedef struct { PVOID Provider; UINT64 Token; } CR_GLINK_SERVICE_LEASE;
typedef VOID (*CR_GLINK_SERVICE_WAKE)(PVOID);
typedef struct {
 UINT64 ChannelGeneration;
 UINT32 AdspGeneration;
 BOOLEAN Online,Faulted,More,Quarantined;
 NTSTATUS LastError;
} CR_GLINK_SERVICE_INFORMATION;
typedef struct {
 INTERFACE Header;
 NTSTATUS (*Begin)(PVOID,PVOID Owner,CONST CR_GLINK_SERVICE_CONFIG *,
                   CR_GLINK_SERVICE_WAKE,PVOID WakeContext,CR_GLINK_SERVICE_LEASE *);
 NTSTATUS (*End)(PVOID,PVOID Owner,CR_GLINK_SERVICE_LEASE *);
 NTSTATUS (*Pump)(PVOID,PVOID Owner,CONST CR_GLINK_SERVICE_LEASE *,CR_GLINK_SERVICE_INFORMATION *);
 NTSTATUS (*Send)(PVOID,PVOID Owner,CONST CR_GLINK_SERVICE_LEASE *,UINT64 ChannelGeneration,
                  CONST VOID *,UINT32);
 NTSTATUS (*GetState)(PVOID,PVOID Owner,CONST CR_GLINK_SERVICE_LEASE *,CR_GLINK_SERVICE_INFORMATION *);
 NTSTATUS (*Reopen)(PVOID,PVOID Owner,CONST CR_GLINK_SERVICE_LEASE *);
 NTSTATUS (*GetDependencies)(PVOID,PVOID Owner,CONST CR_GLINK_SERVICE_LEASE *,PDEVICE_OBJECT Out[2]);
} WDF_GLINK_SERVICE_INTERFACE;

/* Query the permanent BUS provider. This version has one named subscription,
 * independent of the two existing UCSI/battery policies; all share one physical
 * SMEM/IPCC/PIL lease. Methods are PASSIVE. Wake is <=DISPATCH and only schedules
 * work. Receive/State callbacks run serialized at PASSIVE, must not call ANY
 * provider interface, allocate, block or retain Data; copy into a bounded client
 * mailbox before exact CR_SUCCESS. Any other receive result faults this channel.
 * Buffers/context are stable nonpaged caller storage, disjoint from provider,
 * leases and SMEM. Config is copied. Repeated Begin must match every field.
 *
 * A failed Begin can retain Lease.Provider; retry exact End until success. End
 * closes data admission, waits both wire close directions and drains Wake before
 * releasing caller storage. NOT_READY/BUSY retain it; Pump and End provide bounded
 * progress. A timeout never establishes retirement. Permanent physical fault or
 * quarantine can discard CPU-copy state after admission closes; hardware bindings
 * remain subject to the separate retained owner. End is NOT APM graph or DMA
 * retirement; the audio executor must retain its own PIL/mapping/recovery objects.
 *
 * State reports the named channel and authenticated ADSP generations, not
 * charger_pd/audio_pd/SPF/codec readiness. Reopen follows complete remote close
 * or channel-local fault. The consumer owns unique command tokens/PD epochs and
 * must discard old replies after generation changes. Send copies an accepted
 * packet; success means queued, not delivered or acknowledged. Pump services RX
 * only after IPCC work; timers may advance TX and protocol deadlines. Dependencies
 * returns independently referenced ADSP Subsystem (ADS0)/SMEM PDOs for PowerRelations; the provider
 * PDO remains a separate dependency. Referenced interfaces and exact retained
 * leases prevent provider destruction. Copied leases have no authority. */
