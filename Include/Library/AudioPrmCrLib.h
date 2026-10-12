/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors. */
#pragma once
#include <Library/AudioGprCrLib.h>

#define CR_AUDIO_PRM_REQUEST 0x0100100fU
#define CR_AUDIO_PRM_RELEASE 0x01001010U
#define CR_AUDIO_PRM_REQUEST_RESPONSE 0x02001002U
#define CR_AUDIO_PRM_RELEASE_RESPONSE 0x02001003U
#define CR_AUDIO_PRM_CLOCK 0x0800102cU
#define CR_AUDIO_PRM_CORE 0x08001032U
#define CR_AUDIO_PRM_MAX_BYTES 76U
#define CR_AUDIO_PRM_VOTES 5U
#define CR_AUDIO_PRM_ALL 0x1fU

typedef enum { CrPrmLpass, CrPrmDcodec, CrPrmTx, CrPrmRx, CrPrmWsa } CR_AUDIO_PRM_RESOURCE;
typedef enum { CrPrmOff, CrPrmRequestPending, CrPrmGranted, CrPrmReleasePending,
               CrPrmRequestUncertain, CrPrmReleaseUncertain } CR_AUDIO_PRM_PHASE;
typedef enum { CrPrmNotAccepted, CrPrmAccepted, CrPrmUnknown } CR_AUDIO_PRM_ACCEPTANCE;
typedef struct { UINT32 Parameter, Id, Frequency; } CR_AUDIO_PRM_TARGET;
extern CONST CR_AUDIO_PRM_TARGET CrAudioPrmSm8450[CR_AUDIO_PRM_VOTES];
/* Exact source identity is captured by the permanent port3 provider. A snapshot
 * never grants copy admission; Reserve/Send must execute the current actual
 * Subsystem/PIL/origin closure again. These serials may never be invented by a
 * client or inferred from just READY, local End, or a changed generation. */
typedef struct {
  CONST VOID *Identity;
  UINT64 Channel, Boot, Image, Binding;
  UINT32 Adsp, AudioPd;
} CR_AUDIO_PRM_SOURCE;
typedef struct {
  CR_STATUS Status;
  UINT32 Acceptance;
} CR_AUDIO_PRM_SEND_RESULT;
typedef struct {
  VOID *Context;
  CR_STATUS (*Current)(VOID *, CR_AUDIO_PRM_SOURCE *);
  CR_STATUS (*Reserve)(VOID *, CONST CR_AUDIO_PRM_SOURCE *, UINT32 Resource,
                       BOOLEAN Release, UINT32 *Token);
  CR_STATUS (*Send)(VOID *, CONST CR_AUDIO_PRM_SOURCE *, CONST VOID *, UINT32,
                    CR_AUDIO_PRM_SEND_RESULT *);
  /* Optional exact old-instance retirement, NOT local callback drainage or
   * remote generation change. Absence means old effects remain retained. */
  CR_STATUS (*Retired)(VOID *, CONST CR_AUDIO_PRM_SOURCE *);
} CR_AUDIO_PRM_IO;
typedef struct {
  UINT32 Phase, Token, RemoteStatus;
  CR_STATUS LastError;
  UINT64 Deadline;
} CR_AUDIO_PRM_VOTE;
typedef struct {
  VOID *Self;
  CR_AUDIO_PRM_IO Io;
  CR_AUDIO_PRM_SOURCE Source;
  CR_AUDIO_PRM_VOTE Votes[CR_AUDIO_PRM_VOTES];
  UINT32 Desired;
  UINT64 LastNow, RetryAt;
  BOOLEAN Bound, Lost, Inside;
} CR_AUDIO_PRM;

/* Pure fixed-policy encoder / validator. Tokens must be permanently burned by
 * the port3 provider. Exact packet sizes: HW core60, clock request76/release64.
 * Failed Build preserves output; input/output ranges must be disjoint. */
CR_STATUS CrAudioPrmBuild(UINT32 Resource, BOOLEAN Release, UINT32 Token,
                          VOID *Buffer, UINTN Capacity, UINT32 *Bytes);
CR_STATUS CrAudioPrmValidate(CONST VOID *, UINTN, UINT32 *Resource, BOOLEAN *Release, UINT32 *Token);
CR_STATUS CrAudioPrmInit(CR_AUDIO_PRM *, CONST CR_AUDIO_PRM_IO *);
CR_STATUS CrAudioPrmSetDesired(CR_AUDIO_PRM *, UINT32 Mask);
/* One bounded transaction per turn. Callbacks are passive and must not reenter
 * this owner; Receive is invoked later from a bounded copied mailbox. Caller
 * serializes ALL APIs with a native whole-worker mutex. Stable native/permanent
 * storage is retained while any vote may exist, even when transport is ended. */
CR_STATUS CrAudioPrmStep(CR_AUDIO_PRM *, UINT64 NowUs);
CR_STATUS CrAudioPrmReceive(CR_AUDIO_PRM *, CONST CR_AUDIO_PRM_SOURCE *, CONST VOID *, UINTN);
CR_STATUS CrAudioPrmCheck(CR_AUDIO_PRM *, UINT32 Mask);
/* Close future sends immediately, without pretending remote votes vanished.
 * Physical transport can drain/End while this small permanent journal remains. */
CR_STATUS CrAudioPrmSourceLost(CR_AUDIO_PRM *);
CR_STATUS CrAudioPrmRetire(CR_AUDIO_PRM *);
BOOLEAN CrAudioPrmEmpty(CONST CR_AUDIO_PRM *);
