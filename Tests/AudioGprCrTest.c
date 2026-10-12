/* Literal Android/AudioReach wire expectations; no transport/mock driver.
   SPDX-License-Identifier: BSD-3-Clause */
#include <Library/AudioGprCrLib.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned Checks;
#define CHECK(X) do { ++Checks; assert(X); } while (0)
static const UINT8 Query[] = {
  0x60,0x18,0,0, 2,3,0,0, 3,0,0,0, 1,0,0,0,
  0x12,0x34,0x56,0x78, 0x21,0x10,0,1
};
static const UINT8 State[] = {
  0x60,0x1c,0,0, 3,2,0,0, 1,0,0,0, 3,0,0,0,
  0x12,0x34,0x56,0x78, 7,0x10,0,2, 1,0,0,0
};
static const UINT8 Basic[] = {
  0x60,0x20,0,0, 3,2,0,0, 1,0,0,0, 3,0,0,0,
  0x12,0x34,0x56,0x78, 5,0x10,0,2, 0x21,0x10,0,1, 0,0,0,0
};

static void Put32(UINT8 *P, UINT32 Value)
{
  for (unsigned I = 0; I < 4; ++I) {
    P[I] = (UINT8)(Value >> (8 * I));
  }
}

static void DecodeFail(const void *Data, size_t Bytes, CR_STATUS Expected)
{
  CR_AUDIO_GPR_HEADER Out, Saved;
  memset(&Out, 0xa5, sizeof(Out)); Saved = Out;
  CHECK(CrAudioGprDecode(Data, Bytes, &Out) == Expected);
  CHECK(!memcmp(&Out, &Saved, sizeof(Out)));
}

static void ReplyFail(const void *Data, size_t Bytes, UINT32 HostPort,
                      UINT32 Token, CR_STATUS Expected)
{
  CR_AUDIO_SPF_REPLY Out, Saved;
  memset(&Out, 0xa5, sizeof(Out)); Saved = Out;
  CHECK(CrAudioGprMatchSpfReply(Data, Bytes, HostPort, Token, &Out) == Expected);
  CHECK(!memcmp(&Out, &Saved, sizeof(Out)));
}

static void Nominal(void)
{
  UINT8 Buffer[96];
  CR_AUDIO_GPR_HEADER Header;
  CR_AUDIO_SPF_REPLY Reply;

  memset(Buffer, 0x59, sizeof(Buffer));
  CHECK(CrAudioGprBuildSpfQuery(3, 0x78563412, Buffer + 1, 93) == CR_SUCCESS);
  CHECK(!memcmp(Buffer + 1, Query, sizeof(Query)));
  CHECK(Buffer[0] == 0x59);
  for (unsigned I = 25; I < sizeof(Buffer); ++I) {
    CHECK(Buffer[I] == 0x59);
  }
  CHECK(CrAudioGprDecode(Buffer + 1, 24, &Header) == CR_SUCCESS);
  CHECK(Header.HeaderBytes == 24 && Header.PacketBytes == 24);
  CHECK(Header.SourceDomain == 3 && Header.DestinationDomain == 2);
  CHECK(Header.SourcePort == 3 && Header.DestinationPort == 1);
  CHECK(Header.Token == 0x78563412 && Header.Opcode == 0x01001021);
  CHECK(Header.ClientData == 0 && Header.Reserved == 0);
  CHECK(CrAudioGprMatchSpfReply(State, sizeof(State), 3, 0x78563412, &Reply) == CR_SUCCESS);
  CHECK(Reply.Kind == CrAudioSpfReady && Reply.Value == 1);
  memcpy(Buffer + 1, State, sizeof(State)); Buffer[25] = 0;
  CHECK(CrAudioGprMatchSpfReply(Buffer + 1, sizeof(State), 3, 0x78563412, &Reply) == CR_SUCCESS);
  CHECK(Reply.Kind == CrAudioSpfNotReady && Reply.Value == 0);
  CHECK(CrAudioGprMatchSpfReply(Basic, sizeof(Basic), 3, 0x78563412, &Reply) == CR_SUCCESS);
  CHECK(Reply.Kind == CrAudioSpfBasicAckWithoutState && Reply.Value == 0);
  memcpy(Buffer + 1, Basic, sizeof(Basic)); Put32(Buffer + 29, 0xffffffff);
  CHECK(CrAudioGprMatchSpfReply(Buffer + 1, sizeof(Basic), 3, 0x78563412, &Reply) == CR_SUCCESS);
  CHECK(Reply.Kind == CrAudioSpfDspError && Reply.Value == 0xffffffff);

  /* Optional header DWORDs remain opaque, payload shifts for every length. */
  for (unsigned Words = 6; Words <= 15; ++Words) {
    memcpy(Buffer, State, 24);
    memset(Buffer + 24, 0x93, Words * 4 - 24);
    Put32(Buffer, ((Words * 4 + 4) << 8) | (Words << 4));
    Put32(Buffer + Words * 4, 1);
    Buffer[6] = 15;
    CHECK(CrAudioGprDecode(Buffer, Words * 4 + 4, &Header) == CR_SUCCESS);
    CHECK(Header.HeaderBytes == Words * 4 && Header.ClientData == 15);
    CHECK(CrAudioGprMatchSpfReply(Buffer, Words * 4 + 4, 3, 0x78563412, &Reply) == CR_SUCCESS);
    CHECK(Reply.Kind == CrAudioSpfReady && Reply.Value == 1);
  }
  /* Zero is a legal caller-managed token, not a hidden allocation request. */
  CHECK(CrAudioGprBuildSpfQuery(7, 0, Buffer, 24) == CR_SUCCESS);
  CHECK(CrAudioGprDecode(Buffer, 24, &Header) == CR_SUCCESS);
  CHECK(Header.Token == 0 && Header.SourcePort == 7);
  memcpy(Buffer, State, sizeof(State)); Put32(Buffer + 16, 0);
  CHECK(CrAudioGprMatchSpfReply(Buffer, sizeof(State), 3, 0, &Reply) == CR_SUCCESS);
}

static void Malformed(void)
{
  UINT8 Buffer[96];
  CR_AUDIO_GPR_HEADER Header;

  for (unsigned N = 0; N < sizeof(State); ++N) {
    DecodeFail(State, N, CR_COMPROMISED_DATA);
    ReplyFail(State, N, 3, 0x78563412, CR_COMPROMISED_DATA);
  }
  for (unsigned N = 1; N < 16; ++N) {
    memcpy(Buffer, State, sizeof(State)); Buffer[0] |= (UINT8)N;
    DecodeFail(Buffer, sizeof(State), CR_UNSUPPORTED);
    ReplyFail(Buffer, sizeof(State), 3, 0x78563412, CR_UNSUPPORTED);
  }
  for (unsigned N = 0; N < 6; ++N) {
    memcpy(Buffer, State, sizeof(State)); Buffer[0] = (UINT8)(N << 4);
    DecodeFail(Buffer, sizeof(State), CR_COMPROMISED_DATA);
  }
  for (unsigned N = 8; N < 16; ++N) {
    memcpy(Buffer, State, sizeof(State)); Buffer[0] = (UINT8)(N << 4);
    DecodeFail(Buffer, sizeof(State), CR_COMPROMISED_DATA);
  }
  for (unsigned N = 0; N < 64; ++N) {
    if (N == sizeof(State)) continue;
    memcpy(Buffer, State, sizeof(State)); Put32(Buffer, (N << 8) | 0x60);
    DecodeFail(Buffer, sizeof(State), CR_COMPROMISED_DATA);
  }
  for (unsigned N = 1; N < 256; ++N) {
    memcpy(Buffer, State, sizeof(State)); Buffer[7] = (UINT8)N;
    DecodeFail(Buffer, sizeof(State), CR_COMPROMISED_DATA);
    if (N >= 16) {
      Buffer[7] = 0; Buffer[6] = (UINT8)N;
      DecodeFail(Buffer, sizeof(State), CR_COMPROMISED_DATA);
    }
  }
  /* Valid framing but malformed matching state/basic payload lengths. */
  for (unsigned N = 0; N < 32; ++N) {
    memset(Buffer, 0, sizeof(Buffer)); memcpy(Buffer, State, 24);
    Put32(Buffer, ((24 + N) << 8) | 0x60);
    CHECK(CrAudioGprDecode(Buffer, 24 + N, &Header) == CR_SUCCESS);
    if (N != 4) ReplyFail(Buffer, 24 + N, 3, 0x78563412, CR_COMPROMISED_DATA);
    Buffer[20] = 5;
    if (N != 8) ReplyFail(Buffer, 24 + N, 3, 0x78563412, CR_COMPROMISED_DATA);
  }
  memcpy(Buffer, State, sizeof(State)); Put32(Buffer + 24, 2);
  ReplyFail(Buffer, sizeof(State), 3, 0x78563412, CR_COMPROMISED_DATA);
  Put32(Buffer + 24, UINT32_MAX);
  ReplyFail(Buffer, sizeof(State), 3, 0x78563412, CR_COMPROMISED_DATA);

  /* Maximum wire packet size is representable; the next size is rejected. */
  UINT8 *Large = malloc(0x1000000U);
  CHECK(Large != NULL);
  memcpy(Large, State, 24); Put32(Large, 0xffffff60U);
  CHECK(CrAudioGprDecode(Large, 0xffffffU, &Header) == CR_SUCCESS);
  CHECK(Header.PacketBytes == 0xffffffU);
  DecodeFail(Large, 0x1000000U, CR_BAD_BUFFER_SIZE);
  ReplyFail(Large, 0x1000000U, 3, 0x78563412, CR_BAD_BUFFER_SIZE);
  free(Large);
}

static void Unrelated(void)
{
  UINT8 Buffer[32];
  for (unsigned I = 4; I < 24; ++I) {
    if (I == 6 || I == 7) continue;
    memcpy(Buffer, State, sizeof(State)); Buffer[I] ^= 0x80;
    ReplyFail(Buffer, sizeof(State), 3, 0x78563412, CR_NOT_FOUND);
  }
  memcpy(Buffer, Basic, sizeof(Basic)); Buffer[24] ^= 1;
  ReplyFail(Buffer, sizeof(Basic), 3, 0x78563412, CR_NOT_FOUND);
  ReplyFail(State, sizeof(State), 7, 0x78563412, CR_NOT_FOUND);
  ReplyFail(State, sizeof(State), 3, 0x78563413, CR_NOT_FOUND);
}

static void PointerContracts(void)
{
  union { UINT64 Align; UINT8 Bytes[128]; } Storage, Saved;
  UINT8 *Buffer = Storage.Bytes;
  CR_AUDIO_GPR_HEADER Header;
  memset(&Storage, 0x5a, sizeof(Storage));
  memcpy(Buffer + 16, State, sizeof(State)); Saved = Storage;
  for (unsigned I = 0; I < 44; I += 4) {
    if (I + sizeof(Header) > 16) {
      CHECK(CrAudioGprDecode(Buffer + 16, sizeof(State),
                             (CR_AUDIO_GPR_HEADER *)(Buffer + I)) == CR_INVALID_PARAMETER);
    }
    if (I + sizeof(CR_AUDIO_SPF_REPLY) > 16) {
      CHECK(CrAudioGprMatchSpfReply(Buffer + 16, sizeof(State), 3, 0x78563412,
                                   (CR_AUDIO_SPF_REPLY *)(Buffer + I)) == CR_INVALID_PARAMETER);
    }
    CHECK(!memcmp(&Storage, &Saved, sizeof(Storage)));
  }
  CHECK(CrAudioGprDecode(State, sizeof(State), (void *)(Buffer + 65)) == CR_INVALID_PARAMETER);
  CHECK(CrAudioGprMatchSpfReply(State, sizeof(State), 3, 0x78563412,
                               (void *)(Buffer + 65)) == CR_INVALID_PARAMETER);
  CHECK(!memcmp(&Storage, &Saved, sizeof(Storage)));
  DecodeFail(NULL, 24, CR_INVALID_PARAMETER);
  ReplyFail(NULL, 24, 3, 0, CR_INVALID_PARAMETER);
  DecodeFail((void *)(UINTN)-2, 24, CR_INVALID_PARAMETER);
  CHECK(CrAudioGprDecode(State, sizeof(State), (void *)(UINTN)-4) == CR_INVALID_PARAMETER);
  ReplyFail(State, sizeof(State), 0, 0, CR_INVALID_PARAMETER);
  ReplyFail(State, sizeof(State), UINT32_MAX, 0, CR_INVALID_PARAMETER);
  for (unsigned N = 0; N < 24; ++N) {
    CHECK(CrAudioGprBuildSpfQuery(3, 0, Buffer, N) == CR_BUFFER_TOO_SMALL);
    CHECK(!memcmp(&Storage, &Saved, sizeof(Storage)));
  }
  CHECK(CrAudioGprBuildSpfQuery(0, 0, Buffer, 24) == CR_INVALID_PARAMETER);
  CHECK(CrAudioGprBuildSpfQuery(UINT32_MAX, 0, Buffer, 24) == CR_INVALID_PARAMETER);
  CHECK(CrAudioGprBuildSpfQuery(3, 0, NULL, 24) == CR_INVALID_PARAMETER);
  CHECK(CrAudioGprBuildSpfQuery(3, 0, (void *)(UINTN)-2, 24) == CR_INVALID_PARAMETER);
  CHECK(!memcmp(&Storage, &Saved, sizeof(Storage)));
}

int main(void)
{
  Nominal();
  Malformed();
  Unrelated();
  PointerContracts();
  printf("Audio GPR actual wire helpers PASS: %u checks; no transport/epoch claim\n", Checks);
  return 0;
}
