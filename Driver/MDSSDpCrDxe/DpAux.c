/** AUX FIFO and EDID discovery adapted from Linux msm/dp/dp_aux.c.
    Copyright (c) 2012-2020, The Linux Foundation.
    SPDX-License-Identifier: GPL-2.0-only */
#include "DpLink.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define AUX_IRQ (BIT3 | BIT6 | BIT9 | BIT12 | BIT15 | BIT18 | BIT21 | BIT24 | BIT27)
#define AUX_FATAL (BIT6 | BIT12 | BIT15 | BIT18 | BIT24 | BIT27)

EFI_STATUS CrDpWait(UINTN Address, UINT32 Mask, UINT32 Expected, UINT32 TimeoutUs)
{
  UINT32 Elapsed;
  for (Elapsed = 0; Elapsed <= TimeoutUs; Elapsed += 50) {
    if ((MmioRead32(Address) & Mask) == Expected) return EFI_SUCCESS;
    if (Elapsed != TimeoutUs) MicroSecondDelay(50);
  }
  DEBUG((DEBUG_ERROR, "MDSSDpCr: timeout reg=%lx value=%08x mask=%08x want=%08x\n",
         Address, MmioRead32(Address), Mask, Expected));
  return EFI_TIMEOUT;
}

STATIC VOID ClearInterrupts(VOID)
{
  UINT32 Pending = MmioRead32(CR_DP_AHB + 0x20) & AUX_IRQ;
  MmioWrite32(CR_DP_AHB + 0x20, (Pending << 1) | (AUX_IRQ << 2));
  MmioRead32(CR_DP_AUX + 0xBC);
  MmioWrite32(CR_DP_AUX + 0x4C, 0x1F);
  MmioWrite32(CR_DP_AUX + 0x4C, 0x9F);
  MmioWrite32(CR_DP_AUX + 0x4C, 0);
}
STATIC VOID AuxReset(VOID)
{
  MmioOr32(CR_DP_AUX + 0x30, BIT1);
  MicroSecondDelay(1000);
  MmioAnd32(CR_DP_AUX + 0x30, (UINT32)~BIT1);
}
VOID CrDpAuxEnable(VOID)
{
  AuxReset();
  MmioWrite32(CR_DP_AUX + 0x3C, 0xFFFF);
  MmioWrite32(CR_DP_AUX + 0x40, 0xFFFF);
  ClearInterrupts();
  MmioWrite32(CR_DP_AUX + 0x30, BIT0);
}
VOID CrDpAuxDisable(VOID)
{
  MmioWrite32(CR_DP_AUX + 0x38, 0);
  MmioWrite32(CR_DP_AUX + 0x30, 0);
  MmioWrite32(CR_DP_AHB + 0x20, 0);
}

EFI_STATUS CrDpAuxTransfer(BOOLEAN Native, BOOLEAN Read, UINT32 Address,
                          UINT8 *Buffer, UINTN Size)
{
  UINT8 Header[4];
  UINTN I;
  UINT32 Attempt, Interrupt, Elapsed, Control;
  EFI_STATUS Status = EFI_TIMEOUT;
  if (Buffer == NULL || Size == 0 || Size > (Native ? 16U : 124U) ||
      Address > (Native ? 0xFFFFFU : 0x7FU)) return EFI_INVALID_PARAMETER;
  Header[0] = (UINT8)((Address >> 16) | (Read ? BIT4 : 0));
  Header[1] = (UINT8)(Address >> 8); Header[2] = (UINT8)Address;
  Header[3] = (UINT8)(Size - 1);
  /* The Qualcomm I2C path uses the same NO_SEND_ADDR convention as Linux.
     Writes retain the bus; each EDID read sends STOP, so offsets are sent
     again before every read chunk for adapters that reset their pointer. */
  Control = BIT9 | (Native ? 0 : BIT8 | BIT10 | (Read ? 0 : BIT11));
  for (Attempt = 0; Attempt < 7; ++Attempt) {
    ClearInterrupts();
    for (I = 0; I < 4 + (Read ? 0 : Size); ++I) {
      UINT32 Byte = I < 4 ? Header[I] : Buffer[I - 4];
      MmioWrite32(CR_DP_AUX + 0x34, (Byte << 8) | (I == 0 ? BIT31 : 0));
    }
    MmioWrite32(CR_DP_AUX + 0x38, 0);
    MemoryFence();
    MmioWrite32(CR_DP_AUX + 0x38, Control);
    Interrupt = 0;
    for (Elapsed = 0; Elapsed < 25000; Elapsed += 50) {
      Interrupt = MmioRead32(CR_DP_AHB + 0x20) & AUX_IRQ;
      if (Interrupt != 0) break;
      MicroSecondDelay(50);
    }
    MmioWrite32(CR_DP_AHB + 0x20, (Interrupt << 1) | (AUX_IRQ << 2));
    MmioWrite32(CR_DP_AUX + 0x38, Control & ~(UINT32)BIT9);
    if (Interrupt == 0 || (Interrupt & BIT9) != 0) {
      Status = EFI_TIMEOUT;
      AuxReset();
    } else if ((Interrupt & AUX_FATAL) != 0 ||
               ((Interrupt & (BIT21 | BIT3)) == (BIT21 | BIT3))) {
      Status = EFI_DEVICE_ERROR;
      break;
    } else if ((Interrupt & BIT21) != 0) {
      Status = EFI_NOT_READY;
      MicroSecondDelay(400);
    } else if ((Interrupt & BIT3) != 0) {
      if (Read) {
        MmioWrite32(CR_DP_AUX + 0x34, BIT31 | BIT0);
        MmioRead32(CR_DP_AUX + 0x34); /* reply header */
        for (I = 0; I < Size; ++I) {
          UINT32 Data = MmioRead32(CR_DP_AUX + 0x34);
          if (((Data >> 16) & 0xFF) != I) return EFI_PROTOCOL_ERROR;
          Buffer[I] = (UINT8)(Data >> 8);
        }
      }
      return EFI_SUCCESS;
    }
  }
  DEBUG((DEBUG_ERROR, "MDSSDpCr: AUX %a %a addr=%x len=%u irq=%08x: %r\n",
         Native ? "native" : "I2C", Read ? "read" : "write", Address,
         (UINT32)Size, Interrupt, Status));
  return Status;
}

EFI_STATUS CrDpReadEdid(UINT8 Block, UINT8 *Data)
{
  UINT8 Segment = Block / 2, Offset;
  UINTN I;
  UINT8 Sum = 0;
  EFI_STATUS Status;
  if (Data == NULL) return EFI_INVALID_PARAMETER;
  for (I = 0; I < 128; I += 16) {
    if (Segment != 0) {
      Status = CrDpAuxTransfer(FALSE, FALSE, 0x30, &Segment, 1);
      if (EFI_ERROR(Status)) return Status;
    }
    Offset = (UINT8)((Block % 2) * 128 + I);
    Status = CrDpAuxTransfer(FALSE, FALSE, 0x50, &Offset, 1);
    if (EFI_ERROR(Status)) return Status;
    Status = CrDpAuxTransfer(FALSE, TRUE, 0x50, Data + I, 16);
    if (EFI_ERROR(Status)) return Status;
  }
  for (I = 0; I < 128; ++I) Sum = (UINT8)(Sum + Data[I]);
  return Sum == 0 ? EFI_SUCCESS : EFI_CRC_ERROR;
}

EFI_STATUS CrDpServiceIrq(VOID)
{
  UINT8 Vector, Nak = 2;
  EFI_STATUS Status = CrDpAuxTransfer(TRUE, TRUE, 0x201, &Vector, 1);
  if (EFI_ERROR(Status)) return Status;
  /* This SST RGB source cannot execute compliance-test or HDCP requests.
     Reject the automated test explicitly before clearing its W1C vector. */
  if ((Vector & BIT1) != 0) {
    Status = CrDpAuxTransfer(TRUE, FALSE, 0x260, &Nak, 1);
    if (EFI_ERROR(Status)) return Status;
  }
  if (Vector != 0) Status = CrDpAuxTransfer(TRUE, FALSE, 0x201, &Vector, 1);
  return Status;
}

STATIC BOOLEAN Dtd(CONST UINT8 *D, UINT32 Width, UINT32 Height, CR_DP_TIMING *T)
{
  UINT32 HBlank, VBlank;
  if ((D[0] | D[1]) == 0 || (D[17] & 0x80) != 0 || (D[17] & 0x18) != 0x18)
    return FALSE; /* progressive, separate digital sync only */
  T->Width = D[2] | ((D[4] & 0xF0) << 4);
  T->Height = D[5] | ((D[7] & 0xF0) << 4);
  if (T->Width != Width || T->Height != Height) return FALSE;
  HBlank = D[3] | ((D[4] & 15) << 8); VBlank = D[6] | ((D[7] & 15) << 8);
  T->HFront = D[8] | ((D[11] & 0xC0) << 2);
  T->HSync = D[9] | ((D[11] & 0x30) << 4);
  T->VFront = (D[10] >> 4) | ((D[11] & 0xC) << 2);
  T->VSync = (D[10] & 15) | ((D[11] & 3) << 4);
  if (T->HSync == 0 || T->VSync == 0 || HBlank <= T->HFront + T->HSync ||
      VBlank <= T->VFront + T->VSync) return FALSE;
  T->HBack = (UINT16)(HBlank - T->HFront - T->HSync);
  T->VBack = (UINT16)(VBlank - T->VFront - T->VSync);
  T->PixelHz = (D[0] | (D[1] << 8)) * 10000U;
  T->HNegative = (D[17] & BIT1) == 0; T->VNegative = (D[17] & BIT2) == 0;
  return T->PixelHz <= 200000000;
}

EFI_STATUS CrDpFindMirrorTiming(CONST UINT8 *Edid, UINTN Size,
                               UINT32 Width, UINT32 Height, CR_DP_TIMING *Timing)
{
  STATIC CONST UINT8 Header[] = {0,255,255,255,255,255,255,0};
  UINTN Block, I;
  UINT8 Sum;
  if (Edid == NULL || Timing == NULL || Size < 128 || Size % 128 != 0 ||
      Width == 0 || Height == 0) return EFI_INVALID_PARAMETER;
  ZeroMem(Timing, sizeof(*Timing));
  if (CompareMem(Edid, Header, sizeof(Header)) != 0 || Edid[18] != 1)
    return EFI_PROTOCOL_ERROR;
  for (Block = 0; Block < Size / 128; ++Block) {
    Sum = 0;
    for (I = 0; I < 128; ++I) Sum = (UINT8)(Sum + Edid[Block * 128 + I]);
    if (Sum != 0) return EFI_CRC_ERROR;
  }
  for (I = 54; I + 18 <= 126; I += 18)
    if (Dtd(Edid + I, Width, Height, Timing)) return EFI_SUCCESS;
  for (Block = 1; Block < Size / 128; ++Block) {
    CONST UINT8 *E = Edid + Block * 128;
    UINTN End = E[2];
    if (E[0] != 2 || End == 0) continue;
    if (End < 4 || End > 127) return EFI_PROTOCOL_ERROR;
    for (I = 4; I < End;) {
      UINTN Length = E[I] & 31, J;
      if (I + Length + 1 > End) return EFI_PROTOCOL_ERROR;
      if ((E[I] >> 5) == 2 && Width == 1920 && Height == 1080)
        for (J = 1; J <= Length; ++J) if ((E[I + J] & 0x7F) == 16) {
          *Timing = (CR_DP_TIMING){148500000,1920,1080,88,44,148,4,5,36,FALSE,FALSE};
          return EFI_SUCCESS;
        }
      I += Length + 1;
    }
    for (I = End; I + 18 <= 127; I += 18)
      if (Dtd(E + I, Width, Height, Timing)) return EFI_SUCCESS;
  }
  ZeroMem(Timing, sizeof(*Timing));
  return EFI_UNSUPPORTED;
}
