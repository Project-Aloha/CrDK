/* Shared MPROC receive owner; Linux qcom-ipcc register semantics.
 * Copyright (c) 2018-2020, The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_IPCC_RX_GLINK 1U
#define CR_IPCC_RX_SMP2P 2U
#define CR_IPCC_RX_ROUTES 2U
#define CR_IPCC_RX_BUDGET 64U
/* Conservative software liveness limit, not a silicon fault indication. */
#define CR_IPCC_RX_MAX_FULL_BUDGETS 4U
typedef struct {
  VOID *Context;
  UINT32 (*Read32)(VOID *,UINT32);
  VOID (*Write32)(VOID *,UINT32,UINT32);
} CR_IPCC_RX_IO;
/* Called under the parent ISR guard, possibly at DIRQL. Publish an atomic
 * fence and queue a DPC only. Never reenter any receive operation. */
typedef CR_STATUS (*CR_IPCC_RX_WAKE)(VOID *);
typedef struct {
  VOID *Provider,*Owner;
  UINT64 Generation,Token;
  UINT32 Role,Reserved;
} CR_IPCC_RX_LEASE;
typedef struct {
  UINT64 Generation,Token,Sequence;
  CR_STATUS Error;
  BOOLEAN Pending,Closing,SourceLost,More,Paused;
} CR_IPCC_RX_OBSERVATION;
typedef struct {
  CR_IPCC_RX_LEASE *Identity;
  VOID *Owner,*WakeContext;
  CR_IPCC_RX_WAKE Wake;
  UINT64 Token,Sequence;
  CR_STATUS Error;
  BOOLEAN Pending,Closing,Enabled;
} CR_IPCC_RX_ROUTE;
typedef struct {
  CR_IPCC_RX_IO Io;
  CR_IPCC_RX_ROUTE Route[CR_IPCC_RX_ROUTES];
  UINT64 Generation,NextToken;
  UINT32 Irqs,Unknown,Received,FullBudgets;
  BOOLEAN Initialized,Started,ConfigOwned,SavedClearOnRead,Sealed,Lost,More,Exhausted,Faulted;
} CR_IPCC_RECEIVE;
/* Every operation, including IRQ, runs under one OS interrupt guard. Adapter
 * Init/Start/Stop also serialize source mapping/registration lifetimes. Guard
 * acquisition is not part of this core: it must exclude the real parent ISR.
 * Storage/counters stay resident and are never moved/zeroed after first Init.
 * No SEND access; exactly one receiver may own these CONFIG/RX windows. */
CR_STATUS CrIpccReceiveInit(CR_IPCC_RECEIVE *,CONST CR_IPCC_RX_IO *,UINT64);
CR_STATUS CrIpccReceiveStart(CR_IPCC_RECEIVE *);
CR_STATUS CrIpccReceiveBegin(CR_IPCC_RECEIVE *,VOID *,UINT32,CR_IPCC_RX_WAKE,VOID *,CR_IPCC_RX_LEASE *);
CR_STATUS CrIpccReceiveObserve(CR_IPCC_RECEIVE *,VOID *,CONST CR_IPCC_RX_LEASE *,CR_IPCC_RX_OBSERVATION *);
CR_STATUS CrIpccReceiveComplete(CR_IPCC_RECEIVE *,VOID *,CONST CR_IPCC_RX_LEASE *,UINT64);
CR_STATUS CrIpccReceiveClose(CR_IPCC_RECEIVE *,VOID *,CONST CR_IPCC_RX_LEASE *);
/* Close fences direct callbacks. Consumer must independently drain its queued
 * DPC/work before End and before releasing callback context/code. */
CR_STATUS CrIpccReceiveEnd(CR_IPCC_RECEIVE *,VOID *,CR_IPCC_RX_LEASE *);
VOID CrIpccReceiveInterrupt(CR_IPCC_RECEIVE *);
/* Deferred bounded FIFO drainage, including when no route is admitted. */
CR_STATUS CrIpccReceiveDrain(CR_IPCC_RECEIVE *);
/* NoMmio is one-way source loss; callbacks receive an error, exact leases and
 * config ownership survive. It is not evidence that hardware was masked. */
CR_STATUS CrIpccReceiveSeal(CR_IPCC_RECEIVE *,BOOLEAN NoMmio);
CR_STATUS CrIpccReceiveStop(CR_IPCC_RECEIVE *);
CR_STATUS CrIpccReceiveDetach(CR_IPCC_RECEIVE *);

/* Resume wake is an ordinary new observation under the same ISR guard. */
CR_STATUS CrIpccReceiveRefresh(CR_IPCC_RECEIVE *);
