/** Synchronous PIL consumer retirement. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_PIL_MAX_CLIENTS 8U
typedef CR_STATUS (*CR_PIL_CLIENT_QUIESCE)(VOID *Context);
typedef struct {
  VOID *Owner, *Context;
  CR_PIL_CLIENT_QUIESCE Quiesce;
  BOOLEAN Quiet;
} CR_PIL_CLIENT;
typedef struct {
  CR_PIL_CLIENT Entries[CR_PIL_MAX_CLIENTS];
  UINT32 Count;
  BOOLEAN Frozen, Dispatching;
} CR_PIL_CLIENTS;

/* Zero initialize once. The adapter serializes all calls, including callbacks.
   A registered client and its code remain resident until Unregister succeeds.
   Quiesce freezes registration permanently. Success means every registered
   client synchronously stopped all remote writers and IRQ/deferred callbacks;
   a queued stop is insufficient. Failed clients remain callable on retry.
   Successful callbacks are not repeated. No allocation or OS calls occur. */
CR_STATUS CrPilClientRegister(CR_PIL_CLIENTS *, VOID *Owner,
                             CR_PIL_CLIENT_QUIESCE, VOID *Context);
CR_STATUS CrPilClientUnregister(CR_PIL_CLIENTS *, VOID *Owner);
CR_STATUS CrPilClientsQuiesce(CR_PIL_CLIENTS *);
