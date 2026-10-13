/* Independent remote-processor policy. SPDX-License-Identifier: MIT */
#include <Library/SubsystemCrLib.h>

static BOOLEAN Valid(CONST CR_SUBSYSTEM *C) { return C && C->Self==(UINTN)C; }
static BOOLEAN Overlap(CONST VOID *A,UINTN N,CONST VOID *B,UINTN M) {
  UINTN X=(UINTN)A,Y=(UINTN)B;
  return !X || !Y || N>~(UINTN)0-X || M>~(UINTN)0-Y || (X<Y+M && Y<X+N);
}
static BOOLEAN Output(CR_SUBSYSTEM *C,CONST VOID *P,UINTN N) {
  UINT32 I;
  if(((UINTN)P&7) || Overlap(C,sizeof(*C),P,N))return FALSE;
  for(I=0;I<CR_SUBSYSTEM_CLIENTS;++I)
    if(C->Clients[I].ExactLease && Overlap(C->Clients[I].ExactLease,sizeof(CR_SUBSYSTEM_LEASE),P,N))return FALSE;
  return TRUE;
}
static VOID Close(CR_SUBSYSTEM *C,CR_STATUS Error,BOOLEAN Fault) {
  if(Fault){C->Faulted=TRUE;C->LastError=Error;}
  if(!C->Closing) {
    C->Closing=TRUE;
    if(C->StopTicket==CR_MAX_UINT64 || C->Sequence==CR_MAX_UINT64) {
      C->Exhausted=TRUE;C->Faulted=TRUE;C->LastError=CR_OUT_OF_RESOURCES;
    } else {++C->StopTicket;++C->Sequence;}
  }
  C->Phase=CrSubsystemClosing;
}
static CR_SUBSYSTEM_CLIENT *Find(CR_SUBSYSTEM *C,CONST VOID *Owner,CONST CR_SUBSYSTEM_LEASE *L) {
  UINT32 I;
  if(!Owner || !L || ((UINTN)L&7) || Overlap(C,sizeof(*C),L,sizeof(*L)))return NULL;
  for(I=0;I<CR_SUBSYSTEM_CLIENTS;++I) {
    CR_SUBSYSTEM_CLIENT *S=&C->Clients[I];
    if(S->ExactLease==L && S->Owner==Owner && L->Provider==C && L->Token==S->Token &&
       L->Instance==S->Instance && S->Token && S->Instance)return S;
  }
  return NULL;
}
CR_STATUS CrSubsystemInit(CR_SUBSYSTEM *C) {
  UINTN I;CONST UINT8 *P=(CONST UINT8 *)C;
  if(!C || ((UINTN)C&7))return CR_INVALID_PARAMETER;
  for(I=0;I<sizeof(*C);++I)if(P[I])return CR_ALREADY_STARTED;
  C->Self=(UINTN)C;C->NextToken=1;C->NextAction=1;C->Sequence=1;
  return CR_SUCCESS;
}
CR_STATUS CrSubsystemDemand(CR_SUBSYSTEM *C,BOOLEAN Desired) {
  if(!Valid(C))return CR_INVALID_PARAMETER;
  C->Desired=!!Desired;
  if(!Desired && (C->Observation.ControllerOwned || C->Pending.Serial || C->Phase==CrSubsystemRunning))Close(C,CR_SUCCESS,FALSE);
  return CR_SUCCESS;
}
CR_STATUS CrSubsystemProviderLost(CR_SUBSYSTEM *C) {
  if(!Valid(C))return CR_INVALID_PARAMETER;
  if(C->ProviderLost)return CR_SUCCESS;
  C->ProviderLost=TRUE;Close(C,CR_NOT_READY,TRUE);return CR_SUCCESS;
}
BOOLEAN CrSubsystemHasClients(CONST CR_SUBSYSTEM *C) {
  UINT32 I;if(!Valid(C))return FALSE;
  for(I=0;I<CR_SUBSYSTEM_CLIENTS;++I)if(C->Clients[I].ExactLease)return TRUE;
  return FALSE;
}
BOOLEAN CrSubsystemCanRelease(CONST CR_SUBSYSTEM *C) {
  return Valid(C) && !C->Pending.Serial && !C->Observation.ControllerOwned &&
    !C->Observation.ImageOwned && !C->Malformed && !CrSubsystemHasClients(C);
}
CR_STATUS CrSubsystemNext(CR_SUBSYSTEM *C,CR_SUBSYSTEM_ACTION *A) {
  UINT32 I,Kind=CrSubsystemNoAction;
  if(!Valid(C) || !Output(C,A,sizeof(*A)))return CR_INVALID_PARAMETER;
  if(C->Pending.Serial)return CR_BUSY;
  if(C->Malformed)return CR_COMPROMISED_DATA;
  if(C->Closing) {
    for(I=0;I<CR_SUBSYSTEM_CLIENTS;++I)
      if(C->Clients[I].ExactLease && C->Clients[I].DrainTicket!=C->StopTicket)return CR_BUSY;
    if(C->Observation.ImageOwned)Kind=CrSubsystemRetireImage;
    else if(C->Observation.ControllerOwned)Kind=CrSubsystemEndController;
    else {
      C->Closing=FALSE;C->Phase=C->Faulted?CrSubsystemFaulted:CrSubsystemStopped;
      return CR_NOT_READY;
    }
  } else if(C->Desired && !C->Faulted && !C->Exhausted) {
    if(!C->Observation.ControllerOwned && CrSubsystemHasClients(C))return CR_BUSY;
    if(!C->Observation.ControllerOwned)Kind=CrSubsystemBeginController;
    else if(!C->Observation.ImageOwned)Kind=CrSubsystemStartImage;
    else if(C->Phase==CrSubsystemRunning)Kind=CrSubsystemQueryImage;
  }
  if(!Kind)return CR_NOT_READY;
  if(!C->NextAction || C->NextAction==CR_MAX_UINT64) {
    C->Exhausted=TRUE;Close(C,CR_OUT_OF_RESOURCES,TRUE);return CR_OUT_OF_RESOURCES;
  }
  C->Pending.Kind=Kind;C->Pending.Serial=C->NextAction++;
  if(Kind==CrSubsystemBeginController || Kind==CrSubsystemStartImage)C->Phase=CrSubsystemStarting;
  else if(Kind==CrSubsystemRetireImage || Kind==CrSubsystemEndController)C->Phase=CrSubsystemRetiring;
  *A=C->Pending;return CR_SUCCESS;
}
static BOOLEAN Shape(CONST CR_SUBSYSTEM_OBSERVATION *O) {
  if(O->ReceiptMalformed)return FALSE;
  if(O->ImageOwned && (!O->ControllerOwned || !O->ImageSerial))return FALSE;
  /* BootSerial is old-instance identity and survives PAS/resource retirement.
   * Only current admission, not historical identity, implies owned auth. */
  if(O->BootSerial && !O->BindingSerial)return FALSE;
  if(O->AdmissionOpen && (!O->BootSerial || !O->Authenticated || !O->Owned || O->Inherited))return FALSE;
  if(O->Inherited && O->Owned)return FALSE;
  if(O->MechanismRetired && (O->ImageOwned || O->RetainedEffects))return FALSE;
  return TRUE;
}
CR_STATUS CrSubsystemComplete(CR_SUBSYSTEM *C,CONST CR_SUBSYSTEM_ACTION *A,
                            CR_STATUS Result,CONST CR_SUBSYSTEM_OBSERVATION *O) {
  UINT32 Kind;BOOLEAN Bad;
  if(!Valid(C) || !Output(C,A,sizeof(*A)) || !Output(C,O,sizeof(*O)) ||
     Overlap(A,sizeof(*A),O,sizeof(*O)))return CR_INVALID_PARAMETER;
  if(!A->Serial || A->Serial!=C->Pending.Serial || A->Kind!=C->Pending.Kind)return CR_NOT_FOUND;
  Kind=A->Kind;Bad=!Shape(O);
  /* An observed loss of owned receipt is valid only after its exact terminal
   * operation. Partial output must never erase previously owned effects. */
  if(C->Observation.ControllerOwned && !O->ControllerOwned &&
     !(Kind==CrSubsystemEndController && Result==CR_SUCCESS && !O->ImageOwned))Bad=TRUE;
  if(C->Observation.ImageOwned && !O->ImageOwned &&
     !(Kind==CrSubsystemRetireImage && Result==CR_SUCCESS && O->MechanismRetired))Bad=TRUE;
  if(C->Observation.ImageOwned && O->ImageSerial!=C->Observation.ImageSerial)Bad=TRUE;
  if(Kind==CrSubsystemBeginController && O->ImageOwned)Bad=TRUE;
  if(Kind==CrSubsystemEndController && O->ImageOwned)Bad=TRUE;
  if(Kind==CrSubsystemQueryImage && (O->BindingSerial!=C->Observation.BindingSerial ||
      O->BootSerial!=C->Observation.BootSerial))Bad=TRUE;
  C->Pending.Serial=0;C->Pending.Kind=0;
  if(Bad) {
    /* Preserve every previously known effect, and pin unknown new ones. */
    C->Observation.ControllerOwned|=O->ControllerOwned;C->Observation.ImageOwned|=O->ImageOwned;
    C->Malformed=TRUE;Close(C,CR_COMPROMISED_DATA,TRUE);return CR_COMPROMISED_DATA;
  }
  C->Observation=*O;
  if(Kind==CrSubsystemBeginController) {
    if(Result!=CR_SUCCESS) {
      C->LastError=Result;
      if(O->ControllerOwned)Close(C,Result,TRUE);else C->Phase=CrSubsystemStopped;
    } else if(!O->ControllerOwned) {C->Malformed=TRUE;Close(C,CR_COMPROMISED_DATA,TRUE);return CR_COMPROMISED_DATA;}
  } else if(Kind==CrSubsystemStartImage) {
    if(Result==CR_SUCCESS && O->ImageOwned && O->Loaded && O->Ready && O->AdmissionOpen && !O->RecoveryRequired) {
      if(C->Instance==CR_MAX_UINT64 || C->Sequence==CR_MAX_UINT64) {
        C->Exhausted=TRUE;Close(C,CR_OUT_OF_RESOURCES,TRUE);return CR_OUT_OF_RESOURCES;
      }
      ++C->Instance;++C->Sequence;
      C->Phase=C->Closing?CrSubsystemClosing:CrSubsystemRunning;
      C->LastError=CR_SUCCESS;
    } else Close(C,Result==CR_SUCCESS?CR_NOT_READY:Result,TRUE);
  } else if(Kind==CrSubsystemQueryImage) {
    if(Result!=CR_SUCCESS || !O->Loaded || !O->Ready || !O->AdmissionOpen || O->RecoveryRequired)
      Close(C,Result==CR_SUCCESS?CR_NOT_READY:Result,TRUE);
  } else if(Kind==CrSubsystemRetireImage || Kind==CrSubsystemEndController) {
    if(Result!=CR_SUCCESS){C->LastError=Result;C->Phase=CrSubsystemRetiring;}
    else if((Kind==CrSubsystemRetireImage && O->ImageOwned) ||
            (Kind==CrSubsystemEndController && O->ControllerOwned)) {
      C->Malformed=TRUE;Close(C,CR_COMPROMISED_DATA,TRUE);return CR_COMPROMISED_DATA;
    }
  }
  if(!C->Desired && O->ControllerOwned)Close(C,CR_SUCCESS,FALSE);
  return Result;
}
CR_STATUS CrSubsystemBegin(CR_SUBSYSTEM *C,CONST VOID *Owner,CR_SUBSYSTEM_LEASE *L) {
  UINT32 I;
  if(!Valid(C) || !Owner || !Output(C,L,sizeof(*L)))return CR_INVALID_PARAMETER;
  if(L->Provider || L->Token || L->Instance)return CR_ALREADY_STARTED;
  if(!C->Desired || C->Closing || C->Faulted || C->ProviderLost || C->Phase!=CrSubsystemRunning)return CR_NOT_READY;
  if(!C->NextToken || C->NextToken==CR_MAX_UINT64) {C->Exhausted=TRUE;Close(C,CR_OUT_OF_RESOURCES,TRUE);return CR_OUT_OF_RESOURCES;}
  for(I=0;I<CR_SUBSYSTEM_CLIENTS;++I)if(!C->Clients[I].ExactLease) {
    CR_SUBSYSTEM_CLIENT *S=&C->Clients[I];
    S->Owner=Owner;S->ExactLease=L;S->Token=C->NextToken++;S->Instance=C->Instance;S->DrainTicket=0;
    L->Provider=C;L->Token=S->Token;L->Instance=S->Instance;return CR_SUCCESS;
  }
  return CR_OUT_OF_RESOURCES;
}
CR_STATUS CrSubsystemCheck(CR_SUBSYSTEM *C,CONST VOID *Owner,CONST CR_SUBSYSTEM_LEASE *L,
                         UINT64 Instance,UINT64 Boot) {
  if(!Valid(C))return CR_INVALID_PARAMETER;
  if(!Find(C,Owner,L))return CR_NOT_FOUND;
  if(!Instance || Instance!=C->Instance || Instance!=L->Instance)return CR_NOT_FOUND;
  if(C->Closing || C->Faulted || C->ProviderLost || !C->Desired || C->Phase!=CrSubsystemRunning)return CR_NOT_READY;
  if(Boot && Boot!=C->Observation.BootSerial)return CR_NOT_READY;
  return CR_SUCCESS;
}
BOOLEAN CrSubsystemOwns(CR_SUBSYSTEM *C,CONST VOID *Owner,CONST CR_SUBSYSTEM_LEASE *L) {
  return Valid(C) && Find(C,Owner,L)!=NULL;
}
CR_STATUS CrSubsystemDrain(CR_SUBSYSTEM *C,CONST VOID *Owner,CONST CR_SUBSYSTEM_LEASE *L,UINT64 Ticket) {
  CR_SUBSYSTEM_CLIENT *S;if(!Valid(C))return CR_INVALID_PARAMETER;S=Find(C,Owner,L);
  if(!S)return CR_NOT_FOUND;
  if(!C->Closing || !Ticket || Ticket!=C->StopTicket || C->Exhausted)return CR_NOT_READY;
  S->DrainTicket=Ticket;return CR_SUCCESS;
}
CR_STATUS CrSubsystemEnd(CR_SUBSYSTEM *C,CONST VOID *Owner,CR_SUBSYSTEM_LEASE *L) {
  CR_SUBSYSTEM_CLIENT *S;if(!Valid(C))return CR_INVALID_PARAMETER;S=Find(C,Owner,L);
  if(!S)return CR_NOT_FOUND;
  S->Owner=NULL;S->ExactLease=NULL;S->Token=0;S->Instance=0;S->DrainTicket=0;
  L->Provider=NULL;L->Token=0;L->Instance=0;return CR_SUCCESS;
}

CR_STATUS CrSubsystemRemoteFault(CR_SUBSYSTEM *C,CR_STATUS Error) {
  if(!Valid(C)||Error==CR_SUCCESS)return CR_INVALID_PARAMETER;
  Close(C,CR_ERROR(Error)?Error:CR_NOT_READY,TRUE);return CR_SUCCESS;
}
