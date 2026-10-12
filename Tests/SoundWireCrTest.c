/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors. */
#include <Library/SoundWireCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Original model uses the actual pinned Android register definitions. */
#include <swr-mstr-registers.h>
#include <wcd938x-registers.h>
/* Upstream codec headers use the same unrelated unscoped access enum names. */
#define REG_NO_ACCESS WsaNoAccess
#define RD_REG WsaReadAccess
#define WR_REG WsaWriteAccess
#define RD_WR_REG WsaReadWriteAccess
#include <wsa883x-registers.h>
#undef REG_NO_ACCESS
#undef RD_REG
#undef WR_REG
#undef RD_WR_REG

static unsigned Checks;
#define CHECK(X) do { ++Checks; if (!(X)) { fprintf(stderr,"%d: %s\n",__LINE__,#X); exit(1); } } while(0)

#ifndef _KERNEL_MODE
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P, UINT32 Compare, UINT32 Exchange)
{
  __atomic_compare_exchange_n(P, &Compare, Exchange, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return Compare;
}
#endif

typedef struct MODEL MODEL;
struct MODEL {
  CR_SW_CORE C;
  CR_INTERRUPT_CONFIG Irq;
  CR_SW_IO Io;
  UINT32 Reg[0x2000 / 4];
  UINT8 Codec[16][0x3600];
  UINT64 Now, Next, CompleteAt;
  UINT32 PendingRead, PendingWrite, ReturnWord, Rate;
  UINT32 Reads, Writes, Sends, Acquires, Releases, Quiesces, Resets, Schedules;
  UINT32 FailWrite, FailRead;
  BOOLEAN Acquired, Registered, Scheduled, Reset, Quiet, NoReply, WrongId;
  BOOLEAN PartialRegister, BadRegisterSuccess, UnregisterFail, QuiesceFail;
  BOOLEAN CancelFail, ReleaseFail, ClearOnReleaseFail, EmptyAcquireSuccess, PartialAcquire;
  BOOLEAN ResetFail, ScheduleFail, EffectBeforeFail, NoWriteEffect, LinkFail, InterruptInSend;
  MODEL *Sibling;
};

static MODEL *FromIrq(CR_INTERRUPT_CONFIG *Irq)
{
  CHECK(Irq != NULL && Irq->Param != NULL);
  CR_SW_CORE *C = Irq->Param;
  MODEL *M = C->Io.Context;
  CHECK(&M->Irq == Irq && C == &M->C);
  return M;
}

CR_STATUS CrRegisterInterrupt(CR_INTERRUPT_CONFIG *Irq)
{
  MODEL *M = FromIrq(Irq);
  CHECK(!M->Registered && M->Acquired && Irq->Handler != NULL);
  M->Registered = !M->BadRegisterSuccess;
  return M->PartialRegister ? CR_DEVICE_ERROR : CR_SUCCESS;
}

CR_STATUS CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *Irq)
{
  MODEL *M = FromIrq(Irq);
  if (M->UnregisterFail) { return CR_BUSY; }
  M->Registered = FALSE;
  return CR_SUCCESS;
}

BOOLEAN CrInterruptIsRegistered(CR_INTERRUPT_CONFIG *Irq)
{
  return FromIrq(Irq)->Registered;
}

static void Lease(MODEL *M, CONST CR_SW_LEASE *L)
{
  CHECK(M->Acquired && L == &M->C.Lease && L->Provider == M && L->Token == M->Acquires);
}

static CR_STATUS Acquire(VOID *Context, CONST VOID *Owner, CONST CR_SW_TARGET *T, CR_SW_LEASE *L)
{
  MODEL *M = Context;
  CHECK(Owner == &M->C && T == M->C.Target && L == &M->C.Lease && !M->Acquired);
  ++M->Acquires;
  if (M->EmptyAcquireSuccess) { return CR_SUCCESS; }
  M->Acquired = TRUE; M->Quiet = FALSE;
  L->Provider = M; L->Token = M->Acquires;
  return M->PartialAcquire ? CR_DEVICE_ERROR : CR_SUCCESS;
}

static CR_STATUS Rate(VOID *Context, CONST CR_SW_LEASE *L, UINT32 *Hz)
{
  MODEL *M = Context; Lease(M, L); *Hz = M->Rate; return CR_SUCCESS;
}

static void Populate(MODEL *M)
{
  CONST CR_SW_TARGET *T = M->C.Target;
  M->Reg[SWRM_MCP_SLV_STATUS / 4] = 0;
  for (UINT32 I = 0; I < T->SlaveCount; ++I) {
    UINT32 N = T == &CrSoundWireSm8450Rx ? 2 : T == &CrSoundWireSm8450Tx ? 4 : I + 1;
    M->Reg[SWRM_ENUMERATOR_SLAVE_DEV_ID_1(N) / 4] = (UINT32)T->Slaves[I].DeviceId;
    M->Reg[SWRM_ENUMERATOR_SLAVE_DEV_ID_2(N) / 4] = (UINT32)(T->Slaves[I].DeviceId >> 32);
    M->Reg[SWRM_MCP_SLV_STATUS / 4] |= 1U << (N * 2U);
    M->Codec[N][T->Slaves[I].ChipIdRegister] = 0x85 + (UINT8)I;
    M->Codec[N][T->Slaves[I].VariantRegister] = 0x0a + (UINT8)I;
    if (T->Slaves[I].PaControlRegister) { M->Codec[N][T->Slaves[I].PaControlRegister] = 0; }
  }
}

static CR_STATUS Reset(VOID *Context, CONST CR_SW_LEASE *L, BOOLEAN Asserted)
{
  MODEL *M = Context; Lease(M, L);
  if (!Asserted && M->Sibling && M->Reset && M->Sibling->C.State != CrSwBusReady) { return CR_BUSY; }
  ++M->Resets;
  M->Reset = Asserted;
  M->Now += 25;
  if (!Asserted) {
    Populate(M);
    if (M->Sibling) { M->Sibling->Reset = FALSE; Populate(M->Sibling); }
  }
  return M->ResetFail ? CR_DEVICE_ERROR : CR_SUCCESS;
}

static void CompleteHardware(MODEL *M)
{
  if (M->PendingWrite && !M->NoReply && M->Now >= M->CompleteAt) {
    M->PendingWrite = 0;
    if (M->PendingRead == 2) { M->PendingRead = 1; }
  }
}

static CR_STATUS Read(VOID *Context, CONST CR_SW_LEASE *L, UINT32 Offset, UINT32 *V)
{
  MODEL *M = Context; Lease(M, L); ++M->Reads;
  CHECK((Offset & 3) == 0 && Offset < 0x2000);
  if (M->FailRead == Offset + 1) { return CR_DEVICE_ERROR; }
  CompleteHardware(M);
  if (Offset == SWRM_CMD_FIFO_STATUS) {
    *V = M->PendingWrite << 8 | (M->PendingRead == 1 ? 1U << 16 : 0);
  } else if (Offset == SWRM_CMD_FIFO_RD_FIFO) {
    CHECK(M->PendingRead == 1);
    *V = M->ReturnWord; M->PendingRead = 0;
  } else { *V = M->Reg[Offset / 4]; }
  return CR_SUCCESS;
}

static CR_STATUS Write(VOID *Context, CONST CR_SW_LEASE *L, UINT32 Offset, UINT32 V)
{
  MODEL *M = Context; Lease(M, L); ++M->Writes;
  CHECK((Offset & 3) == 0 && Offset < 0x2000);
  BOOLEAN Fail = M->FailWrite == Offset + 1;
  if (Fail && !M->EffectBeforeFail) { return CR_DEVICE_ERROR; }
  if (Offset == SWRM_INTERRUPT_CLEAR) {
    M->Reg[SWRM_INTERRUPT_STATUS / 4] &= ~V;
  } else if (Offset == SWRM_CMD_FIFO_RD_CMD || Offset == SWRM_CMD_FIFO_WR_CMD) {
    CHECK(M->C.Request != NULL && M->C.Request->Submitted && !M->Reset);
    CHECK(M->C.Request->Serial != 0 && M->PendingWrite == 0 && M->PendingRead == 0);
    ++M->Sends;
    UINT32 N = (V >> 20) & 15, Id = (V >> 16) & 15, Reg = V & 65535;
    CHECK(N != 0 && N < 15 && Id != 15 && Reg < 0x3600);
    M->PendingWrite = 1;
    M->CompleteAt = M->Now + (Offset == SWRM_CMD_FIFO_RD_CMD ? 250 : 150);
    if (Offset == SWRM_CMD_FIFO_RD_CMD) {
      CHECK((V >> 24) == 1);
      M->PendingRead = 2;
      M->ReturnWord = ((M->WrongId ? (Id + 1) % 15 : Id) << 8) | M->Codec[N][Reg];
    } else if (!M->NoWriteEffect) { M->Codec[N][Reg] = (UINT8)(V >> 24); }
    if (M->InterruptInSend) {
      M->Reg[SWRM_INTERRUPT_STATUS / 4] |= SWRM_INTERRUPT_STATUS_BUS_RESET_FINISHED;
      M->Irq.Handler(M->Irq.Param);
    }
  } else {
    M->Reg[Offset / 4] = V;
    if (Offset == SWRM_COMP_CFG) { M->Reg[SWRM_COMP_STATUS / 4] = M->LinkFail ? 0 : (V & 1); }
  }
  return Fail ? CR_DEVICE_ERROR : CR_SUCCESS;
}

static CR_STATUS Quiesce(VOID *Context, CONST CR_SW_LEASE *L)
{
  MODEL *M = Context; Lease(M, L);
  CHECK(!M->Registered && !M->Scheduled);
  ++M->Quiesces;
  if (M->QuiesceFail) { return CR_BUSY; }
  if (M->Sibling && M->Sibling->C.State != CrSwStopping && M->Sibling->C.State != CrSwStopped) {
    return CR_BUSY; /* Shared WCD reset: close BOTH controllers first. */
  }
  if (M->Sibling) {
    MODEL *Other = M->Sibling;
    CHECK(!Other->Registered && !Other->Scheduled);
    Other->Reset = TRUE; Other->PendingRead = Other->PendingWrite = 0;
    Other->Reg[SWRM_COMP_CFG / 4] = 0; Other->Reg[SWRM_CPU1_INTERRUPT_EN / 4] = 0;
    Other->Reg[SWRM_INTERRUPT_STATUS / 4] = 0; Other->Quiet = TRUE;
  }
  M->Reset = TRUE; M->PendingRead = M->PendingWrite = 0;
  M->Reg[SWRM_COMP_CFG / 4] = 0; M->Reg[SWRM_CPU1_INTERRUPT_EN / 4] = 0;
  M->Reg[SWRM_INTERRUPT_STATUS / 4] = 0; M->Quiet = TRUE;
  return CR_SUCCESS;
}

static CR_STATUS Release(VOID *Context, CR_SW_LEASE *L)
{
  MODEL *M = Context; Lease(M, L);
  CHECK(!M->Registered && !M->Scheduled);
  CHECK(M->Resets == 0 || M->Quiet);
  if (M->ReleaseFail) {
    if (M->ClearOnReleaseFail) { memset(L, 0, sizeof(*L)); }
    return CR_BUSY;
  }
  ++M->Releases; M->Acquired = FALSE; memset(L, 0, sizeof(*L));
  return CR_SUCCESS;
}

static CR_STATUS Schedule(VOID *Context, UINT32 Delay)
{
  MODEL *M = Context; ++M->Schedules;
  if (M->ScheduleFail) { return CR_DEVICE_ERROR; }
  if (!M->Scheduled || M->Next > M->Now + Delay) { M->Next = M->Now + Delay; }
  M->Scheduled = TRUE;
  return CR_SUCCESS;
}

static CR_STATUS Cancel(VOID *Context)
{
  MODEL *M = Context;
  CHECK(!M->Registered);
  if (M->CancelFail) { return CR_BUSY; }
  M->Scheduled = FALSE;
  return CR_SUCCESS;
}

static UINT64 Now(VOID *Context) { return ((MODEL *)Context)->Now; }

static MODEL *New(CONST CR_SW_TARGET *T)
{
  MODEL *M = calloc(1, sizeof(*M)); CHECK(M != NULL);
  M->Io = (CR_SW_IO){M, Acquire, Rate, Reset, Read, Write, Quiesce, Release, Schedule, Cancel, Now};
  M->Rate = 9600000;
  M->Reg[SWRM_COMP_HW_VERSION / 4] = CR_SW_VERSION_1_7;
  M->Reg[SWRM_COMP_PARAMS / 4] = (8U << 10) | (8U << 15) | (15U << 20);
  CHECK(CrSoundWireInit(&M->C, T, &M->Io, &M->Irq) == CR_SUCCESS);
  return M;
}

static CR_STATUS Pump(MODEL *M)
{
  CHECK(M->Scheduled);
  M->Now = M->Next; M->Scheduled = FALSE;
  return CrSoundWireStep(&M->C);
}

static void BusReady(MODEL *M)
{
  CHECK(CrSoundWireStart(&M->C) == CR_NOT_READY);
  CHECK(M->Reg[SWRM_LINK_MANAGER_EE / 4] == 1);
  CHECK(M->Reg[SWRM_MCP_BUS_CTRL / 4] == 4);
  CHECK(M->Reg[SWRM_MCP_FRAME_CTRL_BANK(0) / 4] == ((47U << 16) | 8U));
  CHECK(M->Reg[SWRM_CMD_FIFO_CFG / 4] == (M->C.Target->MasterId == 1 ? 1U : 3U));
  CHECK(M->Reg[0x204 / 4] == 0); /* Never the older/CPU0 interrupt bank. */
  CHECK(Pump(M) == CR_SUCCESS && M->C.State == CrSwBusReady);
}

static void Ready(MODEL *M)
{
  BusReady(M);
  CHECK(CrSoundWireReleaseSlaves(&M->C) == CR_NOT_READY);
  CHECK(Pump(M) == CR_SUCCESS && M->C.State == CrSwReady);
}

static CR_SW_RESULT Transaction(MODEL *M, UINT32 Index, UINT16 Reg, BOOLEAN Wr, UINT8 Value)
{
  CR_SW_SLAVE B; CR_SW_REQUEST R = {0}; CR_SW_RESULT Out;
  CHECK(CrSoundWireGetSlave(&M->C, Index, &B) == CR_SUCCESS);
  CHECK((Wr ? CrSoundWireWriteByte(&M->C, &B, Reg, Value, &R) :
         CrSoundWireReadByte(&M->C, &B, Reg, &R)) == CR_NOT_READY);
  UINT64 Serial = R.Serial;
  CHECK(CrSoundWireTake(&M->C, &R, Serial, &Out) == CR_NOT_READY);
  for (unsigned I = 0; !R.Complete && I < 60; ++I) {
    CR_STATUS S = Pump(M); CHECK(S == CR_SUCCESS || S == CR_NOT_READY);
  }
  CHECK(R.Complete);
  CHECK(CrSoundWireTake(&M->C, &R, Serial + 1, &Out) == CR_INVALID_PARAMETER);
  CHECK(CrSoundWireTake(&M->C, &R, Serial, &Out) == CR_SUCCESS && Out.Status == CR_SUCCESS);
  CHECK(Out.Kind == (Wr ? CrSwWriteDrained : CrSwReadValue));
  CHECK(CrSoundWireTake(&M->C, &R, Serial, &Out) == CR_INVALID_PARAMETER);
  return Out;
}

static void Dispose(MODEL *M)
{
  CHECK(CrSoundWireStop(&M->C) == CR_SUCCESS);
  CHECK(!M->Registered && !M->Acquired && !M->Scheduled);
  free(M);
}

static void ProbeConsumer(void)
{
  CHECK(CrSoundWireSm8450Tx.Slaves[0].ChipIdRegister == WCD938X_DIGITAL_CHIP_ID0);
  CHECK(CrSoundWireSm8450Tx.Slaves[0].VariantRegister == WCD938X_DIGITAL_EFUSE_REG_0);
  CHECK(CrSoundWireSm8450Wsa.Slaves[0].ChipIdRegister == WSA883X_CHIP_ID0);
  CHECK(CrSoundWireSm8450Wsa.Slaves[0].VariantRegister == WSA883X_OTP_REG_0);
  CHECK(CrSoundWireSm8450Wsa.Slaves[0].PaControlRegister == WSA883X_PA_FSM_CTL);
  MODEL *Rx = New(&CrSoundWireSm8450Rx), *Tx = New(&CrSoundWireSm8450Tx);
  Rx->Sibling = Tx; Tx->Sibling = Rx;
  BusReady(Rx);
  CHECK(CrSoundWireReleaseSlaves(&Rx->C) == CR_BUSY && Rx->Reset);
  BusReady(Tx);
  CHECK(CrSoundWireReleaseSlaves(&Rx->C) == CR_NOT_READY);
  CHECK(CrSoundWireReleaseSlaves(&Tx->C) == CR_NOT_READY);
  CHECK(Pump(Rx) == CR_SUCCESS && Pump(Tx) == CR_SUCCESS);
  CHECK(Transaction(Tx, 0, WCD938X_DIGITAL_CHIP_ID0, FALSE, 0).Value == 0x85);
  CHECK(Transaction(Tx, 0, WCD938X_DIGITAL_EFUSE_REG_0, FALSE, 0).Value == 0x0a);
  /* Exercise complete ID reuse cycles only after exact responses + empty FIFO. */
  for (unsigned I = 0; I < 40; ++I) {
    CHECK(Transaction(Tx, 0, WCD938X_DIGITAL_CHIP_ID0, FALSE, 0).Value == 0x85);
  }
  MODEL *Wsa = New(&CrSoundWireSm8450Wsa); Ready(Wsa);
  for (UINT32 I = 0; I < 2; ++I) {
    CHECK(Transaction(Wsa, I, WSA883X_CHIP_ID0, FALSE, 0).Value == 0x85 + I);
    CHECK(Transaction(Wsa, I, WSA883X_OTP_REG_0, FALSE, 0).Value == 0x0a + I);
    CHECK((Transaction(Wsa, I, WSA883X_PA_FSM_CTL, FALSE, 0).Value & 1) == 0);
  }
  /* A model PA initially on is explicitly cleared then VERIFIED by a read. */
  Wsa->Codec[1][WSA883X_PA_FSM_CTL] = 0x81;
  CHECK(Transaction(Wsa, 0, WSA883X_PA_FSM_CTL, TRUE, 0x80).Kind == CrSwWriteDrained);
  CHECK(Transaction(Wsa, 0, WSA883X_PA_FSM_CTL, FALSE, 0).Value == 0x80);
  Wsa->NoWriteEffect = TRUE;
  CHECK(Transaction(Wsa, 0, WSA883X_PA_FSM_CTL, TRUE, 0).Kind == CrSwWriteDrained);
  CHECK(Transaction(Wsa, 0, WSA883X_PA_FSM_CTL, FALSE, 0).Value == 0x80);
  Dispose(Wsa);
  CHECK(CrSoundWireStop(&Rx->C) == CR_BUSY && Rx->Acquired && Rx->Releases == 0);
  CHECK(CrSoundWireStop(&Tx->C) == CR_SUCCESS);
  Dispose(Rx); free(Tx);
}

static void QualifyFailures(void)
{
  for (unsigned Case = 0; Case < 10; ++Case) {
    MODEL *M = New(&CrSoundWireSm8450Tx);
    switch (Case) {
      case 0: M->Reg[SWRM_COMP_HW_VERSION / 4] = 0x01060000; break;
      case 1: M->Rate = 600000; break;
      case 2: M->Reg[SWRM_COMP_PARAMS / 4] &= ~SWRM_COMP_PARAMS_RD_FIFO_DEPTH; break;
      case 3: M->Reg[SWRM_COMP_PARAMS / 4] &= ~SWRM_COMP_PARAMS_WR_FIFO_DEPTH; break;
      case 4: M->Reg[SWRM_COMP_PARAMS / 4] &= ~SWRM_COMP_PARAMS_AUTO_ENUM_SLAVES; break;
      case 5: M->Reg[SWRM_COMP_CFG / 4] = 3; break;
      case 6: M->PendingWrite = 1; M->NoReply = TRUE; break;
      case 7: M->Reg[SWRM_CPU1_INTERRUPT_EN / 4] = 4; break;
      case 8: M->FailRead = SWRM_COMP_PARAMS + 1; break;
      default: M->PartialAcquire = TRUE; break;
    }
    CHECK(CrSoundWireStart(&M->C) != CR_SUCCESS && M->C.State == CrSwFaulted);
    CHECK(M->Writes == 0 && M->Resets == 0 && M->Sends == 0);
    Dispose(M); /* Preflight never resets somebody else's active hardware. */
  }
}

static void FaultCases(void)
{
  for (unsigned Case = 0; Case < 9; ++Case) {
    MODEL *M = New(&CrSoundWireSm8450Tx); Ready(M);
    CR_SW_SLAVE B; CR_SW_REQUEST R = {0}; CR_SW_RESULT Result;
    CHECK(CrSoundWireGetSlave(&M->C, 0, &B) == CR_SUCCESS);
    if (Case == 0) { M->WrongId = TRUE; }
    if (Case == 1) { M->NoReply = TRUE; }
    if (Case == 2) { M->FailWrite = SWRM_CMD_FIFO_RD_CMD + 1; M->EffectBeforeFail = TRUE; }
    if (Case == 7) { M->InterruptInSend = TRUE; }
    CHECK(CrSoundWireReadByte(&M->C, &B, WCD938X_DIGITAL_CHIP_ID0, &R) == CR_NOT_READY);
    if (Case == 8) {
      M->Reg[SWRM_INTERRUPT_STATUS / 4] |= SWRM_INTERRUPT_STATUS_CMD_ERROR;
      /* No ISR delivered yet: the final command path must see the latch. */
    }
    if (Case >= 3 && Case <= 6) {
      CHECK(Pump(M) == CR_NOT_READY);
      UINT32 Bits = Case == 3 ? SWRM_INTERRUPT_STATUS_CMD_ERROR :
                    Case == 4 ? SWRM_INTERRUPT_STATUS_BUS_RESET_FINISHED :
                    Case == 5 ? SWRM_INTERRUPT_STATUS_RD_FIFO_OVERFLOW :
                                SWRM_INTERRUPT_STATUS_CHANGE_ENUM_SLAVE_STATUS;
      if (Case == 6) { M->Reg[SWRM_MCP_SLV_STATUS / 4] = 0; }
      M->Reg[SWRM_INTERRUPT_STATUS / 4] |= Bits;
      M->Irq.Handler(M->Irq.Param);
    }
    for (unsigned I = 0; M->C.State != CrSwFaulted && I < 65; ++I) { (VOID)Pump(M); }
    CHECK(M->C.State == CrSwFaulted && M->C.Request == &R && !R.Complete);
    if (Case == 8) { CHECK(M->Sends == 0); }
    CHECK(CrSoundWireTake(&M->C, &R, R.Serial, &Result) == CR_NOT_READY);
    M->QuiesceFail = TRUE;
    CHECK(CrSoundWireStop(&M->C) == CR_BUSY && M->Acquired && !R.Complete && M->Releases == 0);
    M->QuiesceFail = FALSE;
    CHECK(CrSoundWireStop(&M->C) == CR_SUCCESS && R.Complete);
    CHECK(CrSoundWireTake(&M->C, &R, R.Serial, &Result) == CR_SUCCESS);
    CHECK(Result.Kind == CrSwRetiredError && Result.Status != CR_SUCCESS);
    CHECK(Result.Value == 0); Dispose(M);
  }
}

static void LifetimeCases(void)
{
  for (unsigned Case = 0; Case < 6; ++Case) {
    MODEL *M = New(&CrSoundWireSm8450Wsa);
    if (Case == 0) { M->PartialRegister = TRUE; }
    if (Case == 1) { M->BadRegisterSuccess = TRUE; }
    if (Case == 2) { M->ResetFail = TRUE; }
    if (Case == 3) { M->ScheduleFail = TRUE; }
    if (Case == 4) { M->FailWrite = SWRM_MCP_CFG + 1; M->EffectBeforeFail = TRUE; }
    if (Case == 5) { M->LinkFail = TRUE; }
    CR_STATUS S = CrSoundWireStart(&M->C);
    if (Case == 5) {
      CHECK(S == CR_NOT_READY);
      for (unsigned I = 0; M->C.State != CrSwFaulted && I < 105; ++I) { (VOID)Pump(M); }
    }
    CHECK(M->C.State == CrSwFaulted && M->Acquired);
    M->UnregisterFail = TRUE;
    if (M->C.IrqAttempted) {
      CHECK(CrSoundWireStop(&M->C) == CR_BUSY && M->Acquired && M->Quiesces == 0);
    }
    M->UnregisterFail = FALSE; M->CancelFail = TRUE;
    CHECK(CrSoundWireStop(&M->C) == CR_BUSY && M->Acquired && M->Quiesces == 0);
    M->CancelFail = FALSE; M->ReleaseFail = TRUE;
    CHECK(CrSoundWireStop(&M->C) == CR_BUSY && M->Acquired && M->Quiesces == 1);
    M->ReleaseFail = FALSE; Dispose(M);
  }
  MODEL *M = New(&CrSoundWireSm8450Tx); Ready(M);
  CR_SW_SLAVE Old; CR_SW_REQUEST R = {0};
  CHECK(CrSoundWireGetSlave(&M->C, 0, &Old) == CR_SUCCESS);
  CHECK(CrSoundWireStop(&M->C) == CR_SUCCESS); Ready(M);
  UINT32 Sends = M->Sends;
  CHECK(CrSoundWireReadByte(&M->C, &Old, 0x3401, &R) == CR_NOT_FOUND && M->Sends == Sends);
  CR_SW_CORE Copy = M->C;
  CHECK(CrSoundWireStep(&Copy) == CR_INVALID_PARAMETER);
  CHECK(CrSoundWireInit(&M->C, M->C.Target, &M->Io, &M->Irq) != CR_SUCCESS);
  M->C.NextSerial = CR_MAX_UINT64;
  CR_SW_SLAVE B; CHECK(CrSoundWireGetSlave(&M->C, 0, &B) == CR_SUCCESS);
  CHECK(CrSoundWireReadByte(&M->C, &B, 0x3401, &R) == CR_OUT_OF_RESOURCES && M->Sends == Sends);
  Dispose(M);
  M = New(&CrSoundWireSm8450Tx); M->EmptyAcquireSuccess = TRUE;
  CHECK(CrSoundWireStart(&M->C) == CR_DEVICE_ERROR && M->C.LeaseUncertain);
  CHECK(CrSoundWireStop(&M->C) == CR_DEVICE_ERROR); free(M); /* isolated no-effect model */
  M = New(&CrSoundWireSm8450Tx); Ready(M);
  M->ReleaseFail = M->ClearOnReleaseFail = TRUE;
  CHECK(CrSoundWireStop(&M->C) == CR_BUSY && M->C.LeaseUncertain);
  CHECK(CrSoundWireStop(&M->C) == CR_DEVICE_ERROR); free(M); /* contract-violating peer only */
  M = New(&CrSoundWireSm8450Tx); M->C.Epoch = CR_MAX_UINT64;
  CHECK(CrSoundWireStart(&M->C) == CR_OUT_OF_RESOURCES && M->Acquires == 0); Dispose(M);
  M = New(&CrSoundWireSm8450Tx); BusReady(M); M->C.Enumeration = CR_MAX_UINT64;
  CHECK(CrSoundWireReleaseSlaves(&M->C) == CR_NOT_READY);
  CHECK(Pump(M) == CR_OUT_OF_RESOURCES && M->Sends == 0); Dispose(M);
}

static void InterruptCases(void)
{
  MODEL *M = New(&CrSoundWireSm8450Wsa); Ready(M);
  UINT32 Schedules = M->Schedules;
  M->Reg[SWRM_INTERRUPT_STATUS / 4] = SWRM_INTERRUPT_STATUS_SLAVE_PEND_IRQ;
  M->Irq.Handler(M->Irq.Param);
  CHECK(M->Schedules == Schedules); /* Nested codec IRQ has no owner yet. */
  CHECK(M->Reg[SWRM_INTERRUPT_STATUS / 4] == SWRM_INTERRUPT_STATUS_SLAVE_PEND_IRQ);
  M->Reg[SWRM_INTERRUPT_STATUS / 4] |= SWRM_INTERRUPT_STATUS_SPECIAL_CMD_ID_FINISHED;
  M->Irq.Handler(M->Irq.Param);
  CHECK(M->Schedules == Schedules + 1);
  CHECK(M->Reg[SWRM_INTERRUPT_STATUS / 4] == SWRM_INTERRUPT_STATUS_SLAVE_PEND_IRQ);
  CHECK(Pump(M) == CR_SUCCESS && M->C.State == CrSwReady);
  M->FailRead = SWRM_INTERRUPT_STATUS + 1;
  M->Irq.Handler(M->Irq.Param);
  CHECK(M->Reg[SWRM_CPU1_INTERRUPT_EN / 4] == 0);
  CHECK(Pump(M) != CR_SUCCESS && M->C.State == CrSwFaulted);
  Dispose(M);
}

static void SameAddressEnumerationCases(void)
{
  MODEL *M = New(&CrSoundWireSm8450Tx); Ready(M);
  CR_SW_SLAVE Old, Fresh; CR_SW_REQUEST R = {0}; CR_SW_RESULT Result;
  CHECK(CrSoundWireGetSlave(&M->C, 0, &Old) == CR_SUCCESS);
  /* A detach/re-attach can return the identical final status, ID and address. */
  M->Reg[SWRM_INTERRUPT_STATUS / 4] |= SWRM_INTERRUPT_STATUS_CHANGE_ENUM_SLAVE_STATUS;
  M->Irq.Handler(M->Irq.Param);
  CHECK(CrSoundWireReadByte(&M->C, &Old, 0x3401, &R) == CR_NOT_FOUND);
  CHECK(M->Sends == 0 && R.Owner == NULL);
  CHECK(CrSoundWireGetSlave(&M->C, 0, &Fresh) == CR_SUCCESS);
  CHECK(Fresh.LogicalNumber == Old.LogicalNumber && Fresh.DeviceId == Old.DeviceId);
  CHECK(Fresh.Epoch == Old.Epoch && Fresh.Enumeration > Old.Enumeration);
  CHECK(Transaction(M, 0, 0x3401, FALSE, 0).Value == 0x85);
  Dispose(M);

  for (unsigned Phase = 0; Phase < 3; ++Phase) {
    M = New(&CrSoundWireSm8450Tx); Ready(M); memset(&R, 0, sizeof(R));
    CHECK(CrSoundWireGetSlave(&M->C, 0, &Old) == CR_SUCCESS);
    CHECK(CrSoundWireReadByte(&M->C, &Old, 0x3401, &R) == CR_NOT_READY);
    if (Phase >= 1) { CHECK(Pump(M) == CR_NOT_READY && R.Submitted); }
    if (Phase == 2) { CHECK(Pump(M) == CR_SUCCESS && R.Complete); }
    M->Reg[SWRM_INTERRUPT_STATUS / 4] |= SWRM_INTERRUPT_STATUS_CHANGE_ENUM_SLAVE_STATUS;
    M->Irq.Handler(M->Irq.Param);
    if (Phase < 2) { CHECK(Pump(M) == CR_DEVICE_ERROR && !R.Complete); }
    CHECK(CrSoundWireTake(&M->C, &R, R.Serial, &Result) == CR_NOT_READY);
    CHECK(M->C.State == CrSwFaulted && M->C.Request == &R);
    CHECK(M->Sends == (Phase == 0 ? 0U : 1U));
    M->QuiesceFail = TRUE;
    CHECK(CrSoundWireStop(&M->C) == CR_BUSY && M->Acquired && M->Releases == 0);
    M->QuiesceFail = FALSE;
    CHECK(CrSoundWireStop(&M->C) == CR_SUCCESS);
    CHECK(CrSoundWireTake(&M->C, &R, R.Serial, &Result) == CR_SUCCESS);
    CHECK(Result.Kind == CrSwRetiredError && Result.Status != CR_SUCCESS);
    Dispose(M);
  }
}

int main(void)
{
  ProbeConsumer(); QualifyFailures(); FaultCases(); LifetimeCases(); InterruptCases();
  SameAddressEnumerationCases();
  printf("SoundWire actual core + Android-register model/probe/retained-lifetime PASS: %u checks\n", Checks);
  return 0;
}
