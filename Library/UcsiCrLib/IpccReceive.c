/* Shared MPROC receive owner; Linux qcom-ipcc register semantics.
 * Copyright (c) 2018-2020, The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/IpccReceiveCrLib.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>
#define CONFIG 0x08U
#define RECV 0x10U
#define ENABLE 0x14U
#define DISABLE 0x18U
#define CLEAR 0x1cU
#define EMPTY 0xffffffffU
STATIC BOOLEAN Overlap(CONST VOID *A,UINTN N,CONST VOID *B,UINTN M) {
  UINTN X=(UINTN)A,Y=(UINTN)B;
  return !A || !B || X>~(UINTN)0-N || Y>~(UINTN)0-M || (X<Y+M && Y<X+N);
}
STATIC BOOLEAN Separate(CR_IPCC_RECEIVE *R,CONST VOID *P,UINTN Size,CONST CR_IPCC_RX_LEASE *Allowed) {
  if(!R || ((UINTN)P&7U) || Overlap(P,Size,R,sizeof(*R)))return FALSE;
  for(UINT32 I=0;I<CR_IPCC_RX_ROUTES;++I)
    if(R->Route[I].Identity && R->Route[I].Identity!=Allowed &&
       Overlap(P,Size,R->Route[I].Identity,sizeof(CR_IPCC_RX_LEASE)))return FALSE;
  return TRUE;
}
STATIC UINT32 Id(UINT32 Index){return (3U<<16)|(Index==0?0U:2U);}
STATIC VOID Write(CR_IPCC_RECEIVE *R,UINT32 Offset,UINT32 Value) {
  R->Io.Write32(R->Io.Context,Offset,Value);CrMemoryFence();
}
STATIC CR_IPCC_RX_ROUTE *Find(CR_IPCC_RECEIVE *R,VOID *Owner,CONST CR_IPCC_RX_LEASE *L) {
  if(!R || !R->Initialized || !Owner || !Separate(R,L,sizeof(*L),L) ||
      L->Provider!=R || L->Owner!=Owner || L->Generation!=R->Generation || !L->Token ||
      L->Role<CR_IPCC_RX_GLINK || L->Role>CR_IPCC_RX_SMP2P || L->Reserved)return NULL;
  CR_IPCC_RX_ROUTE *Q=&R->Route[L->Role-1];
  return Q->Identity==L && Q->Owner==Owner && Q->Token==L->Token?Q:NULL;
}
STATIC VOID Notify(CR_IPCC_RX_ROUTE *Q) {
  if(Q->Wake) {
    CR_STATUS S=Q->Wake(Q->WakeContext);
    if(S!=CR_SUCCESS){Q->Error=CR_ERROR(S)?S:CR_DEVICE_ERROR;Q->Closing=TRUE;}
  }
}
STATIC VOID Signal(CR_IPCC_RX_ROUTE *Q) {
  if(Q->Sequence==CR_MAX_UINT64){Q->Error=CR_OUT_OF_RESOURCES;Q->Closing=TRUE;}
  else ++Q->Sequence;
  Q->Pending=TRUE;Notify(Q);
}
CR_STATUS CrIpccReceiveInit(CR_IPCC_RECEIVE *R,CONST CR_IPCC_RX_IO *Io,UINT64 Generation) {
  if(!R || ((UINTN)R&7U) || !Io || Overlap(Io,sizeof(*Io),R,sizeof(*R)) ||
      !Io->Read32 || !Io->Write32 || !Generation)return CR_INVALID_PARAMETER;
  if(R->Initialized || R->Started || R->ConfigOwned || R->Lost)return CR_BUSY;
  if(Generation<=R->Generation)return CR_NOT_READY;
  for(UINT32 I=0;I<CR_IPCC_RX_ROUTES;++I)if(R->Route[I].Identity)return CR_BUSY;
  R->Io=*Io;R->Generation=Generation;R->Initialized=TRUE;R->Sealed=FALSE;
  if(!R->NextToken)R->NextToken=1;
  return CR_SUCCESS;
}
CR_STATUS CrIpccReceiveStart(CR_IPCC_RECEIVE *R) {
  if(!R || !R->Initialized)return CR_INVALID_PARAMETER;
  if(R->Lost || R->Exhausted || R->Faulted)return CR_NOT_READY;
  if(R->Started || R->ConfigOwned)return CR_BUSY;
  for(UINT32 I=0;I<CR_IPCC_RX_ROUTES;++I)if(R->Route[I].Identity)return CR_BUSY;
  UINT32 C=R->Io.Read32(R->Io.Context,CONFIG);
  R->SavedClearOnRead=(C&1U)!=0;R->ConfigOwned=TRUE;
  if(C&1U)Write(R,CONFIG,C&~1U);
  if(R->Io.Read32(R->Io.Context,CONFIG)&1U){R->Sealed=TRUE;return CR_DEVICE_ERROR;}
  R->Sealed=FALSE;R->More=FALSE;R->FullBudgets=0;R->Started=TRUE;return CR_SUCCESS;
}
CR_STATUS CrIpccReceiveBegin(CR_IPCC_RECEIVE *R,VOID *Owner,UINT32 Role,
    CR_IPCC_RX_WAKE Wake,VOID *Context,CR_IPCC_RX_LEASE *L) {
  if(!R || !Owner || !Wake || !Context || Role<CR_IPCC_RX_GLINK || Role>CR_IPCC_RX_SMP2P ||
      !Separate(R,L,sizeof(*L),NULL))return CR_INVALID_PARAMETER;
  if(L->Provider || L->Owner || L->Generation || L->Token || L->Role || L->Reserved)return CR_BUSY;
  if(!R->Initialized || !R->Started || R->Sealed || R->Lost || R->Exhausted)return CR_NOT_READY;
  CR_IPCC_RX_ROUTE *Q=&R->Route[Role-1];if(Q->Identity)return CR_BUSY;
  if(!R->NextToken || R->NextToken==CR_MAX_UINT64){R->Exhausted=TRUE;return CR_OUT_OF_RESOURCES;}
  Q->Identity=L;Q->Owner=Owner;Q->Wake=Wake;Q->WakeContext=Context;Q->Token=R->NextToken++;
  *L=(CR_IPCC_RX_LEASE){R,Owner,R->Generation,Q->Token,Role,0};
  /* Admit an initial masked observation before enabling this role. Pending
   * remote state before registration is therefore sampled, never discarded. */
  Write(R,DISABLE,Id(Role-1));Signal(Q);
  return Q->Error;
}
STATIC VOID Receive(CR_IPCC_RECEIVE *R) {
  R->More=FALSE;
  for(UINT32 I=0;I<CR_IPCC_RX_BUDGET;++I) {
    UINT32 V=R->Io.Read32(R->Io.Context,RECV);
    if(V==EMPTY){R->FullBudgets=0;return;}
    UINT32 Index=V==Id(0)?0:V==Id(1)?1:CR_IPCC_RX_ROUTES;
    CR_IPCC_RX_ROUTE *Q=Index<CR_IPCC_RX_ROUTES?&R->Route[Index]:NULL;
    if(Q && Q->Identity) {
      Write(R,DISABLE,V);Q->Enabled=FALSE;
      if(!Q->Closing){if(R->Received!=CR_MAX_UINT32)++R->Received;Signal(Q);}
    } else if(R->Unknown!=CR_MAX_UINT32)++R->Unknown;
    /* Unknown doorbells are consumed by the sole FIFO owner, without writing
     * any unknown route's enable state or touching its shared-memory data. */
    Write(R,CLEAR,V);
  }
  R->More=TRUE;
  if(++R->FullBudgets>=CR_IPCC_RX_MAX_FULL_BUDGETS) {
    R->Faulted=TRUE;(VOID)CrIpccReceiveSeal(R,FALSE);return;
  }
  for(UINT32 I=0;I<CR_IPCC_RX_ROUTES;++I)if(R->Route[I].Identity && !R->Route[I].Closing) {
    Write(R,DISABLE,Id(I));R->Route[I].Enabled=FALSE;Signal(&R->Route[I]);
  }
}
VOID CrIpccReceiveInterrupt(CR_IPCC_RECEIVE *R) {
  if(!R || !R->Initialized || !R->Started || R->Sealed || R->Lost)return;
  if(R->Irqs!=CR_MAX_UINT32)++R->Irqs;Receive(R);
}
CR_STATUS CrIpccReceiveDrain(CR_IPCC_RECEIVE *R) {
  if(!R||!R->Initialized)return CR_INVALID_PARAMETER;
  if(R->Lost||R->Sealed||R->Faulted)return CR_NOT_READY;
  if(R->More&&R->Started)Receive(R);
  return R->Faulted?CR_NOT_READY:CR_SUCCESS;
}
CR_STATUS CrIpccReceiveObserve(CR_IPCC_RECEIVE *R,VOID *Owner,CONST CR_IPCC_RX_LEASE *L,CR_IPCC_RX_OBSERVATION *Out) {
  CR_IPCC_RX_ROUTE *Q=Find(R,Owner,L);
  if(!Q || !Separate(R,Out,sizeof(*Out),NULL) || Overlap(Out,sizeof(*Out),L,sizeof(*L)))return CR_INVALID_PARAMETER;
  if(R->More && R->Started && !R->Sealed && !R->Lost)Receive(R);
  *Out=(CR_IPCC_RX_OBSERVATION){R->Generation,Q->Token,Q->Sequence,Q->Error,Q->Pending,Q->Closing,R->Lost,R->More,FALSE};
  return CR_SUCCESS;
}
CR_STATUS CrIpccReceiveComplete(CR_IPCC_RECEIVE *R,VOID *Owner,CONST CR_IPCC_RX_LEASE *L,UINT64 Sequence) {
  CR_IPCC_RX_ROUTE *Q=Find(R,Owner,L);if(!Q || !Sequence)return CR_INVALID_PARAMETER;
  if(Q->Error!=CR_SUCCESS)return Q->Error;
  if(!R->Started || R->Sealed || R->Lost || Q->Closing)return CR_NOT_READY;
  if(Q->Sequence!=Sequence || R->More)return CR_NOT_READY;
  if(!Q->Pending)return CR_SUCCESS;
  Q->Pending=FALSE;Q->Enabled=TRUE;Write(R,ENABLE,Id(L->Role-1));return CR_SUCCESS;
}
CR_STATUS CrIpccReceiveClose(CR_IPCC_RECEIVE *R,VOID *Owner,CONST CR_IPCC_RX_LEASE *L) {
  CR_IPCC_RX_ROUTE *Q=Find(R,Owner,L);if(!Q)return CR_INVALID_PARAMETER;
  Q->Closing=TRUE;
  if(!R->Lost && R->Started)Write(R,DISABLE,Id(L->Role-1));
  Q->Enabled=FALSE;return CR_SUCCESS;
}
CR_STATUS CrIpccReceiveEnd(CR_IPCC_RECEIVE *R,VOID *Owner,CR_IPCC_RX_LEASE *L) {
  CR_IPCC_RX_ROUTE *Q=Find(R,Owner,L);if(!Q)return CR_INVALID_PARAMETER;
  if(!Q->Closing)return CR_BUSY;
  /* Caller has already drained its deferred uses. The ISR guard excludes a
   * callback concurrently reading these exact function/context fields. */
  cr_memset(Q,0,sizeof(*Q));cr_memset(L,0,sizeof(*L));return CR_SUCCESS;
}
CR_STATUS CrIpccReceiveSeal(CR_IPCC_RECEIVE *R,BOOLEAN NoMmio) {
  if(!R || !R->Initialized || NoMmio>1)return CR_INVALID_PARAMETER;
  R->Sealed=TRUE;if(NoMmio)R->Lost=TRUE;
  for(UINT32 I=0;I<CR_IPCC_RX_ROUTES;++I) {
    CR_IPCC_RX_ROUTE *Q=&R->Route[I];if(!Q->Identity)continue;
    if(!R->Lost && R->Started)Write(R,DISABLE,Id(I));Q->Enabled=FALSE;
    if(!Q->Closing){Q->Error=CR_NOT_READY;Q->Closing=TRUE;Signal(Q);}
  }
  return CR_SUCCESS;
}
CR_STATUS CrIpccReceiveStop(CR_IPCC_RECEIVE *R) {
  if(!R || !R->Initialized)return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<CR_IPCC_RX_ROUTES;++I)if(R->Route[I].Identity)return CR_BUSY;
  R->Sealed=TRUE;
  /* Source loss cannot certify restoration, even after CPU callbacks drain. */
  if(R->Lost && R->ConfigOwned)return CR_NOT_READY;
  R->Started=FALSE;
  if(R->ConfigOwned) {
    UINT32 C=R->Io.Read32(R->Io.Context,CONFIG);
    C=(C&~1U)|(R->SavedClearOnRead?1U:0U);Write(R,CONFIG,C);
    if((R->Io.Read32(R->Io.Context,CONFIG)&1U)!=(C&1U))return CR_DEVICE_ERROR;
    R->ConfigOwned=FALSE;
  }
  R->More=FALSE;return CR_SUCCESS;
}
CR_STATUS CrIpccReceiveDetach(CR_IPCC_RECEIVE *R) {
  if(!R || !R->Initialized)return CR_INVALID_PARAMETER;
  if(R->Started || R->ConfigOwned || R->Lost)return CR_BUSY;
  for(UINT32 I=0;I<CR_IPCC_RX_ROUTES;++I)if(R->Route[I].Identity)return CR_BUSY;
  cr_memset(&R->Io,0,sizeof(R->Io));R->Initialized=FALSE;return CR_SUCCESS;
}

CR_STATUS CrIpccReceiveRefresh(CR_IPCC_RECEIVE *R) {
  if(!R||!R->Initialized)return CR_INVALID_PARAMETER;
  if(!R->Started||R->Sealed||R->Lost)return CR_NOT_READY;
  for(UINT32 I=0;I<CR_IPCC_RX_ROUTES;++I)if(R->Route[I].Identity&&!R->Route[I].Closing) {
    Write(R,DISABLE,Id(I));R->Route[I].Enabled=FALSE;Signal(&R->Route[I]);
  }
  return CR_SUCCESS;
}
