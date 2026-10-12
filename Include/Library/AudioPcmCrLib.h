/** AudioReach shared-memory PCM packet helpers.
    Copyright (c) 2026 Project Aloha Authors.
    SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <Library/AudioGprCrLib.h>

#define CR_AUDIO_PCM_MAP_PACKET_BYTES 44U
#define CR_AUDIO_PCM_UNMAP_PACKET_BYTES 28U
#define CR_AUDIO_PCM_WRITE_PACKET_BYTES 68U
#define CR_AUDIO_PCM_READ_PACKET_BYTES 56U
#define CR_AUDIO_PCM_MAP 0x0100100cU
#define CR_AUDIO_PCM_MAP_RESPONSE 0x02001001U
#define CR_AUDIO_PCM_UNMAP 0x0100100dU
#define CR_AUDIO_PCM_WRITE_V2 0x0400100aU
#define CR_AUDIO_PCM_WRITE_DONE_V2 0x05001004U
#define CR_AUDIO_PCM_READ_V2 0x0400100bU
#define CR_AUDIO_PCM_READ_DONE_V2 0x05001005U
#define CR_AUDIO_PCM_TIMESTAMP_VALID 0x80000000U
#define CR_AUDIO_PCM_LAST_BUFFER 0x40000000U
#define CR_AUDIO_PCM_TIMESTAMP_CONTINUE 0x20000000U
#define CR_AUDIO_PCM_END_OF_FRAME 0x00000010U

typedef enum {
  CrAudioPcmMap = 1,
  CrAudioPcmUnmap = 2,
  CrAudioPcmWrite = 3,
  CrAudioPcmRead = 4
} CR_AUDIO_PCM_OPERATION;

typedef struct {
  UINT32 Operation;
  UINT32 HostPort;
  UINT32 EndpointPort; /* APM1 for MAP/UNMAP, exact module instance for data. */
  UINT32 Token;
  UINT64 Address;      /* DSP-visible mapped address, NOT an HLOS VA or FD. */
  UINT32 MapHandle;
  UINT32 Bytes;
  UINT64 Timestamp;    /* Raw two's-complement microsecond timestamp bits. */
  UINT32 Flags;        /* MAP: 0=cached, 2=uncached; WRITE: documented bits. */
  UINT32 Reserved;
} CR_AUDIO_PCM_REQUEST;

typedef enum {
  CrAudioPcmMapHandle = 1,
  CrAudioPcmUnmapped = 2,
  CrAudioPcmWriteDone = 3,
  CrAudioPcmReadDone = 4,
  CrAudioPcmBasicAck = 5,
  CrAudioPcmBasicError = 6
} CR_AUDIO_PCM_REPLY_KIND;

typedef struct {
  UINT32 Kind;
  UINT32 DspStatus;
  UINT32 MetadataStatus;
  UINT32 MapHandle;
  UINT32 DataBytes; /* READ's reported byte count; zero for WRITE (not reported). */
  UINT32 FrameCount;
  UINT64 Timestamp;
  UINT32 Flags;
  UINT32 Reserved;
} CR_AUDIO_PCM_REPLY;

CR_STATIC_ASSERT(sizeof(CR_AUDIO_PCM_REQUEST) == 48, "PCM request layout");
CR_STATIC_ASSERT(sizeof(CR_AUDIO_PCM_REPLY) == 40, "PCM reply layout");

/* Pure wire operations; no allocation, I/O, token ownership or readiness grant.
   This bounded initial subset maps ONE physical-address region, pool3, cached
   or uncached. No offset, loaned, virtual, FD, global, satellite, scatter/gather
   or metadata modes. Data commands use V2 and zero metadata fields. Firmware
   V2 support must be independently established before dispatch; SPF Ready is
   insufficient. MapHandle0 is rejected as in the pinned GSL runtime.

   Caller owns immutable Request (8-byte aligned) and valid storage. Buffer may
   be unaligned. No pointer range may wrap or overlap another input/output.
   Build writes exactly the packet length above, preserving remaining capacity.
   Every failure preserves Buffer/Reply. Decoder Reply must be 8-byte aligned.
   Invalid parameters are rejected before output is written. Address+Bytes must
   fit a 36-bit DSP-visible aperture; MAP address/size are nonzero 4K aligned;
   data address is nonzero 64-byte aligned, Bytes is nonzero. Token is any wire
   value, including0; a permanent owner outside this helper must allocate it.

   These functions do NOT map memory, perform cache maintenance, establish a
   graph/media format, verify containment in an actual retained DMA mapping,
   serialize buffers or certify a real current epoch. The live caller must do
   all of those, journal before Send, and retain data/mapping/graph/physical PIL
   leases after accepted/unknown sends until qualified retirement. Timeout,
   generation change, basic ACK/error and local transport End never imply DMA
   retirement. No function here releases memory or resets an ownership journal. */
CR_STATUS CrAudioPcmBuild(CONST CR_AUDIO_PCM_REQUEST *Request,
                          VOID *Buffer, UINTN Capacity);

/* Require ADSP2->APPS3, exact ports/token, exact response opcode/payload and,
   for data completions, the full outstanding address/map-handle tuple. Header
   extensions remain supported by the existing GPR decoder. Unrelated routes,
   token, opcode or echoed data tuple return NOT_FOUND; malformed matching
   payload/reserved metadata/range returns COMPROMISED_DATA. READ DataBytes may
   not exceed the outstanding capacity. No implicit truncation.

   CR_SUCCESS means decoded, not DSP success: inspect Kind AND status fields.
   MAP basic success is BasicAck without a handle. WRITE/READ basic success or
   error is never a data completion. UNMAP basic success is Unmapped, which
   describes the remote map only; it does not retire outstanding data or prove
   the local SMMU/cache/DMA provider has released anything. A genuine matching
   DONE returns the buffer according to the V2 wire contract even when its data
   status is nonzero; caller still checks same-epoch provenance and performs
   appropriate cache synchronization before CPU use. */
CR_STATUS CrAudioPcmMatchReply(CONST CR_AUDIO_PCM_REQUEST *Request,
                               CONST VOID *Packet, UINTN Bytes,
                               CR_AUDIO_PCM_REPLY *Reply);
