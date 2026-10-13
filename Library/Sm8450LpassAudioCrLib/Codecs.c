/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors.
 * HDK topology and variant decoding: OnePlus audio-kernel 877203a7cf9b7997,
 * wcd938x.c/.h and wsa883x.c. See README.md and LICENSE. */
#include <Library/Sm8450LpassAudioCrLib.h>

static BOOLEAN Valid(CONST CR_SM8450_AUDIO_CODECS *C) {
  return C && C->Self == C;
}
static CR_STATUS Fail(CR_SM8450_AUDIO_CODECS *C, CR_STATUS S) {
  if (C->Error == CR_SUCCESS) { C->Error = S; }
  C->Phase = CrAudioCodecFaulted;
  for (UINT32 I = 0; I < 4; ++I) { C->Id[I].Identified = FALSE; C->Id[I].PaOffVerified = FALSE; }
  return S;
}
CR_STATUS CrSm8450AudioCodecsInit(CR_SM8450_AUDIO_CODECS *C, CR_SW_CORE *Rx,
                                  CR_SW_CORE *Tx, CR_SW_CORE *Wsa) {
  if (!C || C->Self || !Rx || !Tx || !Wsa || Rx == Tx || Rx == Wsa || Tx == Wsa ||
      Rx->Self != Rx || Tx->Self != Tx || Wsa->Self != Wsa ||
      Rx->Target != &CrSoundWireSm8450Rx || Tx->Target != &CrSoundWireSm8450Tx ||
      Wsa->Target != &CrSoundWireSm8450Wsa || Rx->State != CrSwStopped ||
      Tx->State != CrSwStopped || Wsa->State != CrSwStopped) { return CR_INVALID_PARAMETER; }
  *C = (CR_SM8450_AUDIO_CODECS){.Self = C, .Cores = {Rx, Tx, Wsa}};
  return CR_SUCCESS;
}
CR_STATUS CrSm8450AudioCodecsStart(CR_SM8450_AUDIO_CODECS *C) {
  if (!Valid(C)) { return CR_INVALID_PARAMETER; }
  if (C->Phase != CrAudioCodecStopped) { return CR_BUSY; }
  C->Error = CR_SUCCESS; C->Device = C->RegisterStep = 0;
  C->Request = (CR_SW_REQUEST){0}; C->Pending = C->Write = FALSE;
  for (UINT32 I = 0; I < 4; ++I) { C->Id[I] = (CR_AUDIO_CODEC_ID){0}; }
  C->Phase = CrAudioCodecStarting;
  for (UINT32 I = 0; I < 3; ++I) {
    C->Started[I] = TRUE; /* journal before the first possible resource effect */
    CR_STATUS S = CrSoundWireStart(C->Cores[I]);
    if (S != CR_SUCCESS && S != CR_NOT_READY) { return Fail(C, S); }
  }
  return CR_NOT_READY;
}
static CR_STATUS Enumerate(CR_SM8450_AUDIO_CODECS *C) {
  for (UINT32 I = 0; I < 4; ++I) {
    UINT32 Core = I < 2 ? I : 2, Index = I == 3 ? 1 : 0;
    CR_SW_SLAVE S;
    CR_STATUS R = CrSoundWireGetSlave(C->Cores[Core], Index, &S);
    if (R != CR_SUCCESS) { return R; }
    C->Id[I] = (CR_AUDIO_CODEC_ID){.DeviceId = S.DeviceId, .ControllerEpoch = S.Epoch,
      .Enumeration = S.Enumeration, .LogicalNumber = S.LogicalNumber};
  }
  /* RX has no register map; its immutable enumerated ID is its inventory. */
  C->Id[0].Identified = TRUE; C->Device = 1; C->RegisterStep = 0;
  C->Phase = CrAudioCodecIdentifying;
  return CR_SUCCESS;
}
static CR_STATUS Identify(CR_SM8450_AUDIO_CODECS *C) {
  UINT32 D = C->Device, N = D == 3 ? 1 : 0;
  CR_SW_CORE *S = C->Cores[D < 2 ? D : 2];
  CR_AUDIO_CODEC_ID *Id = &C->Id[D];
  CONST CR_SW_SLAVE_TARGET *T = &S->Target->Slaves[N];
  if (C->Pending) {
    CR_SW_RESULT R;
    CR_STATUS Status = CrSoundWireTake(S, &C->Request, C->RequestSerial, &R);
    if (Status == CR_NOT_READY) { return Status; }
    if (Status != CR_SUCCESS) { return Fail(C, Status); }
    C->Pending = FALSE;
    if (R.Status != CR_SUCCESS || R.Kind != (UINT32)(C->Write ? CrSwWriteDrained : CrSwReadValue)) {
      return Fail(C, R.Status == CR_SUCCESS ? CR_DEVICE_ERROR : R.Status);
    }
    C->Request = (CR_SW_REQUEST){0};
    if (C->Write) {
      /* A FIFO-drained write is not a PA-off acknowledgement. Read it back. */
      C->Write = FALSE; C->RegisterStep = 4;
    } else if (C->RegisterStep == 0) {
      Id->ChipId = R.Value; C->RegisterStep = 1;
    } else if (C->RegisterStep == 1) {
      Id->VariantRaw = R.Value;
      Id->Variant = D == 1 ? (UINT8)((R.Value & 0x1eU) >> 1) : (UINT8)(R.Value & 0xfU);
      if ((D == 1 && Id->Variant != 5) || (D != 1 && Id->Variant != 1 && Id->Variant != 5)) {
        return Fail(C, CR_UNSUPPORTED);
      }
      C->RegisterStep = D == 1 ? 5 : 2;
    } else {
      Id->PaControl = R.Value;
      if (R.Value & 1U) {
        if (C->RegisterStep == 4) { return Fail(C, CR_DEVICE_ERROR); }
        C->RegisterStep = 3;
      } else {
        Id->PaOffVerified = TRUE; C->RegisterStep = 5;
      }
    }
  }
  if (C->RegisterStep == 5) {
    Id->Identified = TRUE;
    if (++C->Device == 4) { C->Phase = CrAudioCodecIdentifiedMuted; return CR_SUCCESS; }
    C->RegisterStep = 0; return CR_NOT_READY;
  }
  CR_SW_SLAVE Slave;
  CR_STATUS Status = CrSoundWireGetSlave(S, N, &Slave);
  if (Status != CR_SUCCESS) { return Fail(C, Status); }
  if (Slave.DeviceId != Id->DeviceId || Slave.Epoch != Id->ControllerEpoch ||
      Slave.Enumeration != Id->Enumeration || Slave.LogicalNumber != Id->LogicalNumber) {
    return Fail(C, CR_DEVICE_ERROR);
  }
  UINT16 Register = C->RegisterStep == 0 ? T->ChipIdRegister :
                    C->RegisterStep == 1 ? T->VariantRegister : T->PaControlRegister;
  C->Write = C->RegisterStep == 3;
  Status = C->Write ? CrSoundWireWriteByte(S, &Slave, Register, (UINT8)(Id->PaControl & ~1U), &C->Request) :
                     CrSoundWireReadByte(S, &Slave, Register, &C->Request);
  if (Status != CR_SUCCESS && Status != CR_NOT_READY) { return Fail(C, Status); }
  C->RequestSerial = C->Request.Serial; C->Pending = TRUE;
  return CR_NOT_READY;
}
CR_STATUS CrSm8450AudioCodecsStep(CR_SM8450_AUDIO_CODECS *C) {
  if (!Valid(C)) { return CR_INVALID_PARAMETER; }
  if (C->Phase == CrAudioCodecStopped) { return CR_NOT_READY; }
  if (C->Phase == CrAudioCodecStopping) { return CrSm8450AudioCodecsStop(C); }
  if (C->Phase == CrAudioCodecFaulted) { return C->Error; }
  BOOLEAN Ready = TRUE;
  for (UINT32 I = 0; I < 3; ++I) {
    CR_STATUS S = CrSoundWireStep(C->Cores[I]);
    if (S != CR_SUCCESS && S != CR_NOT_READY) { return Fail(C, S); }
    if (C->Cores[I]->State != CrSwReady) { Ready = FALSE; }
  }
  if (C->Phase == CrAudioCodecStarting) {
    BOOLEAN Both = C->Cores[0]->State == CrSwBusReady && C->Cores[1]->State == CrSwBusReady;
    for (UINT32 I = 0; I < 3; ++I) {
      if (C->Cores[I]->State != CrSwBusReady || (I < 2 && !Both)) { continue; }
      CR_STATUS S = CrSoundWireReleaseSlaves(C->Cores[I]);
      if (S != CR_SUCCESS && S != CR_NOT_READY && S != CR_BUSY) { return Fail(C, S); }
    }
    if (!Ready) { return CR_NOT_READY; }
    CR_STATUS S = Enumerate(C); if (S != CR_SUCCESS) { return Fail(C, S); }
  }
  if (C->Phase == CrAudioCodecIdentifying) { return Identify(C); }
  if (C->Phase == CrAudioCodecIdentifiedMuted) {
    for (UINT32 I = 0; I < 4; ++I) {
      CR_SW_SLAVE S; UINT32 N = I < 2 ? I : 2;
      CR_STATUS R = CrSoundWireGetSlave(C->Cores[N], I == 3 ? 1 : 0, &S);
      if (R != CR_SUCCESS || S.Epoch != C->Id[I].ControllerEpoch ||
          S.Enumeration != C->Id[I].Enumeration || S.DeviceId != C->Id[I].DeviceId ||
          S.LogicalNumber != C->Id[I].LogicalNumber) { return Fail(C, CR_DEVICE_ERROR); }
    }
  }
  return C->Phase == CrAudioCodecIdentifiedMuted ? CR_SUCCESS : CR_NOT_READY;
}
CR_STATUS CrSm8450AudioCodecsStop(CR_SM8450_AUDIO_CODECS *C) {
  if (!Valid(C)) { return CR_INVALID_PARAMETER; }
  if (C->Phase == CrAudioCodecStopped) { return CR_SUCCESS; }
  C->Phase = CrAudioCodecStopping;
  for (UINT32 I = 0; I < 4; ++I) { C->Id[I].Identified = FALSE; C->Id[I].PaOffVerified = FALSE; }
  CR_STATUS Result = CR_SUCCESS;
  for (UINT32 I = 3; I != 0; --I) {
    UINT32 N = I - 1;
    if (!C->Started[N]) { continue; }
    CR_STATUS S = CrSoundWireStop(C->Cores[N]);
    if (S == CR_SUCCESS && C->Cores[N]->Request) {
      /* Stop retires the hardware operation but the consumer must still take
       * its exact result before reusing request storage or starting again. */
      CR_SW_RESULT R;
      S = CrSoundWireTake(C->Cores[N], &C->Request, C->RequestSerial, &R);
      if (S == CR_SUCCESS) { C->Pending = FALSE; }
    }
    if (S == CR_SUCCESS) { C->Started[N] = FALSE; }
    else if (Result == CR_SUCCESS) { Result = S; }
  }
  if (Result == CR_SUCCESS) {
    C->Phase = CrAudioCodecStopped; C->Pending = C->Write = FALSE;
    C->Request = (CR_SW_REQUEST){0};
  }
  return Result;
}
