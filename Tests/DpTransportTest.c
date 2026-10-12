/** Independent AUX FIFO, sink and DPU register fixtures. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../Driver/MDSSDpCrDxe/DpLink.h"
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>

STATIC UINT32 mAhb[16], mAux[64], mLink[32], mP0[0x80 / 4], mDpu[0x50000 / 4];
#define mP0Config mP0[0x14 / 4]
STATIC UINTN mAuxSnapshotReads;
STATIC UINT8 mDpcd[0x3000], mEdid[384], mFifo[128];
STATIC UINTN mFifoCount, mRxIndex, mTransactions, mResetCount, mAckCount;
STATIC UINT8 mSegment, mOffset;
STATIC UINT32 mFault, mFaultTransactions, mStatusReads, mTrainingReads;
STATIC BOOLEAN mBadIndex, mTrainFail, mSinkLost, mStopFail, mResetFail, mFrames;
STATIC BOOLEAN mMissingEq,mMissingAlign;
STATIC UINTN mLevelCalls;
STATIC UINTN mElapsed;
STATIC UINTN mClockSnapshotReads;

VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { (VOID)Level; return TRUE; }
VOID EFIAPI MemoryFence(VOID) {}
UINTN EFIAPI MicroSecondDelay(UINTN Delay) { mElapsed += Delay; return Delay; }
VOID *EFIAPI ZeroMem(VOID *Buffer, UINTN Size) { return memset(Buffer, 0, Size); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN Size) { return memcmp(A, B, Size); }

STATIC VOID Transaction(UINT32 Control)
{
  UINT32 Address = ((mFifo[0] & 15) << 16) | (mFifo[1] << 8) | mFifo[2];
  UINT32 Length = mFifo[3] + 1;
  BOOLEAN Read = (mFifo[0] & 16) != 0;
  ++mTransactions;
  assert(mFifoCount == 4U + (Read ? 0 : Length));
  assert((Control & 0xFFF) == (BIT9 | ((Control & BIT8) ? BIT8 | BIT10 | (Read ? 0 : BIT11) : 0)));
  mAhb[8] = mFaultTransactions ? mFault : BIT3;
  if (mFaultTransactions) { --mFaultTransactions; return; }
  if (Control & BIT8) {
    assert(Address == 0x30 || Address == 0x50);
    if (!Read) {
      assert(Length == 1);
      if (Address == 0x30) mSegment = mFifo[4]; else mOffset = mFifo[4];
    }
  } else {
    assert(Address + Length <= sizeof(mDpcd));
    if (!Read) memcpy(mDpcd + Address, mFifo + 4, Length);
    if (Read && Address == 0x202) {
      ++mTrainingReads;
      BOOLEAN Lost = mTrainFail || (mSinkLost && mDpcd[0x102] == 0);
      mDpcd[0x202] = mDpcd[0x203] = Lost ? 0 : mDpcd[0x102] == 0x21 ? 0x11 : 0x77;
      mDpcd[0x204] = Lost ? 0 : 1;
      if(mDpcd[0x102]==0){
        if(mMissingEq)mDpcd[0x202]&=(UINT8)~BIT1;
        if(mMissingAlign)mDpcd[0x204]=0;
      }
      mDpcd[0x206] = mDpcd[0x207] = mTrainFail ? 0 : 0x55;
    }
  }
}
UINT32 EFIAPI MmioRead32(UINTN Address)
{
  if (Address >= 0x0AE90000 && Address < 0x0AE90040) return mAhb[(Address - 0x0AE90000) / 4];
  if (Address >= 0x0AE90200 && Address < 0x0AE90300) {
    if (Address == 0x0AE90234 && (mAux[0x34/4] & BIT0)) {
      if (mRxIndex++ == 0) return 0; /* response header */
      UINT32 Index = (UINT32)mRxIndex - 2;
      UINT32 A = ((mFifo[0] & 15) << 16) | (mFifo[1] << 8) | mFifo[2];
      UINT8 Byte = (mAux[0x38/4] & BIT8) ? mEdid[mSegment * 256 + mOffset + Index] : mDpcd[A + Index];
      return ((mBadIndex ? Index + 1 : Index) << 16) | (Byte << 8);
    }
    return mAux[(Address - 0x0AE90200) / 4];
  }
  if (Address >= 0x0AE90400 && Address < 0x0AE90480) {
    if (Address == 0x0AE90440) return mLink[1] == BIT7 ? 1 : mLink[1] << 3;
    return mLink[(Address - 0x0AE90400)/4];
  }
  if (Address >= CR_DP_P0 && Address < CR_DP_P0 + 0x80) {
    UINTN O = Address - CR_DP_P0;
    assert(O == 0 || O == 0x10 || O == 0x14 || O == 0x60 || O == 0x64 || O == 0x7C);
    return mP0[O / 4];
  }
  if (Address >= 0x0AEB0000 && Address < 0x0AEB0300) {
    UINTN O = Address - 0x0AEB0000;
    assert(O == 0x200 || O == 0x204 || O == 0xB4 || O == 0x160 || O == 0x190 || O == 0x194);
    ++mAuxSnapshotReads;
    return 0; /* read-only VBIF ownership evidence, never a policy write */
  }
  if (Address >= 0x0AF00000 && Address < 0x0AF20000) {
    /* Only the DPTX0 registers described by dispcc-sm8450.c are diagnostic
       inputs. A display snapshot must not read unrelated clock owners. */
    assert(Address == 0x0AF0819C || Address == 0x0AF081A0 || Address == 0x0AF081B4 ||
           Address == 0x0AF081BC || Address == 0x0AF081C0 || Address == 0x0AF081C4 ||
           Address == 0x0AF081C8 || Address == 0x0AF081CC || Address == 0x0AF08040 ||
           Address == 0x0AF08048 || Address == 0x0AF08050);
    ++mClockSnapshotReads;
    return 0;
  }
  assert(Address >= 0x0AE01000 && Address < 0x0AE51000);
  /* Diagnostics may only read our owned route, never the live DSI banks. */
  UINTN O = Address - 0x0AE01000;
  assert((O >= 0x26000 && O < 0x26400) || (O >= 0x16000 && O < 0x16200) ||
         (O >= 0x46000 && O < 0x46400) || (O >= 0x34000 && O < 0x34300) ||
         O == 0x2B4 || O == 0x460 || O == 0x14 || O == 0x360 || O == 0x364);
  if (Address == 0x0AE3526C) return mStopFail ? 1 : mDpu[0x34000 / 4];
  if (Address == 0x0AE350AC && mFrames) return ++mStatusReads;
  if (Address == 0x0AE17030 && !mResetFail) return 0;
  if (Address == 0x0AE17018 && mFrames) return 0;
  return mDpu[(Address - 0x0AE01000) / 4];
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  if (Address >= 0x0AE90000 && Address < 0x0AE90040) {
    if (Address == 0x0AE90020) {
      ++mAckCount;
      assert((Value & ~(UINT32)((BIT3|BIT6|BIT9|BIT12|BIT15|BIT18|BIT21|BIT24|BIT27) << 1)) ==
             ((BIT3|BIT6|BIT9|BIT12|BIT15|BIT18|BIT21|BIT24|BIT27) << 2) || Value == 0);
      mAhb[8] &= ~(Value >> 1);
    } else mAhb[(Address - 0x0AE90000) / 4] = Value;
  } else if (Address >= 0x0AE90200 && Address < 0x0AE90300) {
    mAux[(Address - 0x0AE90200) / 4] = Value;
    if (Address == 0x0AE90230 && (Value & BIT1)) ++mResetCount;
    if (Address == 0x0AE90234) {
      if (Value & BIT31) mFifoCount = mRxIndex = 0;
      if (!(Value & BIT0)) { assert(mFifoCount < sizeof(mFifo)); mFifo[mFifoCount++] = (UINT8)(Value >> 8); }
    }
    if (Address == 0x0AE90238 && (Value & BIT9)) Transaction(Value);
  } else if (Address >= 0x0AE90400 && Address < 0x0AE90480)
    mLink[(Address - 0x0AE90400)/4] = Value;
  else if (Address >= CR_DP_P0 && Address < CR_DP_P0 + 0x80) {
    UINTN O = Address - CR_DP_P0;
    assert(O == 0 || O == 0x10 || O == 0x14 || O == 0x60 || O == 0x7C);
    mP0[O / 4] = Value;
  }
  else {
    assert(Address >= 0x0AE01000 && Address < 0x0AE51000);
    /* A DP mirror may never write the live DSI DMA/CTL/LM/INTF banks. */
    UINTN O = Address - 0x0AE01000;
    assert(O != 0x1612C && O != 0x16130 && O != 0x160C0); /* unsupported onDPU8.1 */
    assert((O >= 0x26000 && O < 0x26400) || (O >= 0x16000 && O < 0x16200) ||
           (O >= 0x46000 && O < 0x46400) || (O >= 0x34000 && O < 0x34300) || O == 0x2B4 || O == 0x460 || O == 0x18);
    if (O == 0x18) {
      assert(Value == BIT24); /* only DP INTF0's underrun W1C */
      mDpu[0x14/4] &= ~Value;
    }
    mDpu[O / 4] = Value;
  }
  return Value;
}
UINT32 EFIAPI MmioOr32(UINTN A, UINT32 V) { return MmioWrite32(A, MmioRead32(A) | V); }
UINT32 EFIAPI MmioAnd32(UINTN A, UINT32 V) { return MmioWrite32(A, MmioRead32(A) & V); }
UINT32 EFIAPI MmioAndThenOr32(UINTN A, UINT32 M, UINT32 V) { return MmioWrite32(A, (MmioRead32(A) & M) | V); }
STATIC EFI_STATUS EFIAPI Levels(EFI_USB_DP_PHY_CR_PROTOCOL *This, UINT8 V, UINT8 P)
{ (VOID)This; assert(V + P <= 3); ++mLevelCalls; return EFI_SUCCESS; }
STATIC VOID Checksum(UINT8 *E)
{ UINT8 Sum = 0; UINTN I; E[127] = 0; for (I = 0; I < 127; ++I) Sum += E[I]; E[127] = 0 - Sum; }

int main(void)
{
  UINT8 B[128];
  CrDpAuxEnable();
  mDpcd[0x1234] = 0xAC;
  assert(CrDpAuxTransfer(TRUE, TRUE, 0x1234, B, 1) == EFI_SUCCESS && B[0] == 0xAC);
  assert(mFifo[0] == 0x10 && mFifo[1] == 0x12 && mFifo[2] == 0x34 && mFifo[3] == 0);
  assert(CrDpAuxTransfer(TRUE, TRUE, 0x100000, B, 1) == EFI_INVALID_PARAMETER);
  assert(CrDpAuxTransfer(TRUE, TRUE, 0, B, 17) == EFI_INVALID_PARAMETER);
  mBadIndex = TRUE;
  assert(CrDpAuxTransfer(TRUE, TRUE, 0, B, 1) == EFI_PROTOCOL_ERROR);
  mBadIndex = FALSE;
  mFault = BIT21; mFaultTransactions = 2;
  UINTN Count = mTransactions;
  assert(CrDpAuxTransfer(FALSE, TRUE, 0x50, B, 1) == EFI_SUCCESS && mTransactions == Count + 3);
  mFault = BIT3 | BIT21; mFaultTransactions = 1;
  assert(CrDpAuxTransfer(FALSE, TRUE, 0x50, B, 1) == EFI_DEVICE_ERROR);
  mFault = BIT12; mFaultTransactions = 1;
  assert(CrDpAuxTransfer(TRUE, TRUE, 0, B, 1) == EFI_DEVICE_ERROR);
  mFault = BIT9; mFaultTransactions = 7;
  UINTN Resets = mResetCount;
  assert(CrDpAuxTransfer(TRUE, TRUE, 0, B, 1) == EFI_TIMEOUT && mResetCount == Resets + 7);
  for (UINTN I = 0; I < sizeof(mEdid); ++I) mEdid[I] = (UINT8)I;
  Checksum(mEdid + 256);
  assert(CrDpReadEdid(2, B) == EFI_SUCCESS && memcmp(B, mEdid + 256, 128) == 0);
  mSegment = 0; Checksum(mEdid);
  assert(CrDpReadEdid(0, B) == EFI_SUCCESS);
  ++mEdid[42]; assert(CrDpReadEdid(0, B) == EFI_CRC_ERROR);
  memset(mEdid, 0, sizeof(mEdid));
  memcpy(mEdid, (UINT8[]){0,255,255,255,255,255,255,0}, 8);
  mEdid[18] = 1; mEdid[126] = 1;
  /* Independent CTA SVD fixture for 1080p60, without any DTD. */
  mEdid[128] = 2; mEdid[129] = 3; mEdid[130] = 6; mEdid[132] = 0x41; mEdid[133] = 16;
  Checksum(mEdid); Checksum(mEdid + 128);
  CR_DP_TIMING T;
  assert(CrDpFindMirrorTiming(mEdid, 256, 1920, 1080, &T) == EFI_SUCCESS);
  assert(T.PixelHz == 148500000 && T.HBack == 148 && T.VBack == 36);
  assert(CrDpFindMirrorTiming(mEdid, 256, 1080, 1920, &T) == EFI_UNSUPPORTED);
  mEdid[132] = 0x5F; Checksum(mEdid + 128);
  assert(CrDpFindMirrorTiming(mEdid, 256, 1920, 1080, &T) == EFI_PROTOCOL_ERROR);
  mEdid[132] = 0x41; Checksum(mEdid + 128);
  assert(CrDpFindMirrorTiming(mEdid, 256, 1920, 1080, &T) == EFI_SUCCESS);
  CR_DP_TU Tu;
  assert(CrDpCalculateTu(&T, 2700, 2, &Tu) == EFI_SUCCESS && Tu.Size == 40 && Tu.Valid == 33 && Tu.Delay == 15);
  assert(CrDpCalculateTu(&T, 1620, 4, &Tu) == EFI_SUCCESS && Tu.Size == 32 && Tu.Valid == 22 && Tu.Delay == 15);
  assert(CrDpCalculateTu(&T, 5400, 1, &Tu) == EFI_SUCCESS && Tu.Size == 40 && Tu.Valid == 33);
  assert(CrDpCalculateTu(&T, 1620, 2, &Tu) == EFI_UNSUPPORTED);
  T.PixelHz = 148350000; /* 1080p59.94 DTD: Linux RGB legacy TU, without moderation. */
  assert(CrDpCalculateTu(&T, 2700, 2, &Tu) == EFI_SUCCESS && Tu.Size == 57 && Tu.Valid == 47 && Tu.Delay == 20);
  T.HFront = T.HSync = T.HBack = 1;
  assert(CrDpCalculateTu(&T, 2700, 2, &Tu) == EFI_UNSUPPORTED);
  T = (CR_DP_TIMING){148500000,1920,1080,88,44,148,4,5,36,FALSE,FALSE};
  UINT8 Caps[16] = {0x14, 0x14, 0xC4, 0x80};
  EFI_USB_DP_PHY_CR_PROTOCOL Phy = {.SetDpLevels = Levels};
  assert(CrDpTrain(&Phy, Caps, 2700, 2) == EFI_SUCCESS);
  assert(mDpcd[0x100] == 10 && mDpcd[0x101] == 0x82 && mDpcd[0x102] == 0 && mTrainingReads == 2);
  assert(mLink[8/4] == 0x4157);
  mTrainFail = TRUE;
  assert(CrDpTrain(&Phy, Caps, 2700, 2) == EFI_TIMEOUT && mTrainingReads < 30);
  assert(mDpcd[0x102] == 0);
  mDpcd[0x201] = 0x12;
  assert(CrDpServiceIrq() == EFI_SUCCESS && mDpcd[0x260] == 2); /* compliance NAK */
  assert(CrDpCalculateTu(&T, 2700, 2, &Tu) == EFI_SUCCESS);
  mP0Config = 0xA5100031; /* wide-bus + unrelated inherited interface fields */
  mP0[0] = 1; mP0[0x10 / 4] = 0xABCD0001; mP0[0x60 / 4] = 0x100; mP0[0x7C / 4] = 0xFFFFFFFF;
  CrDpConfigureStream(&T, 2700, &Tu);
  assert(mP0Config == 0xA5100021);
  assert(mP0[0] == 0 && mP0[0x60 / 4] == 0 && mP0[0x10 / 4] == 0xABCD0000);
  assert(mP0[0x7C / 4] == 0);
  CrDpConfigureStream(&T, 2700, &Tu);
  assert(mP0Config == 0xA5100021); /* repeated configuration also preserves them */
  assert(mLink[0x1C/4] == ((1125U << 16) | 2200));
  assert(mLink[0x30/4] == ((15U << 16) | 33) && mLink[0x4C/4] == 39);
  mDpu[0x460/4] = 0xABC000;
  mDpu[0x26070/4] = 0xABC0000F; /* inherited secure address routes */
  mDpu[0x46000/4] = 0xFFFFFFFF; /* inherited split + unrelated blend stages */
  mDpu[0x14/4] = BIT24 | BIT26; /* old DP + live DSI underrun latches */
  CrDpStartScanout(&T, 0x40000000, 7680);
  assert(mDpu[0x26030/4] == 0x236FF && mDpu[0x26034/4] == 0x03020001);
  assert(mDpu[0x16008/4] == (BIT24 | BIT21));
  assert(mDpu[0x46000/4] == (BIT30 | BIT0)); /* only solo BASE alpha output */
  assert(mDpu[0x3401C/4] == 90392 && mDpu[0x34024/4] == 2466111);
  assert(mDpu[0x460/4] == 0xABC041);
  assert(mDpu[0x26070/4] == 0xABC00000 && mDpu[0x14/4] == BIT26);
  /* 1080p VSW+VBP=41 already covers SM8450's 24-line fetch latency. */
  assert(mDpu[0x34004/4] == (BIT29 | BIT30) && mDpu[0x34170/4] == 0);
  CR_DP_TIMING ShortPorch = T;
  ShortPorch.VSync = 1; ShortPorch.VBack = 3; ShortPorch.VFront = 4;
  CrDpStartScanout(&ShortPorch, 0x40000000, 7680);
  assert(mDpu[0x34004/4] == (BIT29 | BIT30 | BIT31));
  /* All four VFP lines can prefetch, starting after the last active line. */
  assert(mDpu[0x34170/4] == (1084U * 2200 + 1));
  ShortPorch.VFront = 0;
  CrDpStartScanout(&ShortPorch, 0x40000000, 7680);
  assert(mDpu[0x34004/4] == (BIT29 | BIT30) && mDpu[0x34170/4] == 0);
  CrDpStartScanout(&T, 0x40000000, 7680);
  mTrainFail = FALSE; mFrames = TRUE;
  Count = mClockSnapshotReads;
  assert(CrDpWaitScanout() == EFI_SUCCESS);
  assert(mClockSnapshotReads == Count + 11); /* exactly the safe live clock snapshot */
  assert(mAuxSnapshotReads == 6);
  UINT32 Routing = mDpu[0x16008 / 4]; UINTN Waited = mElapsed, Transactions = mTransactions;
  CrDpSnapshotScanout();
  assert(mAuxSnapshotReads == 12 && mElapsed == Waited && mTransactions == Transactions &&
         mDpu[0x16008 / 4] == Routing); /* no waits/AUX/route mutation */
  assert(CrDpObserveLink()==EFI_SUCCESS&&mTransactions == Transactions + 1); /* exact six-byteDPCD observation */
  mMissingEq=TRUE;assert(CrDpObserveLink()==EFI_DEVICE_ERROR);mMissingEq=FALSE;
  mMissingAlign=TRUE;assert(CrDpObserveLink()==EFI_DEVICE_ERROR);mMissingAlign=FALSE;
  mFault=BIT12;mFaultTransactions=1;assert(CrDpObserveLink()==EFI_DEVICE_ERROR);
  assert(CrDpObserveLink()==EFI_SUCCESS);
  /* A timing generator may progress even though pixels underflow or the
     trained link was lost when switching to video. Neither is success. */
  mDpu[0x14/4] |= BIT24;
  Count = mTransactions;
  assert(CrDpWaitScanout() == EFI_DEVICE_ERROR && mTransactions == Count);
  CrDpStartScanout(&T, 0x40000000, 7680);
  mSinkLost = TRUE;
  assert(CrDpWaitScanout() == EFI_DEVICE_ERROR);
  mSinkLost = FALSE; mFault = BIT12; mFaultTransactions = 1;
  assert(CrDpWaitScanout() == EFI_DEVICE_ERROR);
  mFrames = FALSE; Count = mElapsed;
  assert(CrDpWaitScanout() == EFI_TIMEOUT && mElapsed - Count == 250000);
  mFrames = TRUE;
  mStopFail = TRUE; assert(CrDpStopScanout() == EFI_TIMEOUT && mDpu[0x16030/4] == 0);
  mStopFail = FALSE; mResetFail = TRUE; assert(CrDpStopScanout() == EFI_TIMEOUT);
  mResetFail = FALSE; assert(CrDpStopScanout() == EFI_SUCCESS);
  CrDpAuxDisable();
  puts("DP: AUX/EDID, bounded training/TU, preserved P0 fields, solo BASE LM2, nonsecure DMA1, latency-based prefetch and owned-route/clock diagnostic checks passed");
  return 0;
}
