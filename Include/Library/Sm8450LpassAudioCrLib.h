/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors. */
#pragma once
#include <Library/SoundWireCrLib.h>

typedef enum {
  CrAudioCodecStopped = 0, CrAudioCodecStarting, CrAudioCodecIdentifying,
  CrAudioCodecIdentifiedMuted, CrAudioCodecStopping, CrAudioCodecFaulted
} CR_AUDIO_CODEC_PHASE;

typedef struct {
  UINT64 DeviceId;
  UINT64 ControllerEpoch, Enumeration;
  UINT8 LogicalNumber, ChipId, VariantRaw, Variant, PaControl;
  BOOLEAN Identified, PaOffVerified;
} CR_AUDIO_CODEC_ID;

/* One board consumer coordinates the shared WCD RX/TX reset and both WSA
 * slaves. It uses real initialized SoundWire cores, not register callbacks
 * standing in for the controller. Cores, request storage and IO code remain
 * stable through exact successful Stop, including partial Start. The caller
 * serializes all APIs with a whole-worker mutex; IRQ only enters SoundWire.
 * IdentifiedMuted is an inventory result, never graph/stream/DMA admission. */
typedef struct {
  VOID *Self;
  CR_SW_CORE *Cores[3]; /* RX, TX/VA, WSA, in exact HDK physical order. */
  CR_AUDIO_CODEC_ID Id[4]; /* WCD RX, WCD TX, WSA left, WSA right. */
  CR_SW_REQUEST Request;
  UINT64 RequestSerial;
  UINT32 Phase, Device, RegisterStep;
  CR_STATUS Error;
  BOOLEAN Pending, Write, Started[3];
} CR_SM8450_AUDIO_CODECS;

CR_STATUS CrSm8450AudioCodecsInit(CR_SM8450_AUDIO_CODECS *, CR_SW_CORE *Rx,
                                  CR_SW_CORE *Tx, CR_SW_CORE *Wsa);
CR_STATUS CrSm8450AudioCodecsStart(CR_SM8450_AUDIO_CODECS *);
CR_STATUS CrSm8450AudioCodecsStep(CR_SM8450_AUDIO_CODECS *);
/* Always attempts every core, even if its sibling needs another stop turn.
 * Success only when each actual IRQ/deferred/resource lease is retired. */
CR_STATUS CrSm8450AudioCodecsStop(CR_SM8450_AUDIO_CODECS *);

#include <Library/AudioPrmCrLib.h>
#define CR_LPASS_WINDOWS 11U
#define CR_LPASS_JOURNAL 64U
typedef enum { CrLpassRxMacro, CrLpassVaMacro, CrLpassWsaMacro, CrLpassLpi,
               CrLpassSlew, CrLpassRxSwr, CrLpassTxSwr, CrLpassWsaSwr,
               CrLpassRxMux, CrLpassVaMux, CrLpassWsaMux } CR_LPASS_WINDOW;
typedef struct { UINT64 Base; UINT32 Span; } CR_LPASS_WINDOW_TARGET;
extern CONST CR_LPASS_WINDOW_TARGET CrSm8450LpassWindows[CR_LPASS_WINDOWS];
typedef struct { VOID *Provider; UINT64 Token; } CR_LPASS_LEASE;
typedef struct {
 VOID *Context;
 /* Acquire the exact exclusive hardware group, real rail/reset owner refs,
  * and validated translated MMIO windows. May return a retained partial lease.
  * Stable lease address is required, including repeated Acquire after pending.
  * Do not publish soundwire leases until Prepare succeeds. */
 CR_STATUS (*Acquire)(VOID *, CONST VOID *Owner, CR_LPASS_LEASE *);
 CR_STATUS (*Read32)(VOID *, CONST CR_LPASS_LEASE *, UINT32 Window, UINT32 Offset, UINT32 *);
 CR_STATUS (*Write32)(VOID *, CONST CR_LPASS_LEASE *, UINT32 Window, UINT32 Offset, UINT32);
 CR_STATUS (*AllDrained)(VOID *, CONST CR_LPASS_LEASE *);
 CR_STATUS (*Release)(VOID *, CR_LPASS_LEASE *);
 /* An OS adapter with a typed permanent-provider power lease uses this exact
  * grant check instead of directly accessing the provider's PRM context. It
  * must validate its real lease, never an observed GetState/READY Boolean. */
 CR_STATUS (*CheckPower)(VOID *, UINT32 Mask);
} CR_LPASS_IO;
typedef struct { UINT32 Window,Offset,Mask,Before; } CR_LPASS_REGISTER;
typedef struct {
 VOID *Self;
 CR_LPASS_IO Io;
 CR_AUDIO_PRM *Power; /* direct portable owner, or NULL with typed CheckPower */
 CR_LPASS_LEASE Lease;
 CR_LPASS_REGISTER Journal[CR_LPASS_JOURNAL];
 UINT32 Count;
 CR_STATUS Error;
 BOOLEAN Ready,Stopping,LeaseUncertain;
} CR_SM8450_LPASS;
/* Serialized passive APIs. MMIO callbacks must independently fence the exact
 * physical hardware lease and admission with each operation. No MMIO is legal
 * without acknowledged actual PRM votes. This layer does not map physical
 * addresses, invent an ACPI allocation, or control global clocks directly. */
CR_STATUS CrSm8450LpassInit(CR_SM8450_LPASS *, CONST CR_LPASS_IO *, CR_AUDIO_PRM *);
CR_STATUS CrSm8450LpassPrepare(CR_SM8450_LPASS *);
CR_STATUS CrSm8450LpassClockHz(CR_SM8450_LPASS *, UINT32 Controller, UINT32 *);
/* Caller first stops ALL actual SoundWire cores; provider AllDrained verifies
 * lease/IRQ/deferred retirement rather than trusting a caller Boolean. */
CR_STATUS CrSm8450LpassStop(CR_SM8450_LPASS *);
