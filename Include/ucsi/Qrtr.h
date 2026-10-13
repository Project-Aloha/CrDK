/** Polled ADSP QRTR/PDR services. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_QRTR_MAX_PACKET 1024U
#define CR_QRTR_QUEUE_SIZE 16U
typedef CR_STATUS (*CR_QRTR_SEND)(VOID *Context, CONST VOID *Data, UINTN Length);
typedef struct {
  UINT16 Length;
  UINT8 Data[CR_QRTR_MAX_PACKET];
} CR_QRTR_PACKET;
/* Optional audio-PD listener on the existing router, not a second edge owner.
 * Fields are private after Enable. Counters never reset across Online changes;
 * exhaustion/fault closes audio only and cannot be cleared by re-enabling.
 * Valid PDR ACKs continue while a usable transaction namespace remains; an
 * AudioCr/session fault is NOT a router fault and must not stop pumping. */
typedef struct {
  BOOLEAN Enabled, ServerFound, Registered, Pending, Known, Ready, Faulted;
  BOOLEAN Indicated, TimeKnown;
  UINT32 ServerNode, ServerPort, Generation, RawState;
  UINT64 ServerGeneration, NextTransaction, DeadlineUs, RetryUs, NowUs;
  UINT16 PendingTransaction;
  CR_STATUS LastError;
} CR_QRTR_AUDIO_PD;
typedef struct {
  BOOLEAN Enabled, ServerFound, Registered, Known, Ready, Faulted;
  UINT32 ServerNode, ServerPort, Generation, RawState;
  UINT64 ServerGeneration;
  CR_STATUS LastError;
} CR_QRTR_AUDIO_PD_INFORMATION;
typedef struct {
  CR_QRTR_SEND Send;
  VOID *Context;
  BOOLEAN Online, HelloSent, Advertised, ServerFound, Registered, Pending;
  BOOLEAN PdKnown, PdReady;
  UINT32 ServerNode, ServerPort, PdGeneration;
  UINT16 Transaction, PendingTransaction;
  UINT64 DeadlineUs, RetryUs;
  UINT32 Received, Malformed, DomainRequests;
  CR_STATUS LastError;
  CR_QRTR_PACKET Queue[CR_QRTR_QUEUE_SIZE];
  UINTN QueueHead, QueueCount;
  CR_QRTR_AUDIO_PD Audio;
} CR_QRTR;
/* Initialize fresh owner storage only. Once audio is enabled, use Online to
 * change transport incarnations; reinitializing loses its non-reused history.
 * All APIs require external serialization. Send does not reenter this router. */
VOID CrQrtrInitialize(CR_QRTR *Router, CR_QRTR_SEND Send, VOID *Context);
VOID CrQrtrOnline(CR_QRTR *Router, BOOLEAN Online);
CR_STATUS CrQrtrReceive(CR_QRTR *Router, CONST UINT8 *Data, UINTN Length);
CR_STATUS CrQrtrTick(CR_QRTR *Router, UINT64 NowUs);
/* Enable is idempotent and persists through Online changes. Known explicit UP
 * plus a qualified listener is required; there is no RPMSG-ready fallback.
 * Success from Get means a snapshot, not readiness or graph/DMA retirement.
 * Provider must inspect it within its RX callback before subsequent audio TX. */
CR_STATUS CrQrtrEnableAudioPd(CR_QRTR *Router);
CR_STATUS CrQrtrGetAudioPd(CONST CR_QRTR *Router, CR_QRTR_AUDIO_PD_INFORMATION *Info);
