/* No-allocation firmware UART admission and actual GENI idle witness. MIT */
#include <Library/UartAdmissionLib.h>
#include <oskal/cr_atomic.h>

static UINT32 Cas(volatile UINT32 *Value, UINT32 Old, UINT32 New) {
#ifdef _KERNEL_MODE
  return (UINT32)InterlockedCompareExchange((volatile LONG *)Value,
      (LONG)New, (LONG)Old);
#else
  return InterlockedCompareExchange32(Value, Old, New);
#endif
}
static BOOLEAN Valid(CONST CR_UART_ADMISSION *Gate) {
  return Gate && !((UINTN)Gate & 3U) &&
      Gate->Signature == CR_UART_ADMISSION_SIGNATURE &&
      Gate->Size == sizeof(*Gate) && !Gate->Reserved[0] &&
      !Gate->Reserved[1];
}
static CR_STATUS Exact(CR_STATUS Status) {
  return Status == CR_SUCCESS ? Status :
      CR_ERROR(Status) ? Status : CR_NOT_READY;
}
CR_STATUS CrUartAdmissionInitialize(CR_UART_ADMISSION *Gate) {
  if (!Gate || ((UINTN)Gate & 3U)) return CR_INVALID_PARAMETER;
  CONST UINT32 *Fields = (CONST UINT32 *)Gate;
  for (UINT32 I = 0; I < sizeof(*Gate) / sizeof(UINT32); ++I)
    if (Fields[I]) return CR_ALREADY_STARTED;
  Gate->Size = sizeof(*Gate);
  Gate->Signature = CR_UART_ADMISSION_SIGNATURE;
  Cas(&Gate->State, 0, CR_UART_ADMISSION_OPEN);
  return CR_SUCCESS;
}
BOOLEAN CrUartAdmissionEnter(CR_UART_ADMISSION *Gate) {
  if (!Valid(Gate) || CrAtomicLoad32(&Gate->State) != CR_UART_ADMISSION_OPEN ||
      Cas(&Gate->Active, 0, 1) != 0) return FALSE;
  if (CrAtomicLoad32(&Gate->State) != CR_UART_ADMISSION_OPEN) {
    Cas(&Gate->Active, 1, 0);
    return FALSE;
  }
  return TRUE;
}
CR_STATUS CrUartAdmissionLeave(CR_UART_ADMISSION *Gate) {
  if (!Valid(Gate)) return CR_INVALID_PARAMETER;
  return Cas(&Gate->Active, 1, 0) == 1 ? CR_SUCCESS : CR_NOT_READY;
}
CR_STATUS CrUartAdmissionBind(CR_UART_ADMISSION *Gate, BOOLEAN Runtime) {
  if (!Valid(Gate) || (Runtime != FALSE && Runtime != TRUE))
    return CR_INVALID_PARAMETER;
  if (!CrUartAdmissionEnter(Gate)) return CR_NOT_READY;
  CR_STATUS Status = CR_SUCCESS;
  UINT32 Bindings = CrAtomicLoad32(&Gate->Bindings);
  UINT32 RuntimeBindings = CrAtomicLoad32(&Gate->RuntimeBindings);
  if (Bindings == CR_MAX_UINT32 ||
      (Runtime && RuntimeBindings == CR_MAX_UINT32))
    Status = CR_OUT_OF_RESOURCES;
  else {
    Cas(&Gate->Bindings, Bindings, Bindings + 1U);
    if (Runtime) Cas(&Gate->RuntimeBindings, RuntimeBindings, RuntimeBindings + 1U);
  }
  CrUartAdmissionLeave(Gate);
  return Status;
}
CR_STATUS CrUartAdmissionClose(CR_UART_ADMISSION *Gate) {
  if (!Valid(Gate)) return CR_INVALID_PARAMETER;
  UINT32 State = Cas(&Gate->State, CR_UART_ADMISSION_OPEN,
      CR_UART_ADMISSION_CLOSED);
  if (State != CR_UART_ADMISSION_OPEN && State != CR_UART_ADMISSION_CLOSED)
    return CR_COMPROMISED_DATA;
  return CrAtomicLoad32(&Gate->Active) ? CR_NOT_READY : CR_SUCCESS;
}

/* Source offsets: upstream include/linux/soc/qcom/geni-se.h. Both UART
 * sequencers use CANCEL bit2 then ABORT bit1 and IRQ acknowledgement4/5. */
#define UART_STATUS 0x40U
#define UART_FW 0x68U
#define UART_DMA 0x258U
#define UART_M_CTRL 0x604U
#define UART_M_IRQ 0x610U
#define UART_M_ENABLE 0x614U
#define UART_M_CLEAR 0x618U
#define UART_S_CTRL 0x634U
#define UART_S_IRQ 0x640U
#define UART_S_ENABLE 0x644U
#define UART_S_CLEAR 0x648U
#define UART_TX_STATUS 0x800U
#define UART_RX_STATUS 0x804U
#define UART_RX_FIFO 0x780U
#define UART_DMA_TX_ENABLE 0xc48U
#define UART_DMA_RX_ENABLE 0xd48U
#define UART_GSI_ENABLE 0xe18U
#define UART_SE_ENABLE 0xe1cU
#define UART_RX_PARAM 0xe28U
#define UART_M_ACTIVE 1U
#define UART_S_ACTIVE (1U << 12)
#define UART_TX_WORDS 0x0fffffffU
#define UART_RX_WORDS 0x01ffffffU

static CR_STATUS Read(CONST CR_UART_RETIRE_IO *Io, UINT32 Offset,
    UINT32 *Value) {
  return Exact(Io->Read32(Io->Context, Offset, Value));
}
static CR_STATUS Write(CONST CR_UART_RETIRE_IO *Io,
    CR_UART_RETIRE_OBSERVATION *Observation, UINT32 Offset, UINT32 Value) {
  ++Observation->WritesAttempted; /* before a potentially committed write */
  CR_STATUS Status = Exact(Io->Write32(Io->Context, Offset, Value));
  Io->Fence(Io->Context);
  return Status;
}
static CR_STATUS Stop(CONST CR_UART_RETIRE_IO *Io,
    CR_UART_RETIRE_OBSERVATION *O, BOOLEAN Secondary) {
  UINT32 Active = Secondary ? UART_S_ACTIVE : UART_M_ACTIVE;
  UINT32 Ctrl = Secondary ? UART_S_CTRL : UART_M_CTRL;
  UINT32 Irq = Secondary ? UART_S_IRQ : UART_M_IRQ;
  UINT32 Clear = Secondary ? UART_S_CLEAR : UART_M_CLEAR;
  UINT32 Value;
  CR_STATUS Status = Read(Io, UART_STATUS, &Value);
  if (Status != CR_SUCCESS || !(Value & Active)) return Status;
  for (UINT32 Attempt = 0; Attempt < 2; ++Attempt) {
    UINT32 Done = 1U << (4U + Attempt);
    Status = Write(Io, O, Clear, Done);
    if (Status != CR_SUCCESS) return Status;
    Status = Write(Io, O, Ctrl, Attempt ? 2U : 4U);
    if (Status != CR_SUCCESS) return Status;
    for (UINT32 Wait = 0; Wait < CR_UART_RETIRE_TIMEOUT_US; Wait += 10U) {
      UINT32 Acknowledgement;
      Status = Read(Io, Irq, &Acknowledgement);
      if (Status != CR_SUCCESS) return Status;
      Status = Read(Io, UART_STATUS, &Value);
      if (Status != CR_SUCCESS) return Status;
      if ((Acknowledgement & Done) && !(Value & Active))
        return Write(Io, O, Clear, Done);
      Io->DelayUs(Io->Context, 10);
    }
  }
  return CR_TIMEOUT;
}
static CR_STATUS Quiesce(CONST CR_UART_RETIRE_IO *Io,
    CR_UART_RETIRE_OBSERVATION *O) {
  UINT32 Value;
  CR_STATUS Status = Read(Io, UART_FW, &Value);
  if (Status != CR_SUCCESS) return Status;
  O->Protocol = (Value >> 8U) & 255U;
  if (O->Protocol != 2) return CR_UNSUPPORTED;
  Status = Read(Io, UART_DMA, &O->DmaMode);
  if (Status != CR_SUCCESS || O->DmaMode)
    return Status != CR_SUCCESS ? Status : CR_UNSUPPORTED;
  /* Preserve a final accepted TX burst before bounded cancel/abort. */
  for (UINT32 Wait = 0; Wait < CR_UART_RETIRE_TIMEOUT_US; Wait += 10U) {
    Status = Read(Io, UART_STATUS, &Value);
    if (Status != CR_SUCCESS) return Status;
    if (!(Value & UART_M_ACTIVE)) break;
    Io->DelayUs(Io->Context, 10);
  }
  Status = Stop(Io, O, FALSE);
  if (Status != CR_SUCCESS) return Status;
  Status = Stop(Io, O, TRUE);
  if (Status != CR_SUCCESS) return Status;
  /* No active sequencer can produce more RX. Consume only the finite actual
   * FIFO contents; impossible counts refuse the handoff without claiming idle. */
  Status = Read(Io, UART_RX_STATUS, &Value);
  if (Status != CR_SUCCESS) return Status;
  Value &= UART_RX_WORDS;
  UINT32 Parameter;
  Status = Read(Io, UART_RX_PARAM, &Parameter);
  if (Status != CR_SUCCESS) return Status;
  /* The existing native console uses the six-bit legacy FIFO format. Do not
   * silently reinterpret that producer as a newer 256-byte hardware variant. */
  O->RxDepth = (Parameter >> 16U) & 63U;
  if (!O->RxDepth || Value > O->RxDepth) return CR_COMPROMISED_DATA;
  while (Value--) {
    UINT32 Discard;
    Status = Read(Io, UART_RX_FIFO, &Discard);
    if (Status != CR_SUCCESS) return Status;
  }
  CONST UINT32 Enables[] = {UART_M_ENABLE, UART_S_ENABLE, UART_DMA_TX_ENABLE,
      UART_DMA_RX_ENABLE, UART_GSI_ENABLE, UART_SE_ENABLE};
  for (UINT32 I = 0; I < sizeof(Enables) / sizeof(Enables[0]); ++I) {
    Status = Write(Io, O, Enables[I], 0);
    if (Status != CR_SUCCESS) return Status;
  }
  Status = Write(Io, O, UART_M_CLEAR, ~0U);
  if (Status != CR_SUCCESS) return Status;
  Status = Write(Io, O, UART_S_CLEAR, ~0U);
  if (Status != CR_SUCCESS) return Status;
  UINT32 *Fields[] = {&O->MasterIrqEnable, &O->SlaveIrqEnable,
      &O->DmaTxIrqEnable, &O->DmaRxIrqEnable, &O->GsiEventEnable,
      &O->SeIrqEnable};
  for (UINT32 I = 0; I < sizeof(Enables) / sizeof(Enables[0]); ++I) {
    Status = Read(Io, Enables[I], Fields[I]);
    if (Status != CR_SUCCESS || *Fields[I])
      return Status != CR_SUCCESS ? Status : CR_NOT_READY;
  }
  Status = Read(Io, UART_STATUS, &O->GeniStatus);
  if (Status != CR_SUCCESS) return Status;
  Status = Read(Io, UART_TX_STATUS, &O->TxWords);
  if (Status != CR_SUCCESS) return Status;
  O->TxWords &= UART_TX_WORDS;
  Status = Read(Io, UART_RX_STATUS, &O->RxWords);
  if (Status != CR_SUCCESS) return Status;
  O->RxWords &= UART_RX_WORDS;
  Status = Read(Io, UART_DMA, &O->DmaMode);
  if (Status != CR_SUCCESS) return Status;
  return (O->GeniStatus & (UART_M_ACTIVE | UART_S_ACTIVE)) ||
      O->TxWords || O->RxWords || O->DmaMode ? CR_NOT_READY : CR_SUCCESS;
}
CR_STATUS CrUartAdmissionRetire(CR_UART_ADMISSION *Gate,
    CONST CR_UART_RETIRE_IO *Io, CR_UART_RETIRE_OBSERVATION *Output) {
  if (!Valid(Gate) || !Output || !Io || !Io->Read32 || !Io->Write32 ||
      !Io->DelayUs || !Io->Fence) return CR_INVALID_PARAMETER;
  if ((UINTN)Output > ~(UINTN)0 - sizeof(*Output) ||
      ((UINTN)Output < (UINTN)Gate + sizeof(*Gate) &&
       (UINTN)Gate < (UINTN)Output + sizeof(*Output)) ||
      ((UINTN)Output < (UINTN)Io + sizeof(*Io) &&
       (UINTN)Io < (UINTN)Output + sizeof(*Output)))
    return CR_INVALID_PARAMETER;
  CR_UART_RETIRE_OBSERVATION O = {0};
  CR_STATUS Status = CrUartAdmissionClose(Gate);
  if (Status == CR_NOT_READY) {
    for (UINT32 Wait = 0; Wait < CR_UART_RETIRE_TIMEOUT_US; Wait += 10U) {
      if (!CrAtomicLoad32(&Gate->Active)) { Status = CR_SUCCESS; break; }
      Io->DelayUs(Io->Context, 10);
    }
    if (Status != CR_SUCCESS) Status = CR_TIMEOUT;
  }
  if (Status == CR_SUCCESS) Status = Quiesce(Io, &O);
  O.GateState = CrAtomicLoad32(&Gate->State);
  O.Active = CrAtomicLoad32(&Gate->Active);
  O.Bindings = CrAtomicLoad32(&Gate->Bindings);
  O.RuntimeBindings = CrAtomicLoad32(&Gate->RuntimeBindings);
  O.Status = Status;
  O.Complete = Status == CR_SUCCESS && !O.Active;
  *Output = O;
  return Status;
}
