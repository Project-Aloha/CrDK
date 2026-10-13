/* Typed retained APM graph companion on the existing audio subscription.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Interface/WdfAudioPowerPublic.h>
#include <Library/AudioGraphCrLib.h>
#include <Library/AudioPullCrLib.h>
#define WDF_AUDIO_GRAPH_INTERFACE_REVISION 1U
#define CR_AUDIO_GRAPH_PROFILE_LIMIT (16U*1024U*1024U)
DEFINE_GUID(GUID_AUDIO_GRAPH_CR_INTERFACE,0x6dfb0d01,0x3262,0x48d6,0x82,0x33,0x4a,0x8c,0x44,0x1e,0x16,0x39);
typedef struct {PVOID Provider;UINT64 Token;} CR_AUDIO_GRAPH_LEASE;
typedef struct {
 UINT32 Size,Revision;
 CONST VOID *Acdb;UINT32 AcdbBytes,Reserved;
 UINT8 FirmwareMetadataSha256[32]; /* Exact sealed PAS metadata, not filename or whole-MBN hash. */
 CR_AUDIO_CAL_REQUEST Request;
} CR_AUDIO_GRAPH_PROFILE;
typedef struct {
 UINT32 Phase;NTSTATUS Error;UINT64 Session;
 CR_AUDIO_GPR_EPOCH Epoch;
 UINT32 Subgraphs,Modules,EndpointModule,EndpointInstance;
 BOOLEAN Pending,Accepted,GraphPossible,Failed,SourceLost,Ended;
} CR_AUDIO_GRAPH_INFORMATION;
typedef enum {CrAudioGraphCommandPrepare=1,CrAudioGraphCommandStart,CrAudioGraphCommandStop,CrAudioGraphCommandClose} CR_AUDIO_GRAPH_COMMAND;
typedef struct {PVOID Provider;UINT64 Token;} CR_AUDIO_GRAPH_MEMORY_LEASE;
typedef struct {UINT32 Module,Instance,Phase,Reserved;CR_AUDIO_GPR_EPOCH Epoch;} CR_AUDIO_GRAPH_MEMORY_INFORMATION;
typedef enum {CrAudioGraphMemoryBegin=1,CrAudioGraphMemoryCheck,CrAudioGraphMemoryEnd} CR_AUDIO_GRAPH_MEMORY_OPERATION;
typedef enum {CrAudioGraphPullConfigure=1,CrAudioGraphPullQuery,CrAudioGraphPullClose} CR_AUDIO_GRAPH_PULL_OPERATION;
typedef struct {
 UINT64 GraphSession,MemoryToken,WatermarkEvents;CR_AUDIO_GPR_EPOCH Epoch;
 NTSTATUS Error;UINT32 Possible,LastWatermark;
 BOOLEAN Initialized,Ready,Closing,Failed,SourceLost,Ended;
} CR_AUDIO_GRAPH_PULL_INFORMATION;
typedef struct {
 INTERFACE Header;
 NTSTATUS (*Begin)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,CONST CR_AUDIO_HARDWARE_POWER_LEASE *,CONST CR_AUDIO_GRAPH_PROFILE *,CR_AUDIO_GRAPH_LEASE *);
 NTSTATUS (*Request)(PVOID,PVOID Owner,CONST CR_AUDIO_GRAPH_LEASE *,UINT32 Command);
 NTSTATUS (*GetState)(PVOID,PVOID Owner,CONST CR_AUDIO_GRAPH_LEASE *,CR_AUDIO_GRAPH_INFORMATION *);
 NTSTATUS (*End)(PVOID,PVOID Owner,CR_AUDIO_GRAPH_LEASE *);
 NTSTATUS (*Memory)(PVOID,PVOID Owner,CONST CR_AUDIO_GRAPH_LEASE *,UINT32 Operation,
                    CR_AUDIO_GRAPH_MEMORY_LEASE *,CR_AUDIO_GRAPH_MEMORY_INFORMATION *);
 NTSTATUS (*Pull)(PVOID,PVOID Owner,CONST CR_AUDIO_GRAPH_LEASE *,CONST CR_AUDIO_GRAPH_MEMORY_LEASE *,UINT32 Operation,
                  CONST CR_AUDIO_PULL_MEMORY *,CR_AUDIO_GRAPH_PULL_INFORMATION *);
} WDF_AUDIO_GRAPH_INTERFACE;
/* PASSIVE, serialized by the actual audio worker. Query is not an admission
 * grant. Begin requires the same exact named audio lease and current hardware
 * power lease; it copies/compiles immutable ACDB using the explicit qualified
 * profile. The provider compares its metadata fingerprint to the exact live
 * authenticated boot, then fences that full source at every command. The
 * caller binds that profile to actual board/codec before
 * Begin; parser success never establishes those facts. No raw packet/MIID API.
 *
 * Provider owns one permanent port0x2010, nonwrapping tokens, immutable packets
 * and graph journal. AudioGpr.Pump advances bounded asynchronous work. Request
 * success is local scheduling only, never a firmware completion. START also
 * requires the connected Pull-memory/route owner; until that owner is present
 * it fails closed, and no active DMA/endpoint is claimed by this companion.
 *
 * Lease address and Owner must remain unchanged until exact successful End
 * zeroes the lease. Timeout/unknown/local Audio End/source loss preserve the
 * graph. A lost graph may stay in permanent escrow until separately proven
 * old-instance termination; no local StopTicket or generation comparison can
 * free it. Normal End requires targeted CLOSE acknowledgment or never-opened
 * cleanup. End certifies neither remote PCM unmaps nor Windows DMA retirement.
 * Begin/GetState/End failure preserve caller outputs except a returned exact
 * nonzero Begin lease, which remains owned even on a nonfinal result. */

/* Memory is a subordinate exact native ownership pin, not a caller readiness
 * Boolean. Begin requires the compiled single Pull MIID and Configured graph,
 * fresh current authenticated source, and existing exact hardware lease.
 * Check observes source admission only; HAL work occurs outside that fence.
 * A failed post-HAL Check retains all partial handles. End is callable only
 * by the same native owner after exact remote-map and HAL/local retirement;
 * it drops the ownership pin and never certifies those independent effects.
 * Graph End is BUSY while the pin exists. Copying either lease is invalid.
 * Information is required for Begin/Check, forbidden for End. No address,
 * token, module or ready flag is supplied by the caller. */

/* Pull is subordinate to the exact Memory lease. Configure copies immutable
 * native HAL descriptors and reserves permanent tokens; it sends no packet.
 * The native owner must journal possible remote ownership before this call,
 * and keep both local mappings until Query reports exact Ended. Query/Close
 * forbid descriptors; all operations require an output snapshot. Configure
 * cannot replace an initialized session. Close is local scheduling and also
 * requests targeted graph CLOSE; reverse remote UNMAP waits for that actual
 * graph completion. Memory End refuses while initialized Pull is not Ended.
 * No raw packet, arbitrary module, remote handle or caller Ready/Retired flag.
 * Ready certifies only remote Pull configuration, never analog route readiness.
 */
