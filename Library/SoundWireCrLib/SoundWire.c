/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors.
 * Register sequence facts: The Linux Foundation (2015-2021) and
 * Qualcomm Innovation Center, Inc. (2022-2023); see README/LICENSE. */
#include <Library/SoundWireCrLib.h>
#include <oskal/cr_atomic.h>

#define HW_VERSION 0x0000U
#define COMP_CFG 0x0004U
#define COMP_STATUS 0x0014U
#define LINK_EE 0x0018U
#define COMP_PARAMS 0x0100U
#define IRQ_STATUS 0x0200U
#define IRQ_CLEAR 0x0208U
#define CPU1_IRQ_EN 0x0210U
#define FIFO_CFG 0x0314U
#define CPU1_WR_CMD 0x031cU
#define CPU1_RD_CMD 0x0320U
#define CPU1_FIFO_STATUS 0x0328U
#define CPU1_RD_FIFO 0x0334U
#define ENUM_CFG 0x0500U
#define ENUM_ID_LOW(N) (0x0530U + 8U * (N))
#define ENUM_ID_HIGH(N) (0x0534U + 8U * (N))
#define FRAME_BANK0 0x101cU
#define BUS_CTRL 0x1044U
#define MCP_CFG 0x1048U
#define SLAVE_STATUS 0x1090U
/* Codec in-band pending remains masked until a real codec IRQ owner exists. */
#define IRQ_MASK 0x001dfdfcU
#define IRQ_FATAL (0x000c0000U | 0x00003bc8U | 0x00010000U)
#define SOFTWARE_FAULT 0x80000000U
#define ENUM_CHANGE 4U

static VOID Zero(VOID *P, UINTN N)
{
  UINT8 *B = P;
  for (UINTN I = 0; I < N; ++I) { B[I] = 0; }
}

static BOOLEAN Valid(CONST CR_SW_CORE *C)
{
  return C != NULL && ((UINTN)C & 7U) == 0 && C->Self == C;
}

static BOOLEAN Overlap(CONST VOID *A, UINTN An, CONST VOID *B, UINTN Bn)
{
  if (A == NULL || B == NULL || (UINTN)A > (UINTN)-1 - An ||
      (UINTN)B > (UINTN)-1 - Bn) { return TRUE; }
  return (UINTN)A < (UINTN)B + Bn && (UINTN)B < (UINTN)A + An;
}

static BOOLEAN LeaseEmpty(CONST CR_SW_LEASE *L)
{
  return L->Provider == NULL && L->Token == 0;
}

static CR_STATUS Rd(CR_SW_CORE *C, UINT32 Offset, UINT32 *V)
{
  return C->Io.Read32(C->Io.Context, &C->Lease, Offset, V);
}

static CR_STATUS Wr(CR_SW_CORE *C, UINT32 Offset, UINT32 V)
{
  return C->Io.Write32(C->Io.Context, &C->Lease, Offset, V);
}

static CR_STATUS Fault(CR_SW_CORE *C, CR_STATUS S)
{
  if (S == CR_SUCCESS) { S = CR_DEVICE_ERROR; }
  if (C->LastError == CR_SUCCESS) { C->LastError = S; }
  C->State = CrSwFaulted;
  if (C->RegistersTouched) { (VOID)Wr(C, CPU1_IRQ_EN, 0); }
  return S;
}

static CR_STATUS Clock(CR_SW_CORE *C, UINT64 *Now)
{
  UINT64 N = C->Io.NowUs(C->Io.Context);
  if (N < C->LastNow || N > CR_MAX_UINT64 - 100000U) {
    return Fault(C, CR_OUT_OF_RESOURCES);
  }
  C->LastNow = N;
  *Now = N;
  return CR_SUCCESS;
}

static CR_STATUS Later(CR_SW_CORE *C, UINT32 Delay)
{
  CR_STATUS S = C->Io.Schedule(C->Io.Context, Delay);
  return S == CR_SUCCESS ? CR_NOT_READY : Fault(C, S);
}

/* OSKAL callback. No resource acquisition, client callback or worker reentry. */
static VOID Interrupt(VOID *Context)
{
  CR_SW_CORE *C = Context;
  UINT32 Status = 0;
  if (CrAtomicLoad32(&C->CaptureEnabled) == 0) { return; }
  CR_STATUS S = Rd(C, IRQ_STATUS, &Status);
  Status &= IRQ_MASK;
  if (S != CR_SUCCESS) {
    Status = SOFTWARE_FAULT;
    (VOID)Wr(C, CPU1_IRQ_EN, 0);
  } else if (Status != 0) {
    if (Wr(C, IRQ_CLEAR, Status) != CR_SUCCESS || Wr(C, IRQ_CLEAR, 0) != CR_SUCCESS) {
      Status |= SOFTWARE_FAULT;
      (VOID)Wr(C, CPU1_IRQ_EN, 0);
    }
  }
  if (Status != 0) {
    CrAtomicOr32(&C->PendingIrq, Status);
    if (C->Io.Schedule(C->Io.Context, 0) != CR_SUCCESS) {
      CrAtomicOr32(&C->PendingIrq, SOFTWARE_FAULT);
      (VOID)Wr(C, CPU1_IRQ_EN, 0);
    }
  }
}

static CR_STATUS Events(CR_SW_CORE *C, UINT32 *EventsOut)
{
  UINT32 Events = CrAtomicAnd32(&C->PendingIrq, 0);
  *EventsOut = Events;
  if ((Events & (IRQ_FATAL | SOFTWARE_FAULT)) != 0) {
    return Fault(C, CR_DEVICE_ERROR);
  }
  if ((Events & ENUM_CHANGE) != 0 && C->State == CrSwReady) {
    /* A detach/re-attach can restore the same final ID/status/address. The
       event invalidates old bindings even when the next snapshot is equal.
       An outstanding command belongs to the old bus and needs Quiesce. */
    if (C->Request != NULL) { return Fault(C, CR_DEVICE_ERROR); }
    Zero(C->Slaves, sizeof(C->Slaves));
  }
  return CR_SUCCESS;
}

CR_STATUS CrSoundWireInit(CR_SW_CORE *C, CONST CR_SW_TARGET *Target,
                          CONST CR_SW_IO *Io, CR_INTERRUPT_CONFIG *Irq)
{
  if (C == NULL || ((UINTN)C & 7U) != 0 || Target == NULL || Io == NULL || Irq == NULL ||
      Overlap(C, sizeof(*C), Target, sizeof(*Target)) ||
      Overlap(C, sizeof(*C), Io, sizeof(*Io)) ||
      Overlap(C, sizeof(*C), Irq, sizeof(*Irq)) ||
      Io->Context == NULL || Io->Acquire == NULL || Io->ClockHz == NULL ||
      Io->SetReset == NULL || Io->Read32 == NULL || Io->Write32 == NULL ||
      Io->Quiesce == NULL || Io->Release == NULL || Io->Schedule == NULL ||
      Io->CancelDeferred == NULL || Io->NowUs == NULL ||
      (Target != &CrSoundWireSm8450Rx && Target != &CrSoundWireSm8450Tx &&
       Target != &CrSoundWireSm8450Wsa) || Irq->Handler != NULL || Irq->Param != NULL) {
    return CR_INVALID_PARAMETER;
  }
  for (UINTN I = 0; I < sizeof(*C); ++I) {
    if (((UINT8 *)C)[I] != 0) { return CR_ALREADY_STARTED; }
  }
  C->Self = C; C->Io = *Io; C->Target = Target; C->Interrupt = Irq;
  C->NextSerial = 1;
  return CR_SUCCESS;
}

CR_STATUS CrSoundWireStart(CR_SW_CORE *C)
{
  UINT32 V, Params, Rate, Config, Fifo, Mask, Mcp;
  UINT64 Now;
  CR_STATUS S;
  if (!Valid(C)) { return CR_INVALID_PARAMETER; }
  if (C->State != CrSwStopped || !LeaseEmpty(&C->Lease) || C->Request != NULL ||
      C->IrqAttempted) { return CR_BUSY; }
  if (C->Epoch == CR_MAX_UINT64 || C->NextSerial == 0 || C->NextSerial == CR_MAX_UINT64) {
    return CR_OUT_OF_RESOURCES;
  }
  ++C->Epoch;
  C->State = CrSwWaitLink; C->LastError = CR_SUCCESS;
  C->Quiesced = FALSE; C->DeferredStopped = FALSE;
  C->RegistersTouched = FALSE; C->ResetAttempted = FALSE;
  CrAtomicAnd32(&C->PendingIrq, 0);
  S = C->Io.Acquire(C->Io.Context, C, C->Target, &C->Lease);
  if ((C->Lease.Provider == NULL) != (C->Lease.Token == 0) ||
      (S == CR_SUCCESS && LeaseEmpty(&C->Lease))) {
    C->LeaseUncertain = TRUE;
    return Fault(C, CR_DEVICE_ERROR);
  }
  if (S != CR_SUCCESS) { return Fault(C, S); }
  if ((S = C->Io.ClockHz(C->Io.Context, &C->Lease, &Rate)) != CR_SUCCESS ||
      (S = Rd(C, HW_VERSION, &V)) != CR_SUCCESS ||
      (S = Rd(C, COMP_PARAMS, &Params)) != CR_SUCCESS ||
      (S = Rd(C, COMP_CFG, &Config)) != CR_SUCCESS ||
      (S = Rd(C, CPU1_FIFO_STATUS, &Fifo)) != CR_SUCCESS ||
      (S = Rd(C, CPU1_IRQ_EN, &Mask)) != CR_SUCCESS) { return Fault(C, S); }
  if (V != CR_SW_VERSION_1_7 || Rate != 9600000) { return Fault(C, CR_UNSUPPORTED); }
  C->WriteDepth = (Params >> 10) & 31U;
  C->ReadDepth = (Params >> 15) & 31U;
  C->AutoSlaves = (Params >> 20) & 15U;
  if (C->WriteDepth == 0 || C->ReadDepth == 0 ||
      C->AutoSlaves < C->Target->EnumerateSlots) {
    return Fault(C, CR_COMPROMISED_DATA);
  }
  if ((Config & 1U) != 0 || (Fifo & 0x001f1f00U) != 0 || Mask != 0) {
    return Fault(C, CR_BUSY);
  }
  C->ResetAttempted = TRUE;
  if ((S = C->Io.SetReset(C->Io.Context, &C->Lease, TRUE)) != CR_SUCCESS) {
    return Fault(C, S);
  }
  if ((S = Clock(C, &Now)) != CR_SUCCESS || (S = Rd(C, MCP_CFG, &Mcp)) != CR_SUCCESS) {
    return Fault(C, S);
  }
  /* The provider's successful Assert holds reset >=20us before returning. */
  C->RegistersTouched = TRUE;
  CONST UINT32 Registers[] = {FRAME_BANK0, ENUM_CFG, MCP_CFG, FIFO_CFG, LINK_EE, BUS_CTRL, COMP_CFG};
  CONST UINT32 Values[] = {0x002f0008U, 1, (Mcp & ~0x003e0000U) | 0x003e0000U,
                          C->Target->MasterId == 1 ? 1U : 3U, 1, 4, 2};
  for (UINTN I = 0; I < sizeof(Registers) / sizeof(Registers[0]); ++I) {
    if ((S = Wr(C, Registers[I], Values[I])) != CR_SUCCESS) { return Fault(C, S); }
  }
  if ((S = Rd(C, IRQ_STATUS, &V)) != CR_SUCCESS ||
      (S = Wr(C, IRQ_CLEAR, V & IRQ_MASK)) != CR_SUCCESS ||
      (S = Wr(C, IRQ_CLEAR, 0)) != CR_SUCCESS) { return Fault(C, S); }
  C->Interrupt->Handler = Interrupt; C->Interrupt->Param = C;
  C->IrqAttempted = TRUE;
  CrAtomicOr32(&C->CaptureEnabled, 1);
  S = CrRegisterInterrupt(C->Interrupt);
  if (S != CR_SUCCESS) { return Fault(C, S); }
  if (!CrInterruptIsRegistered(C->Interrupt)) { return Fault(C, CR_DEVICE_ERROR); }
  if ((S = Wr(C, CPU1_IRQ_EN, IRQ_MASK)) != CR_SUCCESS ||
      (S = Wr(C, COMP_CFG, 3)) != CR_SUCCESS) { return Fault(C, S); }
  C->Deadline = Now + 50000;
  return Later(C, 500);
}

CR_STATUS CrSoundWireReleaseSlaves(CR_SW_CORE *C)
{
  UINT64 Now;
  CR_STATUS S;
  if (!Valid(C)) { return CR_INVALID_PARAMETER; }
  if (C->State != CrSwBusReady) { return CR_NOT_READY; }
  if ((S = Clock(C, &Now)) != CR_SUCCESS) { return S; }
  S = C->Io.SetReset(C->Io.Context, &C->Lease, FALSE);
  if (S == CR_BUSY) { return S; }
  if (S != CR_SUCCESS) { return Fault(C, S); }
  if ((S = Clock(C, &Now)) != CR_SUCCESS) { return S; }
  C->State = CrSwWaitSlaves;
  C->Earliest = Now + C->Target->ResetReleaseUs;
  C->Deadline = Now + 20000;
  return Later(C, C->Target->ResetReleaseUs);
}

static CR_STATUS Enumerate(CR_SW_CORE *C)
{
  CR_SW_SLAVE Found[CR_SW_MAX_SLAVES] = {{0}};
  UINT32 Before, After, Lo, Hi;
  BOOLEAN Changed = FALSE, Missing = FALSE;
  CR_STATUS S = Rd(C, SLAVE_STATUS, &Before);
  if (S != CR_SUCCESS) { return Fault(C, S); }
  for (UINT32 I = 0; I < C->Target->SlaveCount; ++I) {
    Found[I].Core = C; Found[I].Epoch = C->Epoch;
    Found[I].DeviceId = C->Target->Slaves[I].DeviceId;
  }
  for (UINT32 N = 1; N <= C->Target->EnumerateSlots; ++N) {
    UINT32 State = (Before >> (N * 2U)) & 3U;
    if (State != 1 && State != 2) { continue; }
    if ((S = Rd(C, ENUM_ID_LOW(N), &Lo)) != CR_SUCCESS ||
        (S = Rd(C, ENUM_ID_HIGH(N), &Hi)) != CR_SUCCESS) { return Fault(C, S); }
    UINT64 Id = (((UINT64)Hi << 32) | Lo) & 0x0000ffffffffffffULL;
    for (UINT32 I = 0; I < C->Target->SlaveCount; ++I) {
      if (C->Target->Slaves[I].DeviceId == Id) {
        if (Found[I].LogicalNumber != 0) { return Fault(C, CR_COMPROMISED_DATA); }
        Found[I].Core = C; Found[I].Epoch = C->Epoch;
        Found[I].DeviceId = Id; Found[I].LogicalNumber = (UINT8)N;
      }
    }
  }
  if ((S = Rd(C, SLAVE_STATUS, &After)) != CR_SUCCESS) { return Fault(C, S); }
  if (Before != After) { return CR_NOT_READY; }
  for (UINT32 I = 0; I < C->Target->SlaveCount; ++I) {
    Missing |= Found[I].LogicalNumber == 0;
    Changed |= Found[I].LogicalNumber != C->Slaves[I].LogicalNumber || C->Slaves[I].Epoch != C->Epoch;
  }
  if (Changed) {
    if (C->Enumeration == CR_MAX_UINT64) { return Fault(C, CR_OUT_OF_RESOURCES); }
    ++C->Enumeration;
    for (UINT32 I = 0; I < C->Target->SlaveCount; ++I) {
      Found[I].Enumeration = C->Enumeration;
      C->Slaves[I] = Found[I];
    }
    if (C->Request != NULL) { return Fault(C, CR_DEVICE_ERROR); }
  }
  return Missing ? CR_NOT_READY : CR_SUCCESS;
}

static CR_STATUS Fifo(CR_SW_CORE *C, UINT32 *Write, UINT32 *Read)
{
  UINT32 V;
  CR_STATUS S = Rd(C, CPU1_FIFO_STATUS, &V);
  if (S != CR_SUCCESS) { return Fault(C, S); }
  *Write = (V >> 8) & 31U; *Read = (V >> 16) & 31U;
  if (*Write > C->WriteDepth || *Read > C->ReadDepth) { return Fault(C, CR_COMPROMISED_DATA); }
  return CR_SUCCESS;
}

static CR_STATUS Progress(CR_SW_CORE *C, UINT64 Now)
{
  CR_SW_REQUEST *R = C->Request;
  UINT32 W, D, Value, EventsSeen;
  CR_STATUS S;
  if (R == NULL || R->Complete) { return CR_SUCCESS; }
  if (Now >= R->Deadline) { return Fault(C, CR_TIMEOUT); }
  if (Now < R->Earliest) { return Later(C, (UINT32)(R->Earliest - Now)); }
  if ((S = Fifo(C, &W, &D)) != CR_SUCCESS) { return S; }
  if (!R->Submitted) {
    if (D != 0) { return Fault(C, CR_COMPROMISED_DATA); }
    if (W != 0) { return Later(C, 500); }
    if ((S = Enumerate(C)) != CR_SUCCESS) {
      return S == CR_NOT_READY ? Fault(C, CR_DEVICE_ERROR) : S;
    }
    if (R->Enumeration != C->Enumeration) { return Fault(C, CR_DEVICE_ERROR); }
    Interrupt(C); /* Also observe a latched fault before OS callback dispatch. */
    if ((S = Events(C, &EventsSeen)) != CR_SUCCESS) { return S; }
    R->Submitted = TRUE; /* journal before a possibly effected MMIO failure */
    Value = R->Register | ((UINT32)R->CommandId << 16) |
            ((UINT32)R->LogicalNumber << 20) | ((UINT32)(R->Write ? R->Value : 1) << 24);
    S = Wr(C, R->Write ? CPU1_WR_CMD : CPU1_RD_CMD, Value);
    if (S != CR_SUCCESS) { return Fault(C, S); }
    R->Earliest = Now + (R->Write ? 150U : 250U);
    return Later(C, R->Write ? 150 : 250);
  }
  if (R->Write) {
    if (D != 0) { return Fault(C, CR_COMPROMISED_DATA); }
    if (W != 0) { return Later(C, 500); }
  } else {
    if (D == 0) { return Later(C, 500); }
    if (D != 1 || (S = Rd(C, CPU1_RD_FIFO, &Value)) != CR_SUCCESS) {
      return Fault(C, D != 1 ? CR_COMPROMISED_DATA : S);
    }
    if (((Value >> 8) & 15U) != R->CommandId) { return Fault(C, CR_COMPROMISED_DATA); }
    R->Value = (UINT8)Value;
    if ((S = Fifo(C, &W, &D)) != CR_SUCCESS) { return S; }
    if (D != 0 || W != 0) { return Fault(C, CR_COMPROMISED_DATA); }
  }
  /* Observe fault/reset status that arrived before callback dispatch. */
  Interrupt(C);
  if ((S = Events(C, &EventsSeen)) != CR_SUCCESS || (S = Enumerate(C)) != CR_SUCCESS) {
    return Fault(C, S);
  }
  R->Result.Serial = R->Serial; R->Result.Epoch = R->Epoch;
  R->Result.Status = CR_SUCCESS; R->Result.Kind = R->Write ? CrSwWriteDrained : CrSwReadValue;
  R->Result.Value = R->Write ? 0 : R->Value;
  R->Complete = TRUE;
  return CR_SUCCESS;
}

CR_STATUS CrSoundWireStep(CR_SW_CORE *C)
{
  UINT32 EventsSeen, V;
  UINT64 Now;
  CR_STATUS S;
  if (!Valid(C)) { return CR_INVALID_PARAMETER; }
  if (C->State == CrSwStopped || C->State == CrSwStopping) { return CR_NOT_READY; }
  if (C->State == CrSwFaulted) { return C->LastError; }
  if ((S = Clock(C, &Now)) != CR_SUCCESS || (S = Events(C, &EventsSeen)) != CR_SUCCESS) { return S; }
  if (C->State == CrSwWaitLink) {
    if (Now >= C->Deadline) { return Fault(C, CR_TIMEOUT); }
    if ((S = Rd(C, COMP_STATUS, &V)) != CR_SUCCESS) { return Fault(C, S); }
    if ((V & 1U) == 0) { return Later(C, 500); }
    if ((S = Rd(C, FIFO_CFG, &V)) != CR_SUCCESS ||
        (S = Wr(C, FIFO_CFG, V | 0x80000000U)) != CR_SUCCESS) { return Fault(C, S); }
    C->State = CrSwBusReady;
    return CR_SUCCESS;
  }
  if (C->State == CrSwBusReady) { return CR_SUCCESS; }
  if (C->State == CrSwWaitSlaves) {
    if (Now >= C->Deadline) { return Fault(C, CR_TIMEOUT); }
    if (Now < C->Earliest) { return Later(C, (UINT32)(C->Earliest - Now)); }
    S = Enumerate(C);
    if (S == CR_NOT_READY) { return Later(C, 100); }
    if (S != CR_SUCCESS) { return S; }
    C->State = CrSwReady;
  }
  if ((EventsSeen & ENUM_CHANGE) != 0) {
    S = Enumerate(C);
    if (S != CR_SUCCESS) { return Fault(C, S); }
  }
  return Progress(C, Now);
}

CR_STATUS CrSoundWireGetSlave(CR_SW_CORE *C, UINT32 Index, CR_SW_SLAVE *Slave)
{
  if (!Valid(C) || Slave == NULL || ((UINTN)Slave & 7U) != 0 ||
      Overlap(C, sizeof(*C), Slave, sizeof(*Slave)) || Index >= C->Target->SlaveCount) {
    return CR_INVALID_PARAMETER;
  }
  if (C->State != CrSwReady || C->Slaves[Index].LogicalNumber == 0) { return CR_NOT_READY; }
  *Slave = C->Slaves[Index];
  return CR_SUCCESS;
}

static CR_STATUS Begin(CR_SW_CORE *C, CONST CR_SW_SLAVE *Slave, UINT16 Register,
                        UINT8 Value, BOOLEAN Write, CR_SW_REQUEST *R)
{
  UINT64 Now;
  UINT32 EventsSeen;
  CR_STATUS S;
  if (!Valid(C) || Slave == NULL || ((UINTN)Slave & 7U) != 0 || R == NULL ||
      ((UINTN)R & 7U) != 0 || Overlap(C, sizeof(*C), R, sizeof(*R)) ||
      Overlap(Slave, sizeof(*Slave), R, sizeof(*R)) || R->Owner != NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (C->State != CrSwReady || C->Request != NULL) { return CR_BUSY; }
  if (C->NextSerial == 0 || C->NextSerial == CR_MAX_UINT64) { return Fault(C, CR_OUT_OF_RESOURCES); }
  if ((S = Events(C, &EventsSeen)) != CR_SUCCESS || (S = Clock(C, &Now)) != CR_SUCCESS ||
      (S = Enumerate(C)) != CR_SUCCESS) { return S; }
  BOOLEAN Match = FALSE;
  for (UINT32 I = 0; I < C->Target->SlaveCount; ++I) {
    CONST CR_SW_SLAVE *B = &C->Slaves[I];
    if (Slave->Core == C && Slave->Epoch == C->Epoch && Slave->Enumeration == C->Enumeration &&
        Slave->DeviceId == B->DeviceId && Slave->LogicalNumber == B->LogicalNumber &&
        B->LogicalNumber != 0) { Match = TRUE; }
  }
  if (!Match) { return CR_NOT_FOUND; }
  for (UINTN I = 0; I < sizeof(Slave->Reserved); ++I) {
    if (Slave->Reserved[I] != 0) { return CR_INVALID_PARAMETER; }
  }
  Zero(R, sizeof(*R));
  C->CommandId = C->CommandId < 14 ? C->CommandId + 1 : 0;
  R->Owner = C; R->Serial = C->NextSerial++; R->Epoch = C->Epoch;
  R->Enumeration = C->Enumeration; R->DeviceId = Slave->DeviceId;
  R->LogicalNumber = Slave->LogicalNumber; R->CommandId = (UINT8)C->CommandId;
  R->Register = Register; R->Value = Value; R->Write = Write;
  R->Deadline = Now + 20000; R->Earliest = Now + (Write ? 0 : 100U);
  C->Request = R;
  return Later(C, Write ? 0 : 100);
}

CR_STATUS CrSoundWireReadByte(CR_SW_CORE *C, CONST CR_SW_SLAVE *Slave,
                              UINT16 Register, CR_SW_REQUEST *R)
{
  return Begin(C, Slave, Register, 0, FALSE, R);
}

CR_STATUS CrSoundWireWriteByte(CR_SW_CORE *C, CONST CR_SW_SLAVE *Slave,
                               UINT16 Register, UINT8 Value, CR_SW_REQUEST *R)
{
  return Begin(C, Slave, Register, Value, TRUE, R);
}

CR_STATUS CrSoundWireTake(CR_SW_CORE *C, CR_SW_REQUEST *R, UINT64 Serial, CR_SW_RESULT *Out)
{
  if (!Valid(C) || R == NULL || R != C->Request || R->Owner != C || Serial == 0 ||
      R->Serial != Serial || Out == NULL || ((UINTN)Out & 7U) != 0 ||
      Overlap(Out, sizeof(*Out), C, sizeof(*C)) || Overlap(Out, sizeof(*Out), R, sizeof(*R))) {
    return CR_INVALID_PARAMETER;
  }
  if (!R->Complete) { return CR_NOT_READY; }
  if (C->State != CrSwStopped && C->State != CrSwReady) { return CR_NOT_READY; }
  if (C->State == CrSwReady) {
    UINT32 E;
    Interrupt(C);
    if (Events(C, &E) != CR_SUCCESS || Enumerate(C) != CR_SUCCESS) { return CR_NOT_READY; }
  }
  *Out = R->Result;
  R->Owner = NULL; C->Request = NULL;
  return CR_SUCCESS;
}

CR_STATUS CrSoundWireStop(CR_SW_CORE *C)
{
  CR_STATUS S;
  if (!Valid(C)) { return CR_INVALID_PARAMETER; }
  if (C->LeaseUncertain) { return CR_DEVICE_ERROR; }
  if (C->State == CrSwStopped && LeaseEmpty(&C->Lease) && !C->IrqAttempted) { return CR_SUCCESS; }
  C->State = CrSwStopping;
  CrAtomicAnd32(&C->CaptureEnabled, 0);
  /* A failed MMIO mask does not prevent independent OS IRQ rundown or the
     provider's actual reset/quiesce path. No mapping is released early. */
  if (C->RegistersTouched) { (VOID)Wr(C, CPU1_IRQ_EN, 0); }
  if (C->IrqAttempted) {
    S = CrUnregisterInterrupt(C->Interrupt);
    if (S != CR_SUCCESS) { return S; }
    if (CrInterruptIsRegistered(C->Interrupt)) { return CR_DEVICE_ERROR; }
    C->IrqAttempted = FALSE;
    C->Interrupt->Handler = NULL; C->Interrupt->Param = NULL;
  }
  if (!C->DeferredStopped) {
    S = C->Io.CancelDeferred(C->Io.Context);
    if (S != CR_SUCCESS) { return S; }
    C->DeferredStopped = TRUE;
  }
  if (!C->Quiesced && !LeaseEmpty(&C->Lease)) {
    if (C->RegistersTouched || C->ResetAttempted) {
      S = C->Io.Quiesce(C->Io.Context, &C->Lease);
      if (S != CR_SUCCESS) { return S; }
    }
    C->Quiesced = TRUE;
  }
  if (!LeaseEmpty(&C->Lease)) {
    S = C->Io.Release(C->Io.Context, &C->Lease);
    if (S != CR_SUCCESS) {
      if (LeaseEmpty(&C->Lease)) { C->LeaseUncertain = TRUE; }
      return S;
    }
    if (!LeaseEmpty(&C->Lease)) { return CR_DEVICE_ERROR; }
  }
  if (C->Request != NULL && (!C->Request->Complete || C->LastError != CR_SUCCESS)) {
    CR_SW_REQUEST *R = C->Request;
    Zero(&R->Result, sizeof(R->Result));
    R->Result.Serial = R->Serial; R->Result.Epoch = R->Epoch;
    R->Result.Kind = CrSwRetiredError;
    R->Result.Status = C->LastError != CR_SUCCESS ? C->LastError : CR_ABORT;
    R->Complete = TRUE;
  }
  CrAtomicAnd32(&C->PendingIrq, 0);
  Zero(C->Slaves, sizeof(C->Slaves));
  C->RegistersTouched = FALSE; C->ResetAttempted = FALSE;
  C->State = CrSwStopped;
  return CR_SUCCESS;
}
