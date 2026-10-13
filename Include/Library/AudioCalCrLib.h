/** Bounded, offline AudioReach initial codec-DMA plan compiler.
    Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <oskal/cr_types.h>

#define CR_AUDIO_CAL_MAX_KEYS 32U
#define CR_AUDIO_CAL_MAX_SUBGRAPHS 64U
#define CR_AUDIO_CAL_MAX_MODULES 512U
#define CR_AUDIO_CAL_MAX_EDGES 512U
#define CR_AUDIO_CAL_MAX_PARAMETERS 512U
#define CR_AUDIO_CAL_MAX_PACKET_BYTES 65536U
#define CR_AUDIO_CAL_MAX_DRIVER_BYTES 8192U
#define CR_AUDIO_CAL_ENDPOINT_PACKET_BYTES 104U

typedef enum {
  CrAudioCalOk = 0,
  CrAudioCalInvalidArgument,
  CrAudioCalMalformed,
  CrAudioCalNotFound,
  CrAudioCalLimit,
  CrAudioCalWorkspaceSmall,
  CrAudioCalUnsupportedContainer,
  CrAudioCalUnsupportedPersistence,
  CrAudioCalUnsupportedOffload,
  CrAudioCalUnsupportedDomain,
  CrAudioCalUnsupportedAmdb,
  CrAudioCalUnsupportedDriver,
  CrAudioCalUnsupportedEndpoint,
  CrAudioCalUnsupportedGraphProperty,
  CrAudioCalMissingCalibrationKey,
  CrAudioCalMissingCalibration,
  CrAudioCalMissingModuleMap,
  CrAudioCalUnsupportedExternalGraph
} CR_AUDIO_CAL_RESULT;

typedef struct {
  UINT32 Key;
  UINT32 Value;
} CR_AUDIO_CAL_KEY;
typedef struct {
  UINT32 Source;
  UINT32 Destination;
} CR_AUDIO_CAL_EDGE;
typedef struct {
  UINT32 Id;
  UINT32 RoutingDomain;
  UINT32 Flushable;
  UINT32 Type;
} CR_AUDIO_CAL_SUBGRAPH;
typedef struct {
  UINT32 Subgraph;
  UINT32 Module;
  UINT32 Instance;
  UINT32 Container;
} CR_AUDIO_CAL_MODULE;

typedef struct {
  UINT32 GraphKeyCount;
  UINT32 CalibrationKeyCount;
  CR_AUDIO_CAL_KEY GraphKeys[CR_AUDIO_CAL_MAX_KEYS];
  CR_AUDIO_CAL_KEY CalibrationKeys[CR_AUDIO_CAL_MAX_KEYS];
  UINT32 EndpointTag; /* Profile-defined tag, not an endpoint instance. */
  UINT32 SampleRate;
  UINT32 BitWidth;
  UINT32 Channels;
  UINT32 LpaifType; /* Explicit verified backend profile values. */
  UINT32 InterfaceIndex;
  UINT32 ActiveChannelsMask;
  UINT32 HostPort;
  UINT32 OpenToken;
  UINT32 CalibrationToken;
  UINT32 EndpointToken;
} CR_AUDIO_CAL_REQUEST;

typedef struct {
  UINT32 SubgraphCount;
  UINT32 EdgeCount;
  UINT32 ModuleCount;
  UINT32 ParameterCount;
  UINT32 EndpointModule;
  UINT32 EndpointInstance;
  UINT32 DriverBytes;
  UINT32 OpenBytes;
  UINT32 OpenPropertyBytes; /* GPR+APM headers excluded; connections follow. */
  UINT32 CalibrationBytes;
  UINT32 EndpointBytes;
  CR_AUDIO_CAL_REQUEST Request;
  CR_AUDIO_CAL_SUBGRAPH Subgraphs[CR_AUDIO_CAL_MAX_SUBGRAPHS];
  CR_AUDIO_CAL_EDGE Edges[CR_AUDIO_CAL_MAX_EDGES];
  CR_AUDIO_CAL_MODULE Modules[CR_AUDIO_CAL_MAX_MODULES];
  UINT8 Driver[CR_AUDIO_CAL_MAX_DRIVER_BYTES];
  UINT8 Open[CR_AUDIO_CAL_MAX_PACKET_BYTES];
  UINT8 Calibration[CR_AUDIO_CAL_MAX_PACKET_BYTES];
  UINT8 Endpoint[CR_AUDIO_CAL_ENDPOINT_PACKET_BYTES];
} CR_AUDIO_CAL_PLAN;

/* Offline, typed playback endpoints selected by the same exact merged GKV.
   These identities are not firmware availability or DMA/route authority. */
typedef struct {
  UINT32 MemoryModule;
  UINT32 MemoryInstance;
  UINT32 CodecModule;
  UINT32 CodecInstance;
} CR_AUDIO_CAL_PLAYBACK_ENDPOINTS;

/* These are policy limits, not ACDB format limits. No allocation, IO, locking,
   clock/power/transport access or retained pointer. Caller serializes access.
   Data[Bytes] is immutable and alive throughout the call, may be unaligned,
   and is limited to UINT32_MAX. Request/Plan are DWORD-aligned; Workspace is UINTN-aligned;
   every declared region must be disjoint, including the whole input and
   whole WorkspaceBytes. Workspace is disposable scratch and may change on
   failure. All other inputs and Plan are unchanged on EVERY non-Ok result.
   On success Plan owns all bytes; Data/Workspace may immediately be released.

   A successful plan establishes offline configuration only. It does not
   establish firmware/module compatibility, matching board tuning, resource
   readiness, command success, stream/DMA ownership or epoch/token freshness.
   Caller owns unique command tokens, service/audio-PD/ADSP epochs, mute and
   graph/DMA retirement. Runtime interface values must come from the matching
   board profile. Missing calibration inputs are never substituted by another
   board's defaults. This initial API accepts only explicit nonempty exact
   GKV and in-band ADSP codec-DMA PCM profiles. No live CKV delta, aliases,
   AMDB loading, persistent/offloaded data or hardware apply is supported. */
UINTN CrAudioCalWorkspaceBytes(VOID);
CR_AUDIO_CAL_RESULT CrAudioCalBuildPlan(CONST VOID *Data, UINTN Bytes,
                                        CONST CR_AUDIO_CAL_REQUEST *Request, VOID *Workspace,
                                        UINTN WorkspaceBytes, CR_AUDIO_CAL_PLAN *Plan);
/* Reject non-PCM/non-headphone routes and all duplicate GKV keys. Additional
   exact profile keys are retained; this does not invent board-specific values. */
CR_AUDIO_CAL_RESULT CrAudioCalValidateHeadphoneRequest(CONST CR_AUDIO_CAL_REQUEST *Request);
/* Same disjoint-region/unchanged-output contract as BuildPlan, extended to
   Endpoints. Requires hardware RX tag c0000004, SHMEM tag c0000001 naming one
   owned Pull module 07001006, and an actual directed, wholly owned data-link
   path to the selected codec-DMA RX 07001023. Control links do not count.
   External data links are unsupported by this closed playback owner. */
CR_AUDIO_CAL_RESULT CrAudioCalBuildPullPlan(CONST VOID *Data, UINTN Bytes,
    CONST CR_AUDIO_CAL_REQUEST *Request, VOID *Workspace, UINTN WorkspaceBytes,
    CR_AUDIO_CAL_PLAN *Plan, CR_AUDIO_CAL_PLAYBACK_ENDPOINTS *Endpoints);
