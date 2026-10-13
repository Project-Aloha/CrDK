/** Synchronous PIL consumer retirement. SPDX-License-Identifier: MIT */
#include <Library/PILCrClients.h>

CR_STATUS CrPilClientRegister(CR_PIL_CLIENTS *Clients, VOID *Owner,
                             CR_PIL_CLIENT_QUIESCE Quiesce, VOID *Context)
{
  UINT32 Index, Free = CR_PIL_MAX_CLIENTS;
  if (Clients == NULL || Owner == NULL || Quiesce == NULL) return CR_INVALID_PARAMETER;
  if (Clients->Frozen || Clients->Dispatching) return CR_BUSY;
  for (Index = 0; Index < CR_PIL_MAX_CLIENTS; ++Index) {
    if (Clients->Entries[Index].Owner == Owner) return CR_ALREADY_STARTED;
    if (Clients->Entries[Index].Owner == NULL && Free == CR_PIL_MAX_CLIENTS) Free = Index;
  }
  if (Free == CR_PIL_MAX_CLIENTS) return CR_OUT_OF_RESOURCES;
  Clients->Entries[Free] = (CR_PIL_CLIENT){Owner, Context, Quiesce, FALSE};
  ++Clients->Count;
  return CR_SUCCESS;
}

CR_STATUS CrPilClientUnregister(CR_PIL_CLIENTS *Clients, VOID *Owner)
{
  UINT32 Index;
  if (Clients == NULL || Owner == NULL) return CR_INVALID_PARAMETER;
  if (Clients->Dispatching) return CR_BUSY;
  for (Index = 0; Index < CR_PIL_MAX_CLIENTS; ++Index) {
    if (Clients->Entries[Index].Owner != Owner) continue;
    Clients->Entries[Index] = (CR_PIL_CLIENT){0};
    --Clients->Count;
    return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}

CR_STATUS CrPilClientsQuiesce(CR_PIL_CLIENTS *Clients)
{
  CR_STATUS Status, Result = CR_SUCCESS;
  UINT32 Index;
  if (Clients == NULL) return CR_INVALID_PARAMETER;
  if (Clients->Dispatching) return CR_BUSY;
  Clients->Frozen = Clients->Dispatching = TRUE;
  for (Index = 0; Index < CR_PIL_MAX_CLIENTS; ++Index) {
    CR_PIL_CLIENT *Client = &Clients->Entries[Index];
    if (Client->Owner == NULL || Client->Quiet) continue;
    Status = Client->Quiesce(Client->Context);
    if (Status == CR_SUCCESS) Client->Quiet = TRUE;
    /* NT STATUS_PENDING and native warning statuses do not prove that the
       caller has synchronously retired its writer/IRQ ownership. */
    else if (Result == CR_SUCCESS) Result = CR_ERROR(Status) ? Status : CR_NOT_READY;
  }
  Clients->Dispatching = FALSE;
  return Result;
}
