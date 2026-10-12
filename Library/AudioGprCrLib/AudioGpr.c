/* Copyright (c) 2026 Project Aloha Authors.
   SPDX-License-Identifier: BSD-3-Clause
   Original bounded implementation of pinned Qualcomm AudioReach wire facts.
   See README.md and LICENSE. */
#include <Library/AudioGprCrLib.h>

#define MAX_N ((UINTN)-1)

static UINT32 Read32(CONST UINT8 *Data)
{
  return (UINT32)Data[0] | ((UINT32)Data[1] << 8) |
         ((UINT32)Data[2] << 16) | ((UINT32)Data[3] << 24);
}

static VOID Write32(UINT8 *Data, UINT32 Value)
{
  for (UINT32 Index = 0; Index < 4; ++Index) {
    Data[Index] = (UINT8)(Value >> (8U * Index));
  }
}

static CR_STATUS Arguments(CONST VOID *Data, UINTN Bytes,
                           CONST VOID *Output, UINTN OutputBytes)
{
  UINTN In = (UINTN)Data;
  UINTN Out = (UINTN)Output;

  if (Data == NULL || Output == NULL || (Out & 3U) != 0 ||
      In > MAX_N - Bytes || Out > MAX_N - OutputBytes) {
    return CR_INVALID_PARAMETER;
  }
  if (Bytes > CR_AUDIO_GPR_MAX_PACKET_BYTES) {
    return CR_BAD_BUFFER_SIZE;
  }
  if (In < Out + OutputBytes && Out < In + Bytes) {
    return CR_INVALID_PARAMETER;
  }
  return CR_SUCCESS;
}

static BOOLEAN ValidHostPort(UINT32 HostPort)
{
  return HostPort != 0 && HostPort != CR_MAX_UINT32;
}

CR_STATUS CrAudioGprDecode(CONST VOID *Data, UINTN Bytes,
                           CR_AUDIO_GPR_HEADER *Header)
{
  CONST UINT8 *Buffer = Data;
  CR_AUDIO_GPR_HEADER Result = {0};
  UINT32 Word;
  CR_STATUS Status = Arguments(Data, Bytes, Header, sizeof(*Header));

  if (CR_ERROR(Status)) {
    return Status;
  }
  if (Bytes < CR_AUDIO_GPR_HEADER_BYTES) {
    return CR_COMPROMISED_DATA;
  }
  Word = Read32(Buffer);
  if ((Word & 0xfU) != 0) {
    return CR_UNSUPPORTED;
  }
  Result.HeaderBytes = ((Word >> 4) & 0xfU) * 4U;
  Result.PacketBytes = Word >> 8;
  if (Result.HeaderBytes < CR_AUDIO_GPR_HEADER_BYTES ||
      Result.HeaderBytes > Result.PacketBytes || Result.PacketBytes != Bytes ||
      (Buffer[6] & 0xf0U) != 0 || Buffer[7] != 0) {
    return CR_COMPROMISED_DATA;
  }
  Result.DestinationDomain = Buffer[4];
  Result.SourceDomain = Buffer[5];
  Result.ClientData = Buffer[6];
  Result.SourcePort = Read32(Buffer + 8);
  Result.DestinationPort = Read32(Buffer + 12);
  Result.Token = Read32(Buffer + 16);
  Result.Opcode = Read32(Buffer + 20);
  *Header = Result;
  return CR_SUCCESS;
}

CR_STATUS CrAudioGprBuildSpfQuery(UINT32 HostPort, UINT32 Token,
                                 VOID *Buffer, UINTN Capacity)
{
  UINT8 *Bytes = Buffer;

  if (Buffer == NULL || (UINTN)Buffer > MAX_N - Capacity ||
      !ValidHostPort(HostPort)) {
    return CR_INVALID_PARAMETER;
  }
  if (Capacity < CR_AUDIO_GPR_HEADER_BYTES) {
    return CR_BUFFER_TOO_SMALL;
  }
  Write32(Bytes, (CR_AUDIO_GPR_HEADER_BYTES << 8) | (6U << 4));
  Bytes[4] = CR_AUDIO_GPR_ADSP_DOMAIN;
  Bytes[5] = CR_AUDIO_GPR_APPS_DOMAIN;
  Bytes[6] = 0;
  Bytes[7] = 0;
  Write32(Bytes + 8, HostPort);
  Write32(Bytes + 12, CR_AUDIO_GPR_APM_PORT);
  Write32(Bytes + 16, Token);
  Write32(Bytes + 20, CR_AUDIO_APM_GET_SPF_STATE);
  return CR_SUCCESS;
}

CR_STATUS CrAudioGprMatchSpfReply(CONST VOID *Data, UINTN Bytes,
                                 UINT32 HostPort, UINT32 Token,
                                 CR_AUDIO_SPF_REPLY *Reply)
{
  CR_AUDIO_GPR_HEADER Header;
  CR_AUDIO_SPF_REPLY Result;
  CR_STATUS Status = Arguments(Data, Bytes, Reply, sizeof(*Reply));

  if (CR_ERROR(Status)) {
    return Status;
  }
  if (!ValidHostPort(HostPort)) {
    return CR_INVALID_PARAMETER;
  }
  Status = CrAudioGprDecode(Data, Bytes, &Header);
  if (CR_ERROR(Status)) {
    return Status;
  }
  if (Header.SourceDomain != CR_AUDIO_GPR_ADSP_DOMAIN ||
      Header.DestinationDomain != CR_AUDIO_GPR_APPS_DOMAIN ||
      Header.SourcePort != CR_AUDIO_GPR_APM_PORT ||
      Header.DestinationPort != HostPort || Header.Token != Token) {
    return CR_NOT_FOUND;
  }

  CONST UINT8 *Payload = (CONST UINT8 *)Data + Header.HeaderBytes;
  UINT32 PayloadBytes = Header.PacketBytes - Header.HeaderBytes;
  if (Header.Opcode == CR_AUDIO_APM_SPF_STATE_RESPONSE) {
    if (PayloadBytes != 4) {
      return CR_COMPROMISED_DATA;
    }
    Result.Value = Read32(Payload);
    if (Result.Value > 1) {
      return CR_COMPROMISED_DATA;
    }
    Result.Kind = Result.Value == 1 ? CrAudioSpfReady : CrAudioSpfNotReady;
  } else if (Header.Opcode == CR_AUDIO_GPR_BASIC_RESPONSE) {
    if (PayloadBytes != 8) {
      return CR_COMPROMISED_DATA;
    }
    if (Read32(Payload) != CR_AUDIO_APM_GET_SPF_STATE) {
      return CR_NOT_FOUND;
    }
    Result.Value = Read32(Payload + 4);
    Result.Kind = Result.Value == 0 ? CrAudioSpfBasicAckWithoutState : CrAudioSpfDspError;
  } else {
    return CR_NOT_FOUND;
  }
  *Reply = Result;
  return CR_SUCCESS;
}
