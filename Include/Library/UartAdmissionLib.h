/* Shared firmware UART admission and bounded GENI retirement. MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_UART_ADMISSION_SIGNATURE 0x41475255U /* URGA */
#define CR_UART_ADMISSION_OPEN 1U
#define CR_UART_ADMISSION_CLOSED 2U
#define CR_UART_RETIRE_TIMEOUT_US 20000U

/* One permanent, zero-initialized normal-memory object shared by EVERY DXE
 * SerialPortLib copy on the enabled board. SEC/PEI must never survive EBS.
 * Private per-image state is not a replacement for this object. Active is
 * exclusive across copies, also refusing recursive DEBUG during a write.
 * Closed never reopens; failed retirement retains that exact object. */
typedef struct {
  UINT32 Signature, Size;
  volatile UINT32 State, Active;
  volatile UINT32 Bindings, RuntimeBindings;
  UINT32 Reserved[2];
} CR_UART_ADMISSION;

CR_STATUS CrUartAdmissionInitialize(CR_UART_ADMISSION *);
CR_STATUS CrUartAdmissionBind(CR_UART_ADMISSION *, BOOLEAN Runtime);
BOOLEAN CrUartAdmissionEnter(CR_UART_ADMISSION *);
CR_STATUS CrUartAdmissionLeave(CR_UART_ADMISSION *);
CR_STATUS CrUartAdmissionClose(CR_UART_ADMISSION *);

/* Real assigned GENI register access, normal-time source/power ownership and
 * event/IRQ drainage are supplied by the caller. Exact SUCCESS only; failed
 * writes may have committed. Delay must be bounded and may be a no-op in a
 * deterministic model. Fence orders actual posted writes before readback.
 * This core never allocates, opens firmware protocols or withdraws power. */
typedef struct {
  VOID *Context;
  CR_STATUS (*Read32)(VOID *, UINT32 Offset, UINT32 *Value);
  CR_STATUS (*Write32)(VOID *, UINT32 Offset, UINT32 Value);
  VOID (*DelayUs)(VOID *, UINT32);
  VOID (*Fence)(VOID *);
} CR_UART_RETIRE_IO;

typedef struct {
  UINT32 Protocol, GeniStatus, TxWords, RxWords, RxDepth;
  UINT32 MasterIrqEnable, SlaveIrqEnable, SeIrqEnable, GsiEventEnable;
  UINT32 DmaMode, DmaTxIrqEnable, DmaRxIrqEnable, WritesAttempted;
  UINT32 Bindings, RuntimeBindings, GateState, Active;
  CR_STATUS Status;
  BOOLEAN Complete;
} CR_UART_RETIRE_OBSERVATION;

/* Closes common admission before any hardware access. If an active call fails
 * to retire in the bound, no MMIO occurs. A timeout/error never reports idle
 * or releases ownership. Repeat on the same closed gate retries retirement.
 * Output is diagnostic on every valid invocation; Complete requires all
 * actual idle/FIFO/routing/DMA readbacks, not merely a stopped software gate.
 * Success proves native retirement only, not a Windows resource/owner grant. */
CR_STATUS CrUartAdmissionRetire(CR_UART_ADMISSION *,
    CONST CR_UART_RETIRE_IO *, CR_UART_RETIRE_OBSERVATION *);
