/** OSKAL IPCC receive and deferred-work bridge. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_interrupt.h>
#include <oskal/cr_atomic.h>

#define CR_IPCC_BASE 0x0ED18000U
#define CR_IPCC_SOURCE 261U /* sm8450.dtsi GIC_SPI 229 */
#define CR_IPCC_LPASS_GLINK (3U << 16)

/* Called from ISR: queue serialized deferred work; never process GLink here. */
typedef CR_STATUS (*CR_IPCC_WAKE)(VOID *Context);
/* Pure ISR-safe, one-way lifecycle admission. FALSE forbids new startup,
 * receive and signal-enable work; Stop still retires existing ownership. */
typedef BOOLEAN (*CR_IPCC_ADMIT)(VOID *Context);
/* Borrowed ISR-safe register operations for disjoint OS resource windows.
 * Only CONFIG08 and RX/ENABLE/DISABLE/CLEAR10..1c are used; never SEND. */
typedef struct {
  VOID *Context;
  UINT32 (*Read32)(VOID *Context,UINT32 Offset);
  VOID (*Write32)(VOID *Context,UINT32 Offset,UINT32 Value);
} CR_IPCC_IO;
typedef struct {
  CR_INTERRUPT_CONFIG Interrupt;
  UINTN Base;
  CR_IPCC_WAKE Wake;
  VOID *Context;
  BOOLEAN Registered, SignalOwned, ConfigChanged, SavedClearOnRead;
  volatile UINT32 Stopping, Pending;
  volatile CR_STATUS Error;
  volatile UINT32 Irqs, Received, OtherSignals;
  CR_IPCC_IO Io;
  CR_IPCC_ADMIT Admit;
  VOID *AdmitContext;
} CR_IPCC;

/* Caller supplies OS interrupt resources and mapped MMIO, and retains them
   until Stop succeeds. WDF adapters use the direct ISR handler (no LIRQ DPC)
   and queue their own work from Wake. Service/Stop run in serialized work;
   WDF Stop requires PASSIVE_LEVEL and the driver's PnP/power teardown path.
   To restart a stopped WDF context, pass its retained Interrupt config back
   to Start; WDF owns the underlying interrupt object's D0 activation. */
CR_STATUS CrIpccStart(CR_IPCC *Ipcc, UINTN Base, CONST CR_INTERRUPT_CONFIG *Config,
                      CR_IPCC_WAKE Wake, VOID *Context);
CR_STATUS CrIpccStartGuarded(CR_IPCC *Ipcc,UINTN Base,CONST CR_INTERRUPT_CONFIG *Config,
  CR_IPCC_WAKE Wake,VOID *Context,CR_IPCC_ADMIT Admit,VOID *AdmitContext);
CR_STATUS CrIpccStartWithIo(CR_IPCC *Ipcc,CONST CR_IPCC_IO *Io,
                          CONST CR_INTERRUPT_CONFIG *Config,CR_IPCC_WAKE Wake,VOID *Context);
CR_STATUS CrIpccService(CR_IPCC *Ipcc, BOOLEAN *GlinkReceived);
CR_STATUS CrIpccStop(CR_IPCC *Ipcc);
VOID CrIpccSend(UINTN Base);
