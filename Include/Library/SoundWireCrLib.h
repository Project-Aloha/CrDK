/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors.
 * Original portable engine using source-qualified Qualcomm controller facts. */
#pragma once
#include <oskal/cr_interrupt.h>

#define CR_SW_MAX_SLAVES 2U
#define CR_SW_VERSION_1_7 0x01070000U

typedef struct {
  UINT64 DeviceId;
  UINT16 ChipIdRegister;
  UINT16 VariantRegister;
  UINT16 PaControlRegister; /* WSA only; bit0 must remain clear for ID probe. */
  UINT16 ResetGpio;
} CR_SW_SLAVE_TARGET;

typedef struct {
  UINT64 Base;
  UINT32 Span;
  UINT32 CoreSpi;
  UINT32 WakeSpi; /* Inventory only. No low-power/wake operation in this core. */
  UINT32 MasterId;
  UINT32 EnumerateSlots;
  UINT32 SlaveCount;
  UINT32 ResetReleaseUs;
  CR_SW_SLAVE_TARGET Slaves[CR_SW_MAX_SLAVES];
} CR_SW_TARGET;

extern CONST CR_SW_TARGET CrSoundWireSm8450Rx;
extern CONST CR_SW_TARGET CrSoundWireSm8450Tx;
extern CONST CR_SW_TARGET CrSoundWireSm8450Wsa;

typedef struct { VOID *Provider; UINT64 Token; } CR_SW_LEASE;
typedef struct _CR_SW_CORE CR_SW_CORE;

/* Acquire commits a retained exact-address lease, including failure after
   partial ownership. Failure with a zero lease promises NO retained effect.
   Actual PEP/MMIO/clock/rail/reset providers implement these operations; a
   function pointer or local generation is not a grant. Acquire holds this
   controller exclusively and joins the shared WCD RX/TX reset coordinator.
   Every MMIO operation validates the exact current lease atomically with IO.
   Software resets or power loss close every affected lease's admission before
   changing hardware; no independent writer may silently reset an owned bus.
   All MMIO callbacks and Schedule are bounded, nonpaged and IRQ-safe.

   SetReset operates real owned slave reset/shutdown pins. Releasing shared
   WCD reset returns BUSY before effects until RX and TX are both ready; all
   other failures retain the lease. Quiesce must actually stop/reset the exact
   old bus/slaves and retire unknown register commands, including failed MMIO;
   it cannot infer this from a counter or timeout. Shared WCD Quiesce returns
   BUSY until BOTH RX/TX owners closed admission and drained IRQ/deferred work;
   stop the other core, then retry the retained first Stop. Release may only clear Lease
   after all real resources ended. Failure retains it. No direct clock fallback.

   Schedule only queues serialized work, never calls the core synchronously.
   CancelDeferred synchronously cancels timers and drains queued callbacks
   (excluding the caller's current worker). An IRQ-safe failure to schedule
   requires the adapter's persistent worker/teardown path to observe the fault.
   NowUs is monotonic through the context lifetime. ClockHz reports the actual
   retained bus clock, not a requested rate. */
typedef struct {
  VOID *Context;
  CR_STATUS (*Acquire)(VOID *, CONST VOID *Owner, CONST CR_SW_TARGET *, CR_SW_LEASE *);
  CR_STATUS (*ClockHz)(VOID *, CONST CR_SW_LEASE *, UINT32 *);
  CR_STATUS (*SetReset)(VOID *, CONST CR_SW_LEASE *, BOOLEAN Asserted);
  CR_STATUS (*Read32)(VOID *, CONST CR_SW_LEASE *, UINT32 Offset, UINT32 *);
  CR_STATUS (*Write32)(VOID *, CONST CR_SW_LEASE *, UINT32 Offset, UINT32);
  CR_STATUS (*Quiesce)(VOID *, CONST CR_SW_LEASE *);
  CR_STATUS (*Release)(VOID *, CR_SW_LEASE *);
  CR_STATUS (*Schedule)(VOID *, UINT32 DelayUs);
  CR_STATUS (*CancelDeferred)(VOID *);
  UINT64 (*NowUs)(VOID *);
} CR_SW_IO;

typedef struct {
  CONST CR_SW_CORE *Core;
  UINT64 Epoch;
  UINT64 Enumeration;
  UINT64 DeviceId;
  UINT8 LogicalNumber;
  UINT8 Reserved[7];
} CR_SW_SLAVE;

typedef enum {
  CrSwStopped = 0, CrSwWaitLink, CrSwBusReady, CrSwWaitSlaves,
  CrSwReady, CrSwFaulted, CrSwStopping
} CR_SW_STATE;
typedef enum { CrSwReadValue = 1, CrSwWriteDrained = 2, CrSwRetiredError = 3 } CR_SW_RESULT_KIND;
typedef struct {
  UINT64 Serial;
  UINT64 Epoch;
  CR_STATUS Status;
  UINT32 Kind;
  UINT8 Value;
  UINT8 Reserved[3];
} CR_SW_RESULT;
typedef struct {
  CONST CR_SW_CORE *Owner;
  UINT64 Serial, Epoch, Enumeration, DeviceId, Deadline, Earliest;
  UINT16 Register;
  UINT8 LogicalNumber, CommandId, Value, Write, Submitted, Complete;
  CR_SW_RESULT Result;
} CR_SW_REQUEST;

struct _CR_SW_CORE {
  CR_SW_CORE *Self;
  CR_SW_IO Io;
  CONST CR_SW_TARGET *Target;
  CR_INTERRUPT_CONFIG *Interrupt;
  CR_SW_LEASE Lease;
  CR_SW_REQUEST *Request;
  UINT64 Epoch, Enumeration, NextSerial, LastNow, Deadline, Earliest;
  CR_SW_SLAVE Slaves[CR_SW_MAX_SLAVES];
  volatile UINT32 PendingIrq, CaptureEnabled;
  CR_STATUS LastError;
  UINT32 State, ReadDepth, WriteDepth, AutoSlaves, CommandId;
  BOOLEAN IrqAttempted, DeferredStopped, Quiesced, RegistersTouched, ResetAttempted;
  BOOLEAN LeaseUncertain;
};

/* Init once on zeroed aligned stable storage. Never memcpy/reinitialize a
   live/previous context; Self/counters remain through Stop/Start. Target and
   IO code/storage and OSKAL interrupt config remain immutable through final
   Stop. No allocation. Caller serializes every worker API; IRQ is the only
   concurrent entry and touches only stable IO and atomic event fields.
   The actual adapter owns IRQ/PnP/code rundown; this core does not pin images.
   OSKAL configuration is prepared by that adapter with empty Handler/Param. */
CR_STATUS CrSoundWireInit(CR_SW_CORE *, CONST CR_SW_TARGET *,
                          CONST CR_SW_IO *, CR_INTERRUPT_CONFIG *);
/* Actual setup, followed asynchronously by Step. Requires inactive/empty EE1,
   observed version1.7 and COMP_PARAMS, actual9.6MHz, real reset ownership.
   On partial failure call Stop on the exact context; do not zero it. */
CR_STATUS CrSoundWireStart(CR_SW_CORE *);
/* Called when bus ready; board owner coordinates WCD's shared RX/TX reset.
   BUSY is a pre-effect refusal and may be retried. */
CR_STATUS CrSoundWireReleaseSlaves(CR_SW_CORE *);
CR_STATUS CrSoundWireStep(CR_SW_CORE *);
CR_STATUS CrSoundWireGetSlave(CR_SW_CORE *, UINT32 Index, CR_SW_SLAVE *);
/* Request storage is zeroed initially, retained unchanged until successful
   Take or real Stop retirement. There is ONE outstanding command. Begin
   journals before MMIO. Zero/wrap serial exhaustion fails before dispatch.
   A foreign/stale binding or changed enumeration cannot dispatch. */
CR_STATUS CrSoundWireReadByte(CR_SW_CORE *, CONST CR_SW_SLAVE *, UINT16 Register,
                              CR_SW_REQUEST *);
CR_STATUS CrSoundWireWriteByte(CR_SW_CORE *, CONST CR_SW_SLAVE *, UINT16 Register,
                               UINT8 Value, CR_SW_REQUEST *);
CR_STATUS CrSoundWireTake(CR_SW_CORE *, CR_SW_REQUEST *, UINT64 ExpectedSerial,
                          CR_SW_RESULT *);
/* Ordinary writes have no remote ACK. WriteDrained proves only a drained
   local write FIFO after the minimum delay and no observed fault. Verify
   required mute/control state with readback; do not call it confirmed codec
   programming. No streaming/port-bank/PA enable operation is provided. */
CR_STATUS CrSoundWireStop(CR_SW_CORE *);
