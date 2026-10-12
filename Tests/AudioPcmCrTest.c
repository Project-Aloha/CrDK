/* Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause */
#include <Library/AudioPcmCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Actual pinned upstream headers, not local replicas. */
#include <gpr_packet.h>
#include <apm_memmap_api.h>
#include <wr_sh_mem_ep_api.h>
#include <rd_sh_mem_ep_api.h>

static unsigned Checks;
#define CHECK(X) do { ++Checks; if (!(X)) { fprintf(stderr, "%d: %s\n", __LINE__, #X); exit(1); } } while (0)

static gpr_packet_t Header(CR_AUDIO_PCM_REQUEST *R, UINT32 Opcode, UINT32 N, int Reply)
{
  gpr_packet_t H = {0};
  H.header = ((UINT32)sizeof(H) + N) << 8 | (sizeof(H) / 4 << 4);
  H.src_domain_id = Reply ? 2 : 3;
  H.dst_domain_id = Reply ? 3 : 2;
  H.src_port = Reply ? R->EndpointPort : R->HostPort;
  H.dst_port = Reply ? R->HostPort : R->EndpointPort;
  H.token = R->Token;
  H.opcode = Opcode;
  return H;
}

static UINTN OracleBuild(CR_AUDIO_PCM_REQUEST *R, UINT8 *P)
{
  UINT32 Opcode, N;
  UINT8 Payload[64] = {0};
  if (R->Operation == CrAudioPcmMap) {
    struct { apm_cmd_shared_mem_map_regions_t H; apm_shared_map_region_payload_t R; } M = {0};
    M.H.mem_pool_id = APM_MEMORY_MAP_SHMEM8_4K_POOL;
    M.H.num_regions = 1; M.H.property_flag = R->Flags;
    M.R.shm_addr_lsw = (UINT32)R->Address; M.R.shm_addr_msw = (UINT32)(R->Address >> 32);
    M.R.mem_size_bytes = R->Bytes;
    Opcode = APM_CMD_SHARED_MEM_MAP_REGIONS; N = sizeof(M);
    memcpy(Payload, &M, N);
  } else if (R->Operation == CrAudioPcmUnmap) {
    apm_cmd_shared_mem_unmap_regions_t U = {R->MapHandle};
    Opcode = APM_CMD_SHARED_MEM_UNMAP_REGIONS; N = sizeof(U);
    memcpy(Payload, &U, N);
  } else if (R->Operation == CrAudioPcmWrite) {
    data_cmd_wr_sh_mem_ep_data_buffer_v2_t W = {0};
    W.data_buf_addr_lsw = (UINT32)R->Address; W.data_buf_addr_msw = (UINT32)(R->Address >> 32);
    W.data_mem_map_handle = R->MapHandle; W.data_buf_size = R->Bytes;
    W.timestamp_lsw = (UINT32)R->Timestamp; W.timestamp_msw = (UINT32)(R->Timestamp >> 32);
    W.flags = R->Flags;
    Opcode = DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2; N = sizeof(W);
    memcpy(Payload, &W, N);
  } else {
    data_cmd_rd_sh_mem_ep_data_buffer_v2_t D = {0};
    D.data_buf_addr_lsw = (UINT32)R->Address; D.data_buf_addr_msw = (UINT32)(R->Address >> 32);
    D.data_mem_map_handle = R->MapHandle; D.data_buf_size = R->Bytes;
    Opcode = DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2; N = sizeof(D);
    memcpy(Payload, &D, N);
  }
  gpr_packet_t H = Header(R, Opcode, N, 0);
  memcpy(P, &H, sizeof(H)); memcpy(P + sizeof(H), Payload, N);
  return sizeof(H) + N;
}

static UINTN OracleReply(CR_AUDIO_PCM_REQUEST *R, UINT8 *P, UINT32 Error)
{
  UINT32 Opcode, N;
  UINT8 Payload[64] = {0};
  if (R->Operation == CrAudioPcmMap) {
    apm_cmd_rsp_shared_mem_map_regions_t M = {0xabcdef01};
    Opcode = APM_CMD_RSP_SHARED_MEM_MAP_REGIONS; N = sizeof(M);
    memcpy(Payload, &M, N);
  } else if (R->Operation == CrAudioPcmUnmap) {
    UINT32 A[2] = {APM_CMD_SHARED_MEM_UNMAP_REGIONS, Error};
    Opcode = CR_AUDIO_GPR_BASIC_RESPONSE; N = sizeof(A);
    memcpy(Payload, A, N);
  } else if (R->Operation == CrAudioPcmWrite) {
    data_cmd_rsp_wr_sh_mem_ep_data_buffer_done_v2_t W = {0};
    W.data_buf_addr_lsw = (UINT32)R->Address; W.data_buf_addr_msw = (UINT32)(R->Address >> 32);
    W.data_mem_map_handle = R->MapHandle; W.data_status = Error; W.md_status = Error + 1;
    Opcode = DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2; N = sizeof(W);
    memcpy(Payload, &W, N);
  } else {
    data_cmd_rsp_rd_sh_mem_ep_data_buffer_done_v2_t D = {0};
    D.data_buf_addr_lsw = (UINT32)R->Address; D.data_buf_addr_msw = (UINT32)(R->Address >> 32);
    D.data_mem_map_handle = R->MapHandle; D.data_size = R->Bytes / 2;
    D.data_status = Error; D.md_status = Error + 1; D.num_frames = 1;
    D.timestamp_lsw = 0xabcdef12; D.timestamp_msw = 0xfedcba98;
    D.flags = RD_SH_MEM_EP_BIT_MASK_TIMESTAMP_VALID_FLAG;
    Opcode = DATA_CMD_RSP_RD_SH_MEM_EP_DATA_BUFFER_DONE_V2; N = sizeof(D);
    memcpy(Payload, &D, N);
  }
  gpr_packet_t H = Header(R, Opcode, N, 1);
  memcpy(P, &H, sizeof(H)); memcpy(P + sizeof(H), Payload, N);
  return sizeof(H) + N;
}

static void Reject(CR_AUDIO_PCM_REQUEST *R, UINT8 *P, UINTN N, CR_STATUS Expected)
{
  CR_AUDIO_PCM_REPLY O, Before;
  memset(&O, 0xa5, sizeof(O)); Before = O;
  CHECK(CrAudioPcmMatchReply(R, P, N, &O) == Expected);
  CHECK(memcmp(&O, &Before, sizeof(O)) == 0);
}

static CR_AUDIO_PCM_REQUEST Request(UINT32 Op, UINT32 I)
{
  CR_AUDIO_PCM_REQUEST R = {0};
  R.Operation = Op; R.HostPort = 0x2010; R.EndpointPort = Op <= 2 ? 1 : 0x60000101;
  R.Token = I; R.Address = 0x120000000ULL + (UINT64)I * 4096;
  R.Bytes = 4096; R.MapHandle = I + 1;
  if (Op == CrAudioPcmMap) { R.MapHandle = 0; R.Flags = I & 1 ? 2 : 0; }
  if (Op == CrAudioPcmUnmap) { R.Address = 0; R.Bytes = 0; }
  if (Op == CrAudioPcmWrite) {
    R.Flags = CR_AUDIO_PCM_TIMESTAMP_VALID | (I & 1 ? CR_AUDIO_PCM_END_OF_FRAME : 0);
    R.Timestamp = 0xfedcba9876543210ULL + I;
  }
  return R;
}

static void OracleCases(void)
{
  UINT8 Actual[128], Expected[128], Packet[128], Extended[128];
  CR_AUDIO_PCM_REPLY O;
  for (UINT32 Op = 1; Op <= 4; ++Op) {
    for (UINT32 I = 0; I < 256; ++I) {
      CR_AUDIO_PCM_REQUEST R = Request(Op, I);
      memset(Actual, 0xa5, sizeof(Actual)); memset(Expected, 0xa5, sizeof(Expected));
      UINTN N = OracleBuild(&R, Expected + 1);
      CHECK(CrAudioPcmBuild(&R, Actual + 1, sizeof(Actual) - 1) == CR_SUCCESS);
      CHECK(memcmp(Actual, Expected, sizeof(Actual)) == 0);
      N = OracleReply(&R, Packet + 1, I);
      CHECK(CrAudioPcmMatchReply(&R, Packet + 1, N, &O) == CR_SUCCESS);
      CHECK(O.Kind == (Op == 2 && I != 0 ? CrAudioPcmBasicError : Op));
      CHECK(O.Reserved == 0);
      CHECK(O.DspStatus == (Op == 1 ? 0 : I));
      CHECK(O.MapHandle == (Op == 1 ? 0xabcdef01 : Op == 2 ? 0 : R.MapHandle));
      if (Op >= 3) {
        CHECK(O.MetadataStatus == I + 1);
        CHECK(O.DataBytes == (Op == 3 ? 0 : R.Bytes / 2));
      }
      if (Op == 4) {
        CHECK(O.FrameCount == 1 && O.Timestamp == 0xfedcba98abcdef12ULL);
        CHECK(O.Flags == CR_AUDIO_PCM_TIMESTAMP_VALID);
      }
      /* Real GPR extended header shifts payload; no fixed24 assumption. */
      memcpy(Extended, Packet + 1, 24); memset(Extended + 24, 0x72, 8);
      memcpy(Extended + 32, Packet + 25, N - 24);
      UINT32 H = (UINT32)((N + 8) << 8) | 0x80;
      memcpy(Extended, &H, 4);
      CHECK(CrAudioPcmMatchReply(&R, Extended, N + 8, &O) == CR_SUCCESS);
      /* Wrong exact token, either domain, either port, opcode are unrelated. */
      CONST UINT32 Offsets[] = {4, 5, 8, 12, 16, 20};
      for (UINT32 J = 0; J < sizeof(Offsets) / sizeof(Offsets[0]); ++J) {
        UINT32 At = 1 + Offsets[J]; Packet[At] ^= 0x10;
        Reject(&R, Packet + 1, N, CR_NOT_FOUND); Packet[At] ^= 0x10;
      }
      /* Test every actual truncation, including missing tail bytes. */
      for (UINTN Short = 0; Short < N; ++Short) {
        Reject(&R, Packet + 1, Short, CR_COMPROMISED_DATA);
      }
      Reject(&R, Packet + 1, N + 1, CR_COMPROMISED_DATA);
      Packet[8] = 1; Reject(&R, Packet + 1, N, CR_COMPROMISED_DATA); Packet[8] = 0;
    }
  }
}

static void NegativeCases(void)
{
  UINT8 P[128], Before[128]; CR_AUDIO_PCM_REPLY O;
  for (UINT32 Op = 1; Op <= 4; ++Op) {
    CR_AUDIO_PCM_REQUEST R = Request(Op, 0), Bad;
    UINTN N = OracleBuild(&R, P);
    for (UINTN Capacity = 0; Capacity < N; ++Capacity) {
      memset(P, 0xc9, sizeof(P)); memcpy(Before, P, sizeof(P));
      CHECK(CrAudioPcmBuild(&R, P, Capacity) == CR_BUFFER_TOO_SMALL);
      CHECK(memcmp(P, Before, sizeof(P)) == 0);
    }
    for (UINT32 Case = 0; Case < 12; ++Case) {
      Bad = R;
      switch (Case) {
        case 0: Bad.Reserved = 1; break;
        case 1: Bad.HostPort = 0; break;
        case 2: Bad.EndpointPort = CR_MAX_UINT32; break;
        case 3: Bad.Flags = 0x100; break;
        case 4: Bad.Address = 0x1000000000ULL; break;
        case 5: Bad.Address = 0xffffffffffffffffULL; break;
        case 6: Bad.Address = 1; break;
        case 7: Bad.Bytes = Op == 2 ? 1 : 0; break;
        case 8: Bad.MapHandle = Op == 1 ? 1 : 0; break;
        case 9: Bad.Flags = 0; Bad.Timestamp = 1; break;
        case 10: Bad.Address = 0xfffffffc0ULL; Bad.Bytes = 65; break;
        default: Bad.Operation = 0; break;
      }
      memset(P, 0xc9, sizeof(P)); memcpy(Before, P, sizeof(P));
      CHECK(CrAudioPcmBuild(&Bad, P, sizeof(P)) != CR_SUCCESS);
      CHECK(memcmp(P, Before, sizeof(P)) == 0);
    }
    /* Basic status never returns a data buffer or synthesizes a map handle. */
    UINT32 Command;
    OracleBuild(&R, P); memcpy(&Command, P + 20, 4);
    gpr_packet_t H = Header(&R, CR_AUDIO_GPR_BASIC_RESPONSE, 8, 1);
    memcpy(P, &H, sizeof(H)); memcpy(P + 24, &Command, 4);
    for (UINT32 Error = 0; Error < 2; ++Error) {
      memcpy(P + 28, &Error, 4);
      CHECK(CrAudioPcmMatchReply(&R, P, 32, &O) == CR_SUCCESS);
      CHECK(O.Kind == (Error ? CrAudioPcmBasicError : Op == 2 ? CrAudioPcmUnmapped : CrAudioPcmBasicAck));
      CHECK(O.MapHandle == 0 && O.DataBytes == 0);
    }
    P[24] ^= 0x10; Reject(&R, P, 32, CR_NOT_FOUND);
    /* Typed alias/range/unaligned outputs must not touch retained request. */
    CHECK(CrAudioPcmBuild(&R, &R, sizeof(R)) == CR_INVALID_PARAMETER);
    CHECK(CrAudioPcmBuild(&R, (VOID *)(~(UINTN)0 - 7), 16) == CR_INVALID_PARAMETER);
    CHECK(CrAudioPcmBuild((VOID *)((UINT8 *)&R + 1), P, sizeof(P)) == CR_INVALID_PARAMETER);
    CHECK(CrAudioPcmMatchReply(&R, P, 32, (VOID *)((UINT8 *)&O + 1)) == CR_INVALID_PARAMETER);
    CHECK(CrAudioPcmMatchReply(&R, P, 32, (VOID *)&R) == CR_INVALID_PARAMETER);
    CHECK(CrAudioPcmMatchReply(&R, P, 32, (VOID *)P) == CR_INVALID_PARAMETER);
    if (Op == 2) { continue; }
    N = OracleReply(&R, P, 0);
    if (Op == 1) {
      memset(P + 24, 0, 4); Reject(&R, P, N, CR_COMPROMISED_DATA);
    } else {
      UINT32 Address = Op == 3 ? 24 : 28;
      UINT32 Handle = Op == 3 ? 32 : 36;
      UINT32 Metadata = Op == 3 ? 40 : 64;
      for (UINT32 I = 0; I < 8; ++I) {
        P[Address + I] ^= 1; Reject(&R, P, N, CR_NOT_FOUND); P[Address + I] ^= 1;
      }
      P[Handle] ^= 1; Reject(&R, P, N, CR_NOT_FOUND); P[Handle] ^= 1;
      for (UINT32 I = 0; I < 12; ++I) {
        P[Metadata + I] ^= 1; Reject(&R, P, N, CR_COMPROMISED_DATA); P[Metadata + I] ^= 1;
      }
      if (Op == 4) {
        UINT32 TooBig = R.Bytes + 1;
        memcpy(P + 40, &TooBig, 4); Reject(&R, P, N, CR_COMPROMISED_DATA);
        N = OracleReply(&R, P, 0); P[52] |= 1; Reject(&R, P, N, CR_COMPROMISED_DATA);
        N = OracleReply(&R, P, 0); P[76] = 1; Reject(&R, P, N, CR_COMPROMISED_DATA);
      }
    }
  }
  CR_AUDIO_PCM_REQUEST Edge = Request(CrAudioPcmRead, CR_MAX_UINT32);
  Edge.MapHandle = 1; Edge.Address = 0xfffffffc0ULL; Edge.Bytes = 64;
  CHECK(CrAudioPcmBuild(&Edge, P, sizeof(P)) == CR_SUCCESS);
  Edge.Bytes = 65; CHECK(CrAudioPcmBuild(&Edge, P, sizeof(P)) == CR_INVALID_PARAMETER);
  Edge = Request(CrAudioPcmMap, 0); Edge.Address = 0x100000000ULL;
  CHECK(CrAudioPcmBuild(&Edge, P, sizeof(P)) == CR_SUCCESS); /* low word0 valid */
}

int main(void)
{
  CHECK(sizeof(gpr_packet_t) == 24);
  CHECK(sizeof(data_cmd_wr_sh_mem_ep_data_buffer_v2_t) == 44);
  CHECK(sizeof(data_cmd_rd_sh_mem_ep_data_buffer_v2_t) == 32);
  CHECK(sizeof(data_cmd_rsp_wr_sh_mem_ep_data_buffer_done_v2_t) == 32);
  CHECK(sizeof(data_cmd_rsp_rd_sh_mem_ep_data_buffer_done_v2_t) == 56);
  OracleCases(); NegativeCases();
  printf("Audio PCM upstream-struct oracle + bounded wire tests PASS: %u checks\n", Checks);
  return 0;
}
