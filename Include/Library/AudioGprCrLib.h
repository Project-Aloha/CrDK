/** Stateless AudioReach GPR wire and SPF readiness helpers.
    Copyright (c) 2026 Project Aloha Authors.
    SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_AUDIO_GPR_HEADER_BYTES 24U
#define CR_AUDIO_GPR_MAX_PACKET_BYTES 0x00ffffffU
#define CR_AUDIO_GPR_APM_PORT 1U
#define CR_AUDIO_GPR_APPS_DOMAIN 3U
#define CR_AUDIO_GPR_ADSP_DOMAIN 2U
#define CR_AUDIO_APM_GET_SPF_STATE 0x01001021U
#define CR_AUDIO_APM_SPF_STATE_RESPONSE 0x02001007U
#define CR_AUDIO_GPR_BASIC_RESPONSE 0x02001005U

typedef struct {
  UINT32 HeaderBytes;
  UINT32 PacketBytes;
  UINT8 DestinationDomain;
  UINT8 SourceDomain;
  UINT8 ClientData; /* Low four opaque bits; high four reserved bits are zero. */
  UINT8 Reserved;   /* Always zero. */
  UINT32 SourcePort;
  UINT32 DestinationPort;
  UINT32 Token;
  UINT32 Opcode;
} CR_AUDIO_GPR_HEADER;

typedef enum {
  CrAudioSpfNotReady = 1,
  CrAudioSpfReady = 2,
  CrAudioSpfDspError = 3,
  CrAudioSpfBasicAckWithoutState = 4
} CR_AUDIO_SPF_REPLY_KIND;

typedef struct {
  UINT32 Kind;   /* CR_AUDIO_SPF_REPLY_KIND */
  UINT32 Value;  /* Exact remote state (0/1) or basic-response status. */
} CR_AUDIO_SPF_REPLY;

CR_STATIC_ASSERT(sizeof(CR_AUDIO_GPR_HEADER) == 28, "GPR decoded header layout");
CR_STATIC_ASSERT(sizeof(CR_AUDIO_SPF_REPLY) == 8, "SPF reply layout");

/* These are pure byte helpers, not a transport or transaction owner. The
   caller supplies valid readable/writable storage; input bytes must remain
   immutable through each call. Input and encoded output may be unaligned.
   Typed outputs require DWORD alignment and must not overlap any input byte.
   Every failure leaves output unchanged. No retained pointers or allocation.

   Decode accepts wire version0, header lengths6..15 DWORDs, exact packet
   length, zero reserved byte and zero high client-data nibble. Header extension
   bytes are opaque; payload begins at HeaderBytes. Domains/ports/opcode are
   decoded without assigning authority to them. PacketBytes <= 0xffffff.
   INVALID_PARAMETER is a pointer/range/alignment/alias error; BAD_BUFFER_SIZE
   is an oversized packet; COMPROMISED_DATA is malformed framing/reserved data;
   UNSUPPORTED is an unknown version. */
CR_STATUS CrAudioGprDecode(CONST VOID *Data, UINTN Bytes,
                           CR_AUDIO_GPR_HEADER *Header);

/* Write exactly24 bytes, preserving all capacity after them. HostPort must be
   neither0 (uninitialized) nor UINT32_MAX (undefined). Token is any caller-owned
   wire token, including0; this function neither allocates nor fences tokens.
   BUFFER_TOO_SMALL does not modify Buffer. Remaining fields and reserved bytes
   are initialized explicitly, with source APPS3 and destination ADSP2/APM1. */
CR_STATUS CrAudioGprBuildSpfQuery(UINT32 HostPort, UINT32 Token,
                                 VOID *Buffer, UINTN Capacity);

/* Decode anew, then require source ADSP2/APM1, destination APPS3/HostPort and
   exact Token. Accept only4-byte state response0/1 or8-byte basic result whose
   original opcode is GET_SPF_STATE. Unrelated well-formed routing, token or
   opcode returns NOT_FOUND; malformed matching payload returns COMPROMISED_DATA.
   A successful basic result yields BasicAckWithoutState, never Ready.

   CR_SUCCESS means a matching reply was decoded, NOT that SPF is ready or its
   command succeeded: inspect Kind/Value. The caller must retain channel/PIL/PD
   generation and prevent stale-token reuse; these bytes alone cannot prove
   epoch, firmware compatibility, codec state, clocks or graph availability. */
CR_STATUS CrAudioGprMatchSpfReply(CONST VOID *Data, UINTN Bytes,
                                 UINT32 HostPort, UINT32 Token,
                                 CR_AUDIO_SPF_REPLY *Reply);
