/* AUX FIFO/EDID extracted from MDSSDpCrDxe/DpAux.c and Linux msm/dp/dp_aux.c.
 * Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <Library/MdssDpCrLib.h>
#include <oskal/cr_string.h>

#define B(N) ((UINT32)1U << (N))
#define AUX_STATUS (B(3)|B(6)|B(9)|B(12)|B(15)|B(18)|B(21)|B(24)|B(27))
#define AUX_FATAL (B(6)|B(12)|B(15)|B(18)|B(24)|B(27))
#define GO B(9)
#define ENABLE B(0)
#define RESET B(1)

static CR_STATUS Exact(CR_STATUS Status)
{
  return Status == CR_SUCCESS || CR_ERROR(Status) ? Status : CR_NOT_READY;
}

static BOOLEAN Range(CONST VOID *Pointer, UINTN Bytes)
{
  return Pointer && Bytes && (UINTN)Pointer <= ~(UINTN)0 - Bytes;
}

static BOOLEAN Overlap(CONST VOID *Left, UINTN LeftBytes, CONST VOID *Right, UINTN RightBytes)
{
  return !Range(Left, LeftBytes) || !Range(Right, RightBytes) ||
    ((UINTN)Left < (UINTN)Right + RightBytes && (UINTN)Right < (UINTN)Left + LeftBytes);
}

static CR_STATUS Owner(CR_MDSS_DP_AUX *Aux, CONST VOID *StableOwner)
{
  if (!Aux || !Aux->Initialized || !StableOwner) return CR_INVALID_PARAMETER;
  return Aux->Owner == StableOwner ? CR_SUCCESS : CR_BUSY;
}

static CR_STATUS Ready(CR_MDSS_DP_AUX *Aux)
{
  return Exact(Aux->Io.Ready(Aux->Io.Context, Aux->Owner));
}

static CR_STATUS Read32(CR_MDSS_DP_AUX *Aux, CR_MDSS_DP_WINDOW Window,
                        UINT32 Offset, UINT32 *Value)
{
  CR_STATUS Status = Ready(Aux);
  UINT32 Copy;
  if (Status != CR_SUCCESS) return Status;
  if ((UINT32)Window >= CrMdssDpWindowCount || (Offset & 3) ||
      Offset > Aux->Io.View[Window].Bytes - sizeof(UINT32)) return CR_INVALID_PARAMETER;
  Status = Exact(Aux->Io.Read32(Aux->Io.Context, Window, Offset, &Copy));
  if (Status == CR_SUCCESS) *Value = Copy;
  return Status;
}

static CR_STATUS Write32(CR_MDSS_DP_AUX *Aux, CR_MDSS_DP_WINDOW Window,
                         UINT32 Offset, UINT32 Value)
{
  CR_STATUS Status = Ready(Aux);
  if (Status != CR_SUCCESS) return Status;
  if ((UINT32)Window >= CrMdssDpWindowCount || (Offset & 3) ||
      Offset > Aux->Io.View[Window].Bytes - sizeof(UINT32)) return CR_INVALID_PARAMETER;
  Aux->Touched = TRUE;
  Aux->Stopped = FALSE;
  Aux->CleanupPending = TRUE;
  return Exact(Aux->Io.Write32(Aux->Io.Context, Window, Offset, Value));
}

static CR_STATUS Fence(CR_MDSS_DP_AUX *Aux)
{
  CR_STATUS Status = Ready(Aux);
  return Status == CR_SUCCESS ? Exact(Aux->Io.Fence(Aux->Io.Context)) : Status;
}

static CR_STATUS Delay(CR_MDSS_DP_AUX *Aux, UINT32 Microseconds)
{
  CR_STATUS Status = Ready(Aux);
  return Status == CR_SUCCESS ? Exact(Aux->Io.DelayUs(Aux->Io.Context, Microseconds)) : Status;
}

static CR_STATUS Verify(CR_MDSS_DP_AUX *Aux, CR_MDSS_DP_WINDOW Window,
                        UINT32 Offset, UINT32 Mask, UINT32 Expected)
{
  UINT32 Value;
  CR_STATUS Status = Fence(Aux);
  if (Status != CR_SUCCESS) return Status;
  Status = Read32(Aux, Window, Offset, &Value);
  return Status != CR_SUCCESS ? Status : (Value & Mask) == Expected ? CR_SUCCESS : CR_NOT_READY;
}

static CR_STATUS ResetAux(CR_MDSS_DP_AUX *Aux)
{
  UINT32 Control;
  CR_STATUS Status = Read32(Aux, CrMdssDpAux, 0x30, &Control);
  if (Status != CR_SUCCESS) return Status;
  Status = Write32(Aux, CrMdssDpAux, 0x30, Control | RESET);
  if (Status != CR_SUCCESS) return Status;
  Status = Verify(Aux, CrMdssDpAux, 0x30, RESET, RESET);
  if (Status != CR_SUCCESS) return Status;
  Status = Delay(Aux, 1000);
  if (Status != CR_SUCCESS) return Status;
  Status = Write32(Aux, CrMdssDpAux, 0x30, Control & ~RESET);
  if (Status != CR_SUCCESS) return Status;
  return Verify(Aux, CrMdssDpAux, 0x30, RESET, 0);
}

static CR_STATUS Clear(CR_MDSS_DP_AUX *Aux, BOOLEAN Masks)
{
  UINT32 Interrupt;
  CR_STATUS Status = Read32(Aux, CrMdssDpAhb, 0x20, &Interrupt);
  if (Status != CR_SUCCESS) return Status;
  /* Packed status/ack/mask lanes. Preserve HPD's independent mask (bit2),
     never acknowledge its status. All AUX masks are owned by this caller. */
  UINT32 Value = (Interrupt & B(2)) | ((Interrupt & AUX_STATUS) << 1) |
    (Masks ? AUX_STATUS << 2 : 0);
  Status = Write32(Aux, CrMdssDpAhb, 0x20, Value);
  if (Status != CR_SUCCESS) return Status;
  Status = Verify(Aux, CrMdssDpAhb, 0x20, AUX_STATUS | (AUX_STATUS << 2),
                   Masks ? AUX_STATUS << 2 : 0);
  if (Status != CR_SUCCESS) return Status;
  Status = Ready(Aux);
  if (Status != CR_SUCCESS) return Status;
  return Exact(Aux->Io.ClearPhyInterrupts(Aux->Io.Context, Aux->Owner));
}

static CR_STATUS Current(CR_MDSS_DP_AUX *Aux)
{
  CR_STATUS Status = Verify(Aux, CrMdssDpAux, 0x30, ENABLE | RESET, ENABLE);
  if (Status != CR_SUCCESS) return Status;
  return Verify(Aux, CrMdssDpAux, 0x38, GO, 0);
}

CR_STATUS CrMdssDpAuxInit(CR_MDSS_DP_AUX *Aux, CONST CR_MDSS_DP_AUX_IO *Io,
                         CONST VOID *StableOwner)
{
  if (!Aux || !Io || !StableOwner || !Io->Context || !Io->Ready || !Io->Read32 ||
      !Io->Write32 || !Io->Fence || !Io->DelayUs || !Io->ClearPhyInterrupts ||
      Overlap(Aux, sizeof(*Aux), Io, sizeof(*Io))) return CR_INVALID_PARAMETER;
  if ((Io->ArmKick || Io->Wait || Io->Disarm) &&
      (!Io->ArmKick || !Io->Wait || !Io->Disarm)) return CR_INVALID_PARAMETER;
  if (Io->Soc != 0x8450 || Io->View[CrMdssDpAhb].Physical != CR_MDSS_DP_AHB_PHYSICAL ||
      Io->View[CrMdssDpAux].Physical != CR_MDSS_DP_AUX_PHYSICAL) return CR_UNSUPPORTED;
  for (UINT32 I = 0; I < CrMdssDpWindowCount; ++I)
    if (Io->View[I].Bytes != CR_MDSS_DP_VIEW_BYTES) return CR_BAD_BUFFER_SIZE;
  if (Aux->Initialized) return CR_BUSY;
  cr_memset(Aux, 0, sizeof(*Aux));
  Aux->Io = *Io;
  Aux->Owner = StableOwner;
  Aux->Initialized = TRUE;
  return CR_SUCCESS;
}

CR_STATUS CrMdssDpAuxEnable(CR_MDSS_DP_AUX *Aux, CONST VOID *StableOwner)
{
  CR_STATUS Status = Owner(Aux, StableOwner);
  if (Status != CR_SUCCESS) return Status;
  if (Aux->CleanupPending) return CR_NOT_READY;
  if (Aux->Enabled) return Current(Aux);
  Status = ResetAux(Aux);
  if (Status != CR_SUCCESS) goto Done;
  Status = Write32(Aux, CrMdssDpAux, 0x3c, 0xffff);
  if (Status != CR_SUCCESS) goto Done;
  Status = Verify(Aux, CrMdssDpAux, 0x3c, CR_MAX_UINT32, 0xffff);
  if (Status != CR_SUCCESS) goto Done;
  Status = Write32(Aux, CrMdssDpAux, 0x40, 0xffff);
  if (Status != CR_SUCCESS) goto Done;
  Status = Verify(Aux, CrMdssDpAux, 0x40, CR_MAX_UINT32, 0xffff);
  if (Status != CR_SUCCESS) goto Done;
  Status = Write32(Aux, CrMdssDpAux, 0x38, 0);
  if (Status != CR_SUCCESS) goto Done;
  Status = Clear(Aux, Aux->Io.ArmKick == NULL);
  if (Status != CR_SUCCESS) goto Done;
  Status = Write32(Aux, CrMdssDpAux, 0x30, ENABLE);
  if (Status != CR_SUCCESS) goto Done;
  Status = Current(Aux);
  if (Status != CR_SUCCESS) goto Done;
  Status = Ready(Aux);
  if (Status == CR_SUCCESS) {
    Aux->Enabled = TRUE;
    Aux->CleanupPending = FALSE;
  }
Done:
  Aux->LastStatus = Status;
  return Status;
}

CR_STATUS CrMdssDpAuxTransfer(CR_MDSS_DP_AUX *Aux, CONST VOID *StableOwner,
  BOOLEAN Native, BOOLEAN Read, UINT32 Address, UINT8 *Buffer, UINTN Bytes)
{
  CR_STATUS Status = Owner(Aux, StableOwner);
  UINT8 Packet[128], Reply[CR_MDSS_DP_AUX_MAX_BYTES];
  UINT32 Control, Interrupt = 0;
  if (Status != CR_SUCCESS) return Status;
  if ((Native != FALSE && Native != TRUE) || (Read != FALSE && Read != TRUE) ||
      !Range(Buffer, Bytes) || Bytes > (Native ? CR_MDSS_DP_AUX_NATIVE_MAX_BYTES : CR_MDSS_DP_AUX_MAX_BYTES) ||
      Address > (Native ? 0xfffffU : 0x7fU) ||
      (Native && Bytes - 1 > 0xfffffU - Address) || Overlap(Aux, sizeof(*Aux), Buffer, Bytes))
    return CR_INVALID_PARAMETER;
  if (!Aux->Enabled || Aux->CleanupPending) return CR_NOT_READY;
  Packet[0] = (UINT8)((Address >> 16) | (Read ? B(4) : 0));
  Packet[1] = (UINT8)(Address >> 8);
  Packet[2] = (UINT8)Address;
  Packet[3] = (UINT8)(Bytes - 1);
  if (!Read) cr_memcpy(Packet + 4, Buffer, Bytes);
  Control = GO | (Native ? 0 : B(8) | B(10) | (Read ? 0 : B(11)));
  Aux->Attempts = 0;
  for (UINT32 Attempt = 0; Attempt < 7; ++Attempt) {
    Status = Current(Aux);
    if (Status != CR_SUCCESS) goto Done;
    Status = Clear(Aux, Aux->Io.ArmKick == NULL);
    if (Status != CR_SUCCESS) goto Done;
    Aux->Attempts = Attempt + 1;
    for (UINTN I = 0; I < 4 + (Read ? 0 : Bytes); ++I) {
      Status = Write32(Aux, CrMdssDpAux, 0x34, ((UINT32)Packet[I] << 8) | (I ? 0 : B(31)));
      if (Status != CR_SUCCESS) goto Done;
    }
    Status = Write32(Aux, CrMdssDpAux, 0x38, 0);
    if (Status != CR_SUCCESS) goto Done;
    Status = Verify(Aux, CrMdssDpAux, 0x38, CR_MAX_UINT32, 0);
    if (Status != CR_SUCCESS) goto Done;
    Interrupt = 0;
    if (Aux->Io.ArmKick) {
      /* The fixed provider journals any committed generation independently
         even if its output remains unchanged on a nonfinal completion. */
      Aux->CleanupPending = TRUE; Aux->Stopped = FALSE;
      Status = Exact(Aux->Io.ArmKick(Aux->Io.Context, Aux->Owner, Control,
                                   &Aux->IrqGeneration));
      if (Status != CR_SUCCESS) goto Done;
      if (!Aux->IrqGeneration) { Status = CR_NOT_READY; goto Done; }
      Status = Exact(Aux->Io.Wait(Aux->Io.Context, Aux->Owner,
                                Aux->IrqGeneration, 25000, &Interrupt));
      if (Status != CR_SUCCESS && Status != CR_TIMEOUT) goto Done;
      if (Status == CR_TIMEOUT) Interrupt = 0;
      if (Interrupt & ~AUX_STATUS) { Status = CR_COMPROMISED_DATA; goto Done; }
    } else {
      Status = Write32(Aux, CrMdssDpAux, 0x38, Control);
      if (Status != CR_SUCCESS) goto Done;
      Status = Fence(Aux);
      if (Status != CR_SUCCESS) goto Done;
      for (UINT32 Elapsed = 0; Elapsed < 25000; Elapsed += 50) {
        Status = Read32(Aux, CrMdssDpAhb, 0x20, &Interrupt);
        if (Status != CR_SUCCESS) goto Done;
        Interrupt &= AUX_STATUS;
        if (Interrupt) break;
        Status = Delay(Aux, 50);
        if (Status != CR_SUCCESS) goto Done;
      }
    }
    Aux->LastInterrupt = Interrupt;
    Status = Write32(Aux, CrMdssDpAux, 0x38, Control & ~GO);
    if (Status != CR_SUCCESS) goto Done;
    Status = Verify(Aux, CrMdssDpAux, 0x38, GO, 0);
    if (Status != CR_SUCCESS) goto Done;
    if (Aux->Io.Disarm) {
      Status = Exact(Aux->Io.Disarm(Aux->Io.Context, Aux->Owner, Aux->IrqGeneration));
      if (Status != CR_SUCCESS) goto Done;
      Aux->IrqGeneration = 0;
    }
    Status = Clear(Aux, Aux->Io.ArmKick == NULL);
    if (Status != CR_SUCCESS) goto Done;
    if (!Interrupt || (Interrupt & B(9))) {
      Status = ResetAux(Aux);
      if (Status != CR_SUCCESS) goto Done;
      Status = Current(Aux);
      if (Status != CR_SUCCESS) goto Done;
      Status = CR_TIMEOUT;
    } else if ((Interrupt & AUX_FATAL) || ((Interrupt & (B(21) | B(3))) == (B(21) | B(3)))) {
      Status = CR_DEVICE_ERROR;
    } else if (Interrupt & B(21)) {
      Status = Delay(Aux, 400);
      if (Status != CR_SUCCESS) goto Done;
      Status = CR_NOT_READY;
    } else if (Interrupt & B(3)) {
      if (Read) {
        UINT32 Value;
        Status = Write32(Aux, CrMdssDpAux, 0x34, B(31) | B(0));
        if (Status != CR_SUCCESS) goto Done;
        Status = Fence(Aux);
        if (Status != CR_SUCCESS) goto Done;
        Status = Read32(Aux, CrMdssDpAux, 0x34, &Value); /* reply header */
        if (Status != CR_SUCCESS) goto Done;
        for (UINTN I = 0; I < Bytes; ++I) {
          Status = Read32(Aux, CrMdssDpAux, 0x34, &Value);
          if (Status != CR_SUCCESS) goto Done;
          if (((Value >> 16) & 0xff) != I) { Status = CR_COMPROMISED_DATA; goto Clean; }
          Reply[I] = (UINT8)(Value >> 8);
        }
      }
      Status = Current(Aux);
      if (Status != CR_SUCCESS) goto Done;
      Status = Ready(Aux);
      if (Status != CR_SUCCESS) goto Done;
      if (Read) cr_memcpy(Buffer, Reply, Bytes);
      Status = CR_SUCCESS;
Clean:
      Aux->CleanupPending = FALSE;
      goto Done;
    }
    Aux->CleanupPending = FALSE; /* completed GO-off/reset or terminal reply */
    if (Status == CR_DEVICE_ERROR) break;
  }
Done:
  Aux->LastStatus = Status;
  return Status;
}

CR_STATUS CrMdssDpAuxReadEdid(CR_MDSS_DP_AUX *Aux, CONST VOID *StableOwner,
  UINT8 Block, UINT8 *Data, UINTN DataBytes)
{
  CR_STATUS Status = Owner(Aux, StableOwner);
  UINT8 Copy[CR_MDSS_DP_EDID_BYTES], Segment = Block / 2, Offset, Sum = 0;
  if (Status != CR_SUCCESS) return Status;
  if (!Range(Data, DataBytes) || DataBytes < sizeof(Copy) ||
      Overlap(Aux, sizeof(*Aux), Data, DataBytes)) return CR_INVALID_PARAMETER;
  for (UINTN I = 0; I < sizeof(Copy); I += 16) {
    if (Segment) {
      Status = CrMdssDpAuxTransfer(Aux, StableOwner, FALSE, FALSE, 0x30, &Segment, 1);
      if (Status != CR_SUCCESS) return Status;
    }
    Offset = (UINT8)((Block % 2) * 128 + I);
    Status = CrMdssDpAuxTransfer(Aux, StableOwner, FALSE, FALSE, 0x50, &Offset, 1);
    if (Status != CR_SUCCESS) return Status;
    Status = CrMdssDpAuxTransfer(Aux, StableOwner, FALSE, TRUE, 0x50, Copy + I, 16);
    if (Status != CR_SUCCESS) return Status;
  }
  for (UINTN I = 0; I < sizeof(Copy); ++I) Sum = (UINT8)(Sum + Copy[I]);
  if (Sum) return CR_COMPROMISED_DATA;
  Status = Ready(Aux);
  if (Status == CR_SUCCESS) cr_memcpy(Data, Copy, sizeof(Copy));
  return Status;
}

CR_STATUS CrMdssDpAuxServiceIrq(CR_MDSS_DP_AUX *Aux, CONST VOID *StableOwner)
{
  UINT8 Vector, Nak = 2;
  CR_STATUS Status = CrMdssDpAuxTransfer(Aux, StableOwner, TRUE, TRUE, 0x201, &Vector, 1);
  if (Status != CR_SUCCESS) return Status;
  if (Vector & B(1)) {
    Status = CrMdssDpAuxTransfer(Aux, StableOwner, TRUE, FALSE, 0x260, &Nak, 1);
    if (Status != CR_SUCCESS) return Status;
  }
  return Vector ? CrMdssDpAuxTransfer(Aux, StableOwner, TRUE, FALSE, 0x201, &Vector, 1) : CR_SUCCESS;
}

CR_STATUS CrMdssDpAuxStop(CR_MDSS_DP_AUX *Aux, CONST VOID *StableOwner)
{
  CR_STATUS Status = Owner(Aux, StableOwner);
  if (Status != CR_SUCCESS) return Status;
  Status = Write32(Aux, CrMdssDpAux, 0x38, 0);
  if (Status != CR_SUCCESS) goto Done;
  Status = Verify(Aux, CrMdssDpAux, 0x38, GO, 0);
  if (Status != CR_SUCCESS) goto Done;
  Status = ResetAux(Aux);
  if (Status != CR_SUCCESS) goto Done;
  if (Aux->Io.Disarm) {
    Status = Exact(Aux->Io.Disarm(Aux->Io.Context, Aux->Owner, Aux->IrqGeneration));
    if (Status != CR_SUCCESS) goto Done;
    Aux->IrqGeneration = 0;
  }
  Status = Clear(Aux, FALSE);
  if (Status != CR_SUCCESS) goto Done;
  Status = Write32(Aux, CrMdssDpAux, 0x30, 0);
  if (Status != CR_SUCCESS) goto Done;
  Status = Verify(Aux, CrMdssDpAux, 0x30, ENABLE | RESET, 0);
  if (Status != CR_SUCCESS) goto Done;
  Status = Verify(Aux, CrMdssDpAux, 0x38, GO, 0);
  if (Status != CR_SUCCESS) goto Done;
  Status = Verify(Aux, CrMdssDpAhb, 0x20, AUX_STATUS << 2, 0);
  if (Status != CR_SUCCESS) goto Done;
  Status = Ready(Aux);
  if (Status == CR_SUCCESS) {
    Aux->Enabled = Aux->Touched = Aux->CleanupPending = FALSE;
    Aux->Stopped = TRUE;
  }
Done:
  Aux->LastStatus = Status;
  return Status;
}

CR_STATUS CrMdssDpAuxUninit(CR_MDSS_DP_AUX *Aux, CONST VOID *StableOwner)
{
  CR_STATUS Status = Owner(Aux, StableOwner);
  if (Status != CR_SUCCESS) return Status;
  if (Aux->Enabled || Aux->Touched || Aux->CleanupPending) return CR_BUSY;
  cr_memset(Aux, 0, sizeof(*Aux));
  return CR_SUCCESS;
}
