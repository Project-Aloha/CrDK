/* Scripted host-only SMEM/APM peer. Not a production authority. BSD-3-Clause */
#pragma once
#include <Library/AudioGraphCrLib.h>
#include <ucsi/Glink.h>

#define AUDIO_PEER_FIFO 16384U
typedef struct {
  CR_GLINK Link;
  CR_GLINK_SERVICE Service;
  CR_GLINK_SMEM Smem;
  UINT32 Descriptor[8];
  UINT8 Tx[AUDIO_PEER_FIFO], Rx[AUDIO_PEER_FIFO];
  UINT8 IntentStorage[20*512], SendStorage[65536], Wire[65536], SavedReply[512];
  CR_AUDIO_GRAPH_SESSION *Session;
  CR_AUDIO_GRAPH_EPOCH Epoch;
  CR_AUDIO_GRAPH_BINDING Binding;
  CR_AUDIO_GRAPH_CLAIM Claim;
  UINT64 Cookie;
  UINT32 WireBytes, Fragments, Intent, Sent[17], Attempts[17], Received, Pmic, Qrtr;
  UINT32 Runtime;
  UINT32 OpenCount, CloseCount, Graphs, ReplyBytes, MaxPacket, Handshake;
  UINT32 FreezeAfterFragments;
  UINT32 DropPhase, ErrorPhase, DspError, RejectPhase, UnknownPhase;
  UINT32 OverridePhase, OverrideAcceptance, CloseDspError;
  CR_STATUS OverrideStatus;
  BOOLEAN SyncReply, LoseDuringSend, LoseDuringAdmit, RetireTestGeneration;
  BOOLEAN ReleaseBusy, AdmitFailure, AdmitRetained, InCallback, CloseFailed;
} AUDIO_GRAPH_PEER;

/* Everything in this file describes simulated device behavior only. Authority
 * Admit binds the exact claim and checks this peer's empty graph inventory.
 * Tests may retire that exact simulated generation; no real device callback
 * implements this contract yet. */
VOID AudioPeerInit(AUDIO_GRAPH_PEER *, UINT32 Handshake);
VOID AudioPeerAttach(AUDIO_GRAPH_PEER *, CR_AUDIO_GRAPH_SESSION *);
VOID AudioPeerProgress(AUDIO_GRAPH_PEER *);
VOID AudioPeerDetach(AUDIO_GRAPH_PEER *);
VOID AudioPeerLateReply(AUDIO_GRAPH_PEER *);
VOID AudioPeerUnrelated(AUDIO_GRAPH_PEER *, UINT32 Intent);
VOID AudioPeerResetGraph(AUDIO_GRAPH_PEER *);
UINT32 AudioPeerWord(CONST UINT8 *);
VOID AudioPeerPut(UINT8 *, UINT32);
