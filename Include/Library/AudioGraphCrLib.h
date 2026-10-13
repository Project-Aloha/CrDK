/** Retained, in-band AudioReach command owner; hardware authority is external.
 * Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <Library/AudioCalCrLib.h>
#include <Library/AudioGprCrLib.h>

#define CR_AUDIO_GRAPH_RX_BYTES 512U
#define CR_AUDIO_GRAPH_MAILBOX 8U
#define CR_AUDIO_GRAPH_CONTROL_BYTES (56U + ((4U*(1U+CR_AUDIO_CAL_MAX_SUBGRAPHS)+7U)&~7U))

typedef struct {
  UINT64 Channel;
  UINT32 Adsp, AudioPd;
} CR_AUDIO_GRAPH_EPOCH;

typedef enum {
  CrAudioGraphSpf = 1, CrAudioGraphOpen, CrAudioGraphCalibration,
  CrAudioGraphEndpoint, CrAudioGraphConfigured, CrAudioGraphClose,
  CrAudioGraphReleasing, CrAudioGraphRecovery, CrAudioGraphEnded,
  CrAudioGraphPreparing, CrAudioGraphPrepared, CrAudioGraphStarting,
  CrAudioGraphRunning, CrAudioGraphStopping, CrAudioGraphStopped
} CR_AUDIO_GRAPH_PHASE;
typedef enum {
  CrAudioGraphNotAccepted = 1, CrAudioGraphAccepted, CrAudioGraphUnknown
} CR_AUDIO_GRAPH_ACCEPTANCE;
typedef struct {
  CR_STATUS Status;
  UINT32 Acceptance;
} CR_AUDIO_GRAPH_SEND_RESULT;

/* Permanent source-port owner, private after Init. Never zero/copy/reinitialize
 * it, even after all graph sessions end. HostPort must be globally reserved by
 * the OS/provider; local Init does not allocate a firmware port. Session/Token
 * counters never wrap or restart. Storage/code survives old replies and PnP/D3.
 * No unqualified destroy/reset operation exists. */
typedef struct {
  UINTN Self;
  CONST VOID *Owner;
  UINT64 NextToken, NextSession;
  VOID *PullOwner; /* Exact subordinate journal, retained beyond graph CLOSE. */
  UINT32 HostPort;
  CONST VOID *Session;
} CR_AUDIO_GRAPH_PORT;

typedef struct {
  CONST VOID *Owner;
  UINT64 Session;
  UINT32 HostPort;
  CR_AUDIO_GRAPH_EPOCH Epoch;
  CONST CR_AUDIO_CAL_PLAN *Plan;
} CR_AUDIO_GRAPH_CLAIM;

typedef struct {
  VOID *Context;
  CR_AUDIO_GRAPH_SEND_RESULT (*Send)(VOID *, CONST CR_AUDIO_GRAPH_CLAIM *,
                                     CONST VOID *, UINT32);
  /* Authority reserves this exact namespace, with a qualified empty baseline,
   * matching profile/modules and muted physical endpoint prerequisites. It
   * writes a nonzero retained Cookie on success OR any partially held failure.
   * This is a provider contract, not a caller readiness boolean. */
  CR_STATUS (*Admit)(VOID *, CONST CR_AUDIO_GRAPH_CLAIM *, UINT64 *Cookie);
  /* Query the exact retained claim/cookie. Exact SUCCESS attests retirement of
   * THIS old graph generation by the owning PD/subsystem authority. Channel
   * End, down/up notification, counter increase and a timeout are insufficient.
   * It must also fence/discard all queued/in-flight old commands before any
   * new firmware could consume them. This is graph-only: it never certifies
   * DMA/SMMU/buffer retirement. */
  CR_STATUS (*Retired)(VOID *, CONST CR_AUDIO_GRAPH_CLAIM *, UINT64 Cookie);
  /* Release the namespace only after no graph exists. Non-success retains it. */
  CR_STATUS (*Release)(VOID *, CONST CR_AUDIO_GRAPH_CLAIM *, UINT64 Cookie);
} CR_AUDIO_GRAPH_BINDING;

typedef struct {
  CR_AUDIO_CAL_PLAN Plan;
  UINT8 Close[CR_AUDIO_CAL_MAX_PACKET_BYTES];
  UINT8 Control[CR_AUDIO_GRAPH_CONTROL_BYTES];
  CR_AUDIO_CAL_PLAYBACK_ENDPOINTS Playback;
} CR_AUDIO_GRAPH_STORAGE;

typedef struct {
  CR_AUDIO_GRAPH_EPOCH Epoch;
  UINT32 Bytes;
  BOOLEAN State, Online;
  UINT8 Data[CR_AUDIO_GRAPH_RX_BYTES];
} CR_AUDIO_GRAPH_EVENT;

typedef struct {
  UINTN Self;
  CR_AUDIO_GRAPH_PORT *Port;
  CR_AUDIO_GRAPH_STORAGE *Storage;
  CR_AUDIO_GRAPH_CLAIM Claim;
  CR_AUDIO_GRAPH_BINDING Binding;
  UINT64 Cookie, Now, Deadline, Timeout;
  UINT32 Phase, PendingPhase, TokenBase, Acceptance, CloseBytes;
  UINT32 ControlToken, ControlBytes;
  UINT32 DspStatus, DspOpcode, CleanupDspStatus, CleanupDspOpcode;
  UINT32 Replies, Ignored, Malformed;
  CR_STATUS Error, CleanupError;
  BOOLEAN Online, Active, Pending, Failed, Lost, Stop, Overflow;
  BOOLEAN GraphPossible, OpenKnown, Admitted, Attempted, TimeKnown;
  UINT32 Head, Count;
  CR_AUDIO_GRAPH_EVENT Events[CR_AUDIO_GRAPH_MAILBOX];
  UINT8 Spf[CR_AUDIO_GPR_HEADER_BYTES];
} CR_AUDIO_GRAPH_SESSION;

typedef struct {
  UINT32 Phase, PendingPhase, Token, Acceptance;
  UINT32 DspStatus, DspOpcode, CleanupDspStatus, CleanupDspOpcode;
  UINT32 Replies, Ignored, Malformed;
  CR_STATUS Error, CleanupError;
  UINT64 Cookie, Session;
  CR_AUDIO_GRAPH_EPOCH Epoch;
  BOOLEAN Pending, Failed, Lost, GraphPossible, NamespaceHeld;
} CR_AUDIO_GRAPH_STATUS;

/* All APIs require externally serialized stable storage and immutable inputs.
 * Port/Session require all-zero virgin storage. Typed objects are naturally
 * aligned; all declared input/output regions are disjoint. Caller owns actual
 * address validity. Init compiles into disposable Storage/Workspace: these may
 * change on failure, but Session/Port and source ACDB/request stay unchanged.
 * Successful Init burns five tokens, binds one session to Port, and owns plan
 * bytes until Ended. Init itself performs no transport/authority calls.
 * Every callback and provider/context stays alive through transport End and
 * callback/work rundown, even if the graph reaches Ended. */
CR_STATUS CrAudioGraphPortInit(CR_AUDIO_GRAPH_PORT *, CONST VOID *Owner, UINT32 HostPort);
CR_STATUS CrAudioGraphInit(CR_AUDIO_GRAPH_SESSION *, CR_AUDIO_GRAPH_PORT *,
                          CONST VOID *Owner, CONST CR_AUDIO_GRAPH_BINDING *,
                          CONST CR_AUDIO_GRAPH_EPOCH *, UINT64 TimeoutUs,
                          CONST VOID *Acdb, UINTN Bytes, CONST CR_AUDIO_CAL_REQUEST *,
                          VOID *Workspace, UINTN WorkspaceBytes, CR_AUDIO_GRAPH_STORAGE *);
/* Typed headphone Pull graph variant. The exact merged profile/tag/data path
 * is checked before session publication or permanent token consumption.
 * Storage.Playback is populated only by this variant; ordinary Init zeros it. */
CR_STATUS CrAudioGraphInitPull(CR_AUDIO_GRAPH_SESSION *, CR_AUDIO_GRAPH_PORT *,
                          CONST VOID *Owner, CONST CR_AUDIO_GRAPH_BINDING *,
                          CONST CR_AUDIO_GRAPH_EPOCH *, UINT64 TimeoutUs,
                          CONST VOID *Acdb, UINTN Bytes, CONST CR_AUDIO_CAL_REQUEST *,
                          VOID *Workspace, UINTN WorkspaceBytes, CR_AUDIO_GRAPH_STORAGE *);
/* Receive/State only copy to the bounded mailbox. They may run synchronously
 * during Send. They never call a provider or execute state transitions. Their
 * caller serializes mailbox access with Tick (short OS adapter lock; do not
 * hold it over provider calls). Overflow closes admission on the next Tick.
 * No input pointer is retained. Exact SUCCESS means owned copy, not APM success. */
CR_STATUS CrAudioGraphReceive(CR_AUDIO_GRAPH_SESSION *, CONST CR_AUDIO_GRAPH_EPOCH *,
                             CONST VOID *, UINT32);
CR_STATUS CrAudioGraphState(CR_AUDIO_GRAPH_SESSION *, CONST CR_AUDIO_GRAPH_EPOCH *, BOOLEAN Online);
/* One bounded step, at most one Send; deadlines apply at worker dispatch
 * (a queued reply processed after its deadline cannot turn timeout into success); response dispatch occurs only after the
 * send outcome has been recorded. Timers advance TX/deadlines, not GLINK RX.
 * Ended retires the graph/namespace only, not its named-service subscription. */
CR_STATUS CrAudioGraphTick(CR_AUDIO_GRAPH_SESSION *, UINT64 NowUs);
CR_STATUS CrAudioGraphRequestClose(CR_AUDIO_GRAPH_SESSION *);
/* Target only this graph's compiled SG list. Requests schedule one command;
 * exact DSP completion, not this return value, advances the observable phase.
 * PREPARE: Configured -> Prepared; START: Prepared/Stopped -> Running;
 * STOP: Running -> Stopped. A repeated achieved-state request is idempotent.
 * Every new command burns a fresh token in the permanent Port. The binding
 * must independently qualify the real muted route, media and retained DMA
 * maps before allowing START. This core never grants buffer access/retirement.
 * APM FLUSH is intentionally separate: its module-specific pause prerequisite
 * is not established by STOP and is not synthesized here. */
CR_STATUS CrAudioGraphRequestPrepare(CR_AUDIO_GRAPH_SESSION *);
CR_STATUS CrAudioGraphRequestStart(CR_AUDIO_GRAPH_SESSION *);
CR_STATUS CrAudioGraphRequestStop(CR_AUDIO_GRAPH_SESSION *);
CR_STATUS CrAudioGraphGetStatus(CONST CR_AUDIO_GRAPH_SESSION *, CR_AUDIO_GRAPH_STATUS *);
