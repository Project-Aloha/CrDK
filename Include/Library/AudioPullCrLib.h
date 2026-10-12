/* Retained Pull-mode owner on an existing permanent graph client port.
 * Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <Library/AudioGraphCrLib.h>
#include <Library/AudioPcmCrLib.h>
#define CR_AUDIO_PULL_COMMANDS 7U
#define CR_AUDIO_PULL_PACKET_BYTES 96U
#define CR_AUDIO_PULL_MAILBOX 8U
#define CR_AUDIO_PULL_RX_BYTES 128U
typedef struct {UINT64 RingAddress,PositionAddress;UINT32 RingBytes,Watermark;} CR_AUDIO_PULL_MEMORY;
typedef struct {
 VOID *Context;
 /* Real outer owner checks exact native memory lease, immutable local HAL
  * mappings and the current source before any copy. Never a frontend Ready. */
 BOOLEAN (*Current)(VOID *);
 CR_AUDIO_GRAPH_SEND_RESULT (*Send)(VOID *,CONST VOID *,UINT32);
} CR_AUDIO_PULL_BINDING;
typedef struct {
 UINT32 Token,Opcode,Destination,Bytes;UINT64 Deadline;
 BOOLEAN Prepared,Attempted,Complete,TimedOut;
 CR_AUDIO_GRAPH_ACCEPTANCE Acceptance;
 UINT8 Packet[CR_AUDIO_PULL_PACKET_BYTES];
} CR_AUDIO_PULL_COMMAND;
typedef struct {
 UINTN Self;CR_AUDIO_GRAPH_SESSION *Graph;CR_AUDIO_GRAPH_EPOCH Epoch;
 CR_AUDIO_PULL_BINDING Binding;CR_AUDIO_PULL_MEMORY Memory;UINT64 GraphSession,Timeout,LastNow;
 UINT32 Instance,MapHandle[2],Possible,Next,Read,Count;
 CR_AUDIO_PULL_COMMAND Commands[CR_AUDIO_PULL_COMMANDS];
 struct {UINT32 Bytes;UINT8 Data[CR_AUDIO_PULL_RX_BYTES];} Rx[CR_AUDIO_PULL_MAILBOX];
 CR_STATUS Error;BOOLEAN Closing,Failed,Lost,Overflow,Active,Ready,Ended,Uncertain;
 UINT64 WatermarkEvents;UINT32 LastWatermark;
} CR_AUDIO_PULL_SESSION;
/* All APIs are caller-serialized with the actual graph/port owner. Init takes
 * virgin zeroed Session, a live Configured typed Pull graph and immutable DMA
 * descriptors already retained by the native owner. It reserves seven unique
 * tokens from that graph's permanent allocator; no new port/GLINK owner.
 * The exact Port.PullOwner excludes another Pull or graph until Ended.
 * Session, Graph/Storage/Port and provider remain alive through Ended and all
 * native callback/work rundown. This owner never accesses or frees CPU/DMA
 * memory. The caller latches possible remote ownership before exposing the
 * descriptors here and retains local mappings until Ended.
 * No transport call occurs in Init. Input/output objects are disjoint. */
CR_STATUS CrAudioPullInit(CR_AUDIO_PULL_SESSION *,CR_AUDIO_GRAPH_SESSION *,
    CONST CR_AUDIO_PULL_BINDING *,CONST CR_AUDIO_PULL_MEMORY *,UINT64 TimeoutUs);
/* Receive is bounded copy only; same immutable source epoch is required.
 * Tick is bounded, processes at most one mailbox and sends at most one packet.
 * Pending accepted/unknown commands never retry. Timeout, basic MAP ACK/error,
 * source loss and graph CLOSE never independently retire a possible MAP. */
CR_STATUS CrAudioPullReceive(CR_AUDIO_PULL_SESSION *,CONST CR_AUDIO_GRAPH_EPOCH *,CONST VOID *,UINT32);
CR_STATUS CrAudioPullTick(CR_AUDIO_PULL_SESSION *,UINT64 NowUs);
CR_STATUS CrAudioPullClose(CR_AUDIO_PULL_SESSION *);
/* Close allows reverse exact UNMAP only after this actual graph is Ended with
 * no possible graph/command/cookie. Unknown MAP without a handle stays retained
 * for a late exact reply while source remains current. Each UNMAP acknowledges
 * only its own remote handle. Ended grants no local HAL/CPU-view retirement.
 * Source loss permanently retains possible remote maps; there is deliberately
 * no externally supplied Retired Boolean or epoch-change release shortcut. */
