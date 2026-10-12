/* Actual board consumer + actual SoundWire engine; source-derived MMIO model.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main OriginalSoundWireSuite
#include "SoundWireCrTest.c"
#undef main
#include <Library/Sm8450LpassAudioCrLib.h>

static CR_STATUS TickCodecs(CR_SM8450_AUDIO_CODECS *C, MODEL **M, unsigned Mode) {
  for (unsigned I = 0; I < 3; ++I) {
    M[I]->Now += 1000;
    /* Real reset modeled by the existing model populates identity registers.
       Board-specific OTP values here are hardware input, never readiness. */
    if (I == 2 && M[I]->C.State == CrSwWaitSlaves) {
      M[I]->Codec[1][WSA883X_OTP_REG_0] = Mode == 3 ? 2 : 1;
      M[I]->Codec[2][WSA883X_OTP_REG_0] = 5;
      M[I]->Codec[1][WSA883X_PA_FSM_CTL] = Mode == 1 || Mode == 2 ? 0x81 : 0x80;
      M[I]->Codec[2][WSA883X_PA_FSM_CTL] = 0x80;
    }
  }
  return CrSm8450AudioCodecsStep(C);
}
int main(void) {
  for (unsigned Mode = 0; Mode < 10; ++Mode) {
    MODEL *M[3] = {New(&CrSoundWireSm8450Rx), New(&CrSoundWireSm8450Tx), New(&CrSoundWireSm8450Wsa)};
    M[0]->Sibling = M[1]; M[1]->Sibling = M[0];
    CR_SM8450_AUDIO_CODECS C = {0};
    CHECK(CrSm8450AudioCodecsInit(&C, &M[0]->C, &M[1]->C, &M[2]->C) == CR_SUCCESS);
    if (Mode == 2) { M[2]->NoWriteEffect = TRUE; }
    if (Mode == 4) { M[1]->Rate = 600000; }
    if (Mode == 5) { M[2]->PartialRegister = TRUE; }
    CR_STATUS S = CrSm8450AudioCodecsStart(&C);
    if (Mode < 4 || Mode > 5) {
      CHECK(S == CR_NOT_READY);
      for (unsigned I = 0; I < 150 && C.Phase != CrAudioCodecIdentifiedMuted && C.Phase != CrAudioCodecFaulted; ++I) {
        S = TickCodecs(&C, M, Mode);
        if (Mode == 8 && C.Pending) { break; }
      }
      if (Mode == 8) { CHECK(C.Pending && C.Request.Owner != NULL); }
      else if (Mode == 2 || Mode == 3) {
        CHECK(C.Phase == CrAudioCodecFaulted && C.Error != CR_SUCCESS);
        CHECK(!C.Id[0].Identified && !C.Id[2].PaOffVerified);
      } else {
        CHECK(S == CR_SUCCESS && C.Phase == CrAudioCodecIdentifiedMuted);
        CHECK(C.Id[0].Identified && C.Id[1].Variant == 5 && C.Id[2].Variant == 1 && C.Id[3].Variant == 5);
        CHECK(C.Id[2].PaOffVerified && C.Id[3].PaOffVerified);
        CHECK(C.Id[2].PaControl == 0x80 && C.Id[3].PaControl == 0x80);
        if (Mode == 6) {
          M[1]->Reg[SWRM_INTERRUPT_STATUS / 4] |= SWRM_INTERRUPT_STATUS_CHANGE_ENUM_SLAVE_STATUS;
          M[1]->Irq.Handler(M[1]->Irq.Param);
          CHECK(CrSm8450AudioCodecsStep(&C) == CR_DEVICE_ERROR && !C.Id[1].Identified);
        }
      }
    } else { CHECK(C.Phase == CrAudioCodecFaulted && S != CR_SUCCESS); }
    if (Mode == 7) { M[2]->ReleaseFail = TRUE; }
    S = CrSm8450AudioCodecsStop(&C);
    if (Mode != 4) { CHECK(S != CR_SUCCESS); } /* partial preflight never opened sibling capture */
    if (Mode == 7) {
      CHECK(C.Phase == CrAudioCodecStopping && C.Started[2] && M[2]->Acquired);
      M[2]->ReleaseFail = FALSE;
    }
    for (unsigned I = 0; I < 3 && S != CR_SUCCESS; ++I) { S = CrSm8450AudioCodecsStop(&C); }
    CHECK(S == CR_SUCCESS && C.Phase == CrAudioCodecStopped);
    CHECK(C.Request.Owner == NULL && !C.Pending);
    for (unsigned I = 0; I < 3; ++I) { CHECK(M[I]->C.Request == NULL); }
    if (Mode == 8 || Mode == 9) {
      CHECK(CrSm8450AudioCodecsStart(&C) == CR_NOT_READY);
      for (unsigned I = 0; I < 150 && C.Phase != CrAudioCodecIdentifiedMuted; ++I) {
        S = TickCodecs(&C, M, 0); CHECK(S == CR_SUCCESS || S == CR_NOT_READY);
      }
      CHECK(C.Phase == CrAudioCodecIdentifiedMuted);
      S = CrSm8450AudioCodecsStop(&C);
      for (unsigned I = 0; I < 3 && S != CR_SUCCESS; ++I) { S = CrSm8450AudioCodecsStop(&C); }
      CHECK(S == CR_SUCCESS);
    }
    for (unsigned I = 0; I < 3; ++I) {
      CHECK(!M[I]->Registered && !M[I]->Acquired && !M[I]->Scheduled);
      free(M[I]);
    }
  }
  printf("Actual HDK codec consumer: 10 cases, %u checks PASS\n", Checks);
  return 0;
}
