/** Actual portable consumer barrier, native + NT. SPDX-License-Identifier: MIT */
#include <Library/PILCrClients.h>
#include <assert.h>
#include <stdio.h>

static CR_PIL_CLIENTS Clients;
typedef struct { UINT32 Calls; CR_STATUS Status; } CLIENT;
static CLIENT Owners[CR_PIL_MAX_CLIENTS + 1];
static CR_STATUS Quiet(VOID *Context)
{
  CLIENT *Owner = Context;
  ++Owner->Calls;
  assert(CrPilClientUnregister(&Clients, Owner) == CR_BUSY);
  assert(CrPilClientRegister(&Clients, Owner, Quiet, Context) == CR_BUSY);
  assert(CrPilClientsQuiesce(&Clients) == CR_BUSY);
  return Owner->Status;
}
int main(void)
{
  assert(CrPilClientRegister(NULL, Owners, Quiet, Owners) == CR_INVALID_PARAMETER);
  assert(CrPilClientRegister(&Clients, NULL, Quiet, Owners) == CR_INVALID_PARAMETER);
  assert(CrPilClientRegister(&Clients, Owners, NULL, Owners) == CR_INVALID_PARAMETER);
  for (UINT32 I = 0; I < CR_PIL_MAX_CLIENTS; ++I) {
    Owners[I].Status = CR_SUCCESS;
    assert(CrPilClientRegister(&Clients, &Owners[I], Quiet, &Owners[I]) == CR_SUCCESS);
  }
  assert(Clients.Count == CR_PIL_MAX_CLIENTS);
  assert(CrPilClientRegister(&Clients, &Owners[8], Quiet, &Owners[8]) == CR_OUT_OF_RESOURCES);
  assert(CrPilClientRegister(&Clients, &Owners[7], Quiet, &Owners[7]) == CR_ALREADY_STARTED);
  assert(CrPilClientUnregister(&Clients, &Owners[0]) == CR_SUCCESS);
  assert(CrPilClientRegister(&Clients, &Owners[7], Quiet, &Owners[7]) == CR_ALREADY_STARTED);
  assert(CrPilClientRegister(&Clients, &Owners[0], Quiet, &Owners[0]) == CR_SUCCESS);
  Owners[0].Status = CR_TIMEOUT;
  Owners[5].Status = CR_NOT_READY;
  Owners[6].Status = (CR_STATUS)0x103; /* NT STATUS_PENDING, nonzero native warning */
  assert(CrPilClientsQuiesce(&Clients) == CR_TIMEOUT);
  assert(Clients.Frozen && !Clients.Dispatching && Clients.Count == CR_PIL_MAX_CLIENTS);
  for (UINT32 I = 0; I < CR_PIL_MAX_CLIENTS; ++I) assert(Owners[I].Calls == 1);
  Owners[0].Status = CR_SUCCESS;
  assert(CrPilClientsQuiesce(&Clients) == CR_NOT_READY);
  assert(Owners[0].Calls == 2 && Owners[5].Calls == 2 && Owners[1].Calls == 1);
  Owners[5].Status = CR_SUCCESS;
  assert(CrPilClientsQuiesce(&Clients) == CR_NOT_READY && !Clients.Entries[6].Quiet);
  Owners[6].Status = CR_SUCCESS;
  assert(CrPilClientsQuiesce(&Clients) == CR_SUCCESS && Owners[6].Calls == 4);
  assert(Owners[5].Calls == 3);
  assert(CrPilClientsQuiesce(&Clients) == CR_SUCCESS && Owners[5].Calls == 3);
  for (UINT32 I = 0; I < CR_PIL_MAX_CLIENTS; ++I)
    assert(CrPilClientUnregister(&Clients, &Owners[I]) == CR_SUCCESS);
  assert(Clients.Count == 0 && CrPilClientsQuiesce(&Clients) == CR_SUCCESS);
  assert(CrPilClientUnregister(&Clients, &Owners[0]) == CR_NOT_FOUND);
  assert(CrPilClientRegister(&Clients, Owners, Quiet, Owners) == CR_BUSY);
  puts("PIL clients: frozen registration, all-consumer drain, failed retry, no replay and reentrancy PASS");
  return 0;
}
