/* Copyright (c) 2026 Project Aloha Authors.
   SPDX-License-Identifier: BSD-3-Clause
   Original implementation of pinned AudioReach packet facts; see README. */
#include <Library/AudioPcmCrLib.h>

#define MAX_N ((UINTN)-1)
#define DSP_LIMIT 0x1000000000ULL
#define WRITE_FLAGS (CR_AUDIO_PCM_TIMESTAMP_VALID | CR_AUDIO_PCM_LAST_BUFFER | \
                     CR_AUDIO_PCM_TIMESTAMP_CONTINUE | CR_AUDIO_PCM_END_OF_FRAME)

static UINT32 R32(CONST UINT8 *P)
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) |
         ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

static UINT64 R64(CONST UINT8 *P)
{
  return (UINT64)R32(P) | ((UINT64)R32(P + 4) << 32);
}

static VOID W32(UINT8 *P, UINT32 V)
{
  for (UINT32 I = 0; I < 4; ++I) {
    P[I] = (UINT8)(V >> (I * 8U));
  }
}

static VOID W64(UINT8 *P, UINT64 V)
{
  W32(P, (UINT32)V);
  W32(P + 4, (UINT32)(V >> 32));
}

static BOOLEAN Range(CONST VOID *P, UINTN N, UINTN Alignment)
{
  return P != NULL && ((UINTN)P & (Alignment - 1U)) == 0 &&
         (UINTN)P <= MAX_N - N;
}

static BOOLEAN Disjoint(CONST VOID *P, UINTN N, CONST VOID *Q, UINTN M)
{
  return (UINTN)P >= (UINTN)Q + M || (UINTN)Q >= (UINTN)P + N;
}

static BOOLEAN Port(UINT32 P)
{
  return P != 0 && P != CR_MAX_UINT32;
}

static CR_STATUS Validate(CONST CR_AUDIO_PCM_REQUEST *R,
                           UINT32 *Opcode, UINT32 *PacketBytes)
{
  if (!Range(R, sizeof(*R), 8) || !Port(R->HostPort) ||
      !Port(R->EndpointPort) || R->Reserved != 0) {
    return CR_INVALID_PARAMETER;
  }
  if (R->Operation == CrAudioPcmUnmap) {
    if (R->EndpointPort != CR_AUDIO_GPR_APM_PORT || R->MapHandle == 0 ||
        R->Address != 0 || R->Bytes != 0 || R->Timestamp != 0 || R->Flags != 0) {
      return CR_INVALID_PARAMETER;
    }
    *Opcode = CR_AUDIO_PCM_UNMAP;
    *PacketBytes = CR_AUDIO_PCM_UNMAP_PACKET_BYTES;
    return CR_SUCCESS;
  }
  if (R->Address == 0 || R->Address >= DSP_LIMIT || R->Bytes == 0 ||
      R->Bytes > DSP_LIMIT - R->Address) {
    return CR_INVALID_PARAMETER;
  }
  switch (R->Operation) {
    case CrAudioPcmMap:
      if (R->EndpointPort != CR_AUDIO_GPR_APM_PORT || R->MapHandle != 0 ||
          (R->Address & 4095U) != 0 || (R->Bytes & 4095U) != 0 ||
          (R->Flags != 0 && R->Flags != 2) || R->Timestamp != 0) {
        return CR_INVALID_PARAMETER;
      }
      *Opcode = CR_AUDIO_PCM_MAP;
      *PacketBytes = CR_AUDIO_PCM_MAP_PACKET_BYTES;
      return CR_SUCCESS;
    case CrAudioPcmWrite:
    case CrAudioPcmRead:
      if ((R->Address & 63U) != 0 || R->MapHandle == 0) {
        return CR_INVALID_PARAMETER;
      }
      if (R->Operation == CrAudioPcmWrite) {
        if ((R->Flags & ~WRITE_FLAGS) != 0 ||
            ((R->Flags & CR_AUDIO_PCM_TIMESTAMP_VALID) == 0 && R->Timestamp != 0)) {
          return CR_INVALID_PARAMETER;
        }
        *Opcode = CR_AUDIO_PCM_WRITE_V2;
        *PacketBytes = CR_AUDIO_PCM_WRITE_PACKET_BYTES;
      } else {
        if (R->Flags != 0 || R->Timestamp != 0) {
          return CR_INVALID_PARAMETER;
        }
        *Opcode = CR_AUDIO_PCM_READ_V2;
        *PacketBytes = CR_AUDIO_PCM_READ_PACKET_BYTES;
      }
      return CR_SUCCESS;
    default:
      return CR_UNSUPPORTED;
  }
}

CR_STATUS CrAudioPcmBuild(CONST CR_AUDIO_PCM_REQUEST *Request,
                          VOID *Buffer, UINTN Capacity)
{
  UINT32 Opcode, Bytes;
  UINT8 *B = Buffer;
  CR_STATUS S = Validate(Request, &Opcode, &Bytes);
  if (S != CR_SUCCESS) {
    return S;
  }
  if (!Range(Buffer, Capacity, 1) ||
      !Disjoint(Request, sizeof(*Request), Buffer, Capacity)) {
    return CR_INVALID_PARAMETER;
  }
  if (Capacity < Bytes) {
    return CR_BUFFER_TOO_SMALL;
  }
  for (UINT32 I = 0; I < Bytes; ++I) {
    B[I] = 0;
  }
  W32(B, (Bytes << 8) | (6U << 4));
  B[4] = CR_AUDIO_GPR_ADSP_DOMAIN;
  B[5] = CR_AUDIO_GPR_APPS_DOMAIN;
  W32(B + 8, Request->HostPort);
  W32(B + 12, Request->EndpointPort);
  W32(B + 16, Request->Token);
  W32(B + 20, Opcode);
  if (Request->Operation == CrAudioPcmMap) {
    W32(B + 24, 0x00010003U); /* pool3, one region (two LE UINT16s) */
    W32(B + 28, Request->Flags);
    W64(B + 32, Request->Address);
    W32(B + 40, Request->Bytes);
  } else if (Request->Operation == CrAudioPcmUnmap) {
    W32(B + 24, Request->MapHandle);
  } else {
    W64(B + 24, Request->Address);
    W32(B + 32, Request->MapHandle);
    W32(B + 36, Request->Bytes);
    if (Request->Operation == CrAudioPcmWrite) {
      W64(B + 40, Request->Timestamp);
      W32(B + 48, Request->Flags);
    }
  }
  return CR_SUCCESS;
}

CR_STATUS CrAudioPcmMatchReply(CONST CR_AUDIO_PCM_REQUEST *Request,
                               CONST VOID *Packet, UINTN Bytes,
                               CR_AUDIO_PCM_REPLY *Reply)
{
  UINT32 Opcode, Ignored, N;
  CR_AUDIO_GPR_HEADER H;
  CR_AUDIO_PCM_REPLY Result = {0};
  CONST UINT8 *P;
  CR_STATUS S = Validate(Request, &Opcode, &Ignored);
  if (S != CR_SUCCESS) {
    return S;
  }
  if (!Range(Packet, Bytes, 1) || !Range(Reply, sizeof(*Reply), 8) ||
      !Disjoint(Request, sizeof(*Request), Packet, Bytes) ||
      !Disjoint(Request, sizeof(*Request), Reply, sizeof(*Reply)) ||
      !Disjoint(Packet, Bytes, Reply, sizeof(*Reply))) {
    return CR_INVALID_PARAMETER;
  }
  S = CrAudioGprDecode(Packet, Bytes, &H);
  if (S != CR_SUCCESS) {
    return S;
  }
  if (H.SourceDomain != CR_AUDIO_GPR_ADSP_DOMAIN ||
      H.DestinationDomain != CR_AUDIO_GPR_APPS_DOMAIN ||
      H.SourcePort != Request->EndpointPort || H.DestinationPort != Request->HostPort ||
      H.Token != Request->Token) {
    return CR_NOT_FOUND;
  }
  P = (CONST UINT8 *)Packet + H.HeaderBytes;
  N = H.PacketBytes - H.HeaderBytes;
  if (H.Opcode == CR_AUDIO_GPR_BASIC_RESPONSE) {
    if (N != 8) {
      return CR_COMPROMISED_DATA;
    }
    if (R32(P) != Opcode) {
      return CR_NOT_FOUND;
    }
    Result.DspStatus = R32(P + 4);
    Result.Kind = Result.DspStatus != 0 ? CrAudioPcmBasicError :
      (Request->Operation == CrAudioPcmUnmap ? CrAudioPcmUnmapped : CrAudioPcmBasicAck);
  } else if (Request->Operation == CrAudioPcmMap && H.Opcode == CR_AUDIO_PCM_MAP_RESPONSE) {
    if (N != 4 || R32(P) == 0) {
      return CR_COMPROMISED_DATA;
    }
    Result.Kind = CrAudioPcmMapHandle;
    Result.MapHandle = R32(P);
  } else if (Request->Operation == CrAudioPcmWrite && H.Opcode == CR_AUDIO_PCM_WRITE_DONE_V2) {
    if (N != 32) {
      return CR_COMPROMISED_DATA;
    }
    if (R64(P) != Request->Address || R32(P + 8) != Request->MapHandle) {
      return CR_NOT_FOUND;
    }
    if (R64(P + 16) != 0 || R32(P + 24) != 0) {
      return CR_COMPROMISED_DATA;
    }
    Result.Kind = CrAudioPcmWriteDone;
    Result.DspStatus = R32(P + 12);
    Result.MetadataStatus = R32(P + 28);
    Result.MapHandle = Request->MapHandle;
  } else if (Request->Operation == CrAudioPcmRead && H.Opcode == CR_AUDIO_PCM_READ_DONE_V2) {
    if (N != 56) {
      return CR_COMPROMISED_DATA;
    }
    if (R64(P + 4) != Request->Address || R32(P + 12) != Request->MapHandle) {
      return CR_NOT_FOUND;
    }
    if (R32(P + 16) > Request->Bytes ||
        (R32(P + 28) & ~CR_AUDIO_PCM_TIMESTAMP_VALID) != 0 ||
        R64(P + 40) != 0 || R32(P + 48) != 0 || R32(P + 52) != 0) {
      return CR_COMPROMISED_DATA;
    }
    Result.Kind = CrAudioPcmReadDone;
    Result.DspStatus = R32(P);
    Result.MetadataStatus = R32(P + 36);
    Result.MapHandle = Request->MapHandle;
    Result.DataBytes = R32(P + 16);
    Result.Timestamp = R64(P + 20);
    Result.Flags = R32(P + 28);
    Result.FrameCount = R32(P + 32);
  } else {
    return CR_NOT_FOUND;
  }
  *Reply = Result;
  return CR_SUCCESS;
}
