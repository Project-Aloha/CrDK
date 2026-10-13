/* Independent implementation of the published SMP2P wire format.
 * SPDX-License-Identifier: MIT
 * See README.md for pinned Linux/Android format references and their licenses.
 */
#include <Library/Smp2pObserveCrLib.h>

static VOID Zero(VOID *P, UINTN N) {
  UINT8 *B=(UINT8 *)P; while(N--) *B++=0;
}
static BOOLEAN Equal(CONST VOID *A, CONST VOID *B, UINTN N) {
  CONST UINT8 *X=(CONST UINT8 *)A,*Y=(CONST UINT8 *)B;
  while(N--) if(*X++!=*Y++)return FALSE; return TRUE;
}
static BOOLEAN Empty(CONST VOID *P, UINTN N) {
  CONST UINT8 *B=(CONST UINT8 *)P; while(N--)if(*B++)return FALSE;return TRUE;
}
static BOOLEAN Overlap(CONST VOID *A,UINTN N,CONST VOID *B,UINTN M) {
  UINTN X=(UINTN)A,Y=(UINTN)B;
  return !X || !Y || N>~(UINTN)0-X || M>~(UINTN)0-Y || (X<Y+M && Y<X+N);
}
static BOOLEAN SourceValid(CONST CR_SMP2P_SOURCE *S) {
  return S && !((UINTN)S&7) && S->Self==(UINTN)S;
}
static BOOLEAN JournalValid(CONST CR_SMP2P_JOURNAL *J) {
  return J && !((UINTN)J&7) && J->Self==(UINTN)J;
}
static BOOLEAN ViewRange(CONST CR_SMP2P_VIEW *V) {
  return V->Address && V->Size && V->Size<=~(UINTN)0-(UINTN)V->Address;
}
static BOOLEAN JournalOutput(CONST CR_SMP2P_JOURNAL *J,CONST VOID *P,UINTN N) {
  if(((UINTN)P&7) || Overlap(J,sizeof(*J),P,N))return FALSE;
  if(J->ExactLease && Overlap(J->ExactLease,sizeof(*J->ExactLease),P,N))return FALSE;
  if(J->Source) {
    if(Overlap(J->Source,sizeof(*J->Source),P,N))return FALSE;
    if(J->Source->ExactSnapshot && Overlap(J->Source->ExactSnapshot,
        sizeof(*J->Source->ExactSnapshot),P,N))return FALSE;
    if(ViewRange(&J->Source->Receipt.View) && Overlap(J->Source->Receipt.View.Address,
        J->Source->Receipt.View.Size,P,N))return FALSE;
  }
  return TRUE;
}
static BOOLEAN SourceOutput(CONST CR_SMP2P_SOURCE *S,CONST VOID *P,UINTN N) {
  CONST CR_SMP2P_JOURNAL *J=(CONST CR_SMP2P_JOURNAL *)S->Journal;
  if(((UINTN)P&7) || Overlap(S,sizeof(*S),P,N))return FALSE;
  if(S->ExactSnapshot && P!=S->ExactSnapshot &&
     Overlap(S->ExactSnapshot,sizeof(*S->ExactSnapshot),P,N))return FALSE;
  if(ViewRange(&S->Receipt.View) && Overlap(S->Receipt.View.Address,
     S->Receipt.View.Size,P,N))return FALSE;
  if(J && (Overlap(J,sizeof(*J),P,N) ||
      (J->ExactLease && Overlap(J->ExactLease,sizeof(*J->ExactLease),P,N))))return FALSE;
  return TRUE;
}
static BOOLEAN Owns(CONST CR_SMP2P_JOURNAL *J,CONST VOID *Owner,CONST CR_SMP2P_LEASE *L) {
  return JournalValid(J) && Owner && Owner==J->Owner && L && L==J->ExactLease &&
    !((UINTN)L&7) && L->Journal==J && L->Owner==Owner &&
    L->Token==J->State.Token && L->Incarnation==J->State.Incarnation &&
    L->ControllerSerial==J->State.ControllerSerial &&
    L->StartActionSerial==J->State.StartActionSerial && J->State.Armed &&
    SourceValid(J->Source) && J->Source->Journal==J && J->Source->Attached;
}
CR_STATUS CrSmp2pSourceInit(CR_SMP2P_SOURCE *S) {
  if(!S || ((UINTN)S&7))return CR_INVALID_PARAMETER;
  if(!Empty(S,sizeof(*S)))return CR_ALREADY_STARTED;
  S->Self=(UINTN)S;return CR_SUCCESS;
}
CR_STATUS CrSmp2pSourceAttach(CR_SMP2P_SOURCE *S,CONST CR_SMP2P_IO *Io) {
  if(!SourceValid(S) || !Io || ((UINTN)Io&7) ||
     Overlap(S,sizeof(*S),Io,sizeof(*Io)))return CR_INVALID_PARAMETER;
  if(S->Attached || S->Journal)return CR_BUSY;
  if(S->Exhausted)return CR_OUT_OF_RESOURCES;
  if(!Io->Context || !Io->Probe || !Io->Read32 || !Io->Fence ||
     Io->Host!=CR_SMP2P_HOST || Io->Item!=CR_SMP2P_ITEM ||
     !Io->Incarnation || Io->Incarnation<=S->LastIncarnation)return CR_INVALID_PARAMETER;
  S->Io=*Io;S->LastIncarnation=Io->Incarnation;S->Attached=TRUE;S->Lost=FALSE;
  S->ExactSnapshot=NULL;Zero(&S->Receipt,sizeof(S->Receipt));return CR_SUCCESS;
}
CR_STATUS CrSmp2pSourceDetach(CR_SMP2P_SOURCE *S) {
  if(!SourceValid(S))return CR_INVALID_PARAMETER;
  if(S->Journal)return CR_BUSY;
  S->Attached=FALSE;S->ExactSnapshot=NULL;Zero(&S->Io,sizeof(S->Io));
  Zero(&S->Receipt,sizeof(S->Receipt));return CR_SUCCESS;
}
typedef struct {
  CR_SMP2P_VIEW View;
  UINT32 Words[85],Tail[5],Count,Outcome,Reads;
  CR_STATUS Error;
  BOOLEAN Bracketed, Aliased;
} SMP_PASS;
static VOID Failure(SMP_PASS *P,CR_STATUS E) {
  P->Error=E;
  if(E==CR_NOT_FOUND)P->Outcome=CrSmp2pMissing;
  else if(E==CR_NOT_READY || E==CR_ABORT)P->Outcome=CrSmp2pSourceLost;
  else if(E==CR_BAD_BUFFER_SIZE || E==CR_BUFFER_TOO_SMALL)P->Outcome=CrSmp2pMalformed;
  else {P->Outcome=CrSmp2pReadFailure;if(!CR_ERROR(E))P->Error=CR_DEVICE_ERROR;}
}
static VOID ParseHeader(SMP_PASS *P) {
  UINT32 Version=P->Words[1]&255,Total=P->Words[3]&65535,Valid=P->Words[3]>>16;
  P->Outcome=CrSmp2pValid;P->Error=CR_SUCCESS;
  if(!Version) {P->Outcome=CrSmp2pMissing;P->Error=CR_NOT_FOUND;return;}
  if(P->Words[0]!=0x504d5324U) {P->Outcome=CrSmp2pMalformed;P->Error=CR_COMPROMISED_DATA;return;}
  if(Version!=1) {P->Outcome=CrSmp2pUnsupported;P->Error=CR_UNSUPPORTED;return;}
  if(P->Words[2]!=CR_SMP2P_HOST || !Total || Total>CR_SMP2P_MAX_ENTRIES ||
     Valid>Total || P->View.Size<CR_SMP2P_HEADER_BYTES+Total*20U) {
    P->Outcome=CrSmp2pMalformed;P->Error=CR_COMPROMISED_DATA;return;
  }
  P->Count=5+Valid*5;
}
static BOOLEAN ViewAliased(CONST CR_SMP2P_SOURCE *S,CONST CR_SMP2P_SNAPSHOT *Out,
                        CONST CR_SMP2P_VIEW *V) {
  CONST CR_SMP2P_JOURNAL *J=(CONST CR_SMP2P_JOURNAL *)S->Journal;
  if(Overlap(V->Address,V->Size,S,sizeof(*S)) ||
     Overlap(V->Address,V->Size,Out,sizeof(*Out)))return TRUE;
  if(J && (Overlap(V->Address,V->Size,J,sizeof(*J)) ||
     Overlap(V->Address,V->Size,J->ExactLease,sizeof(*J->ExactLease))))return TRUE;
  return FALSE;
}
static VOID Pass(CR_SMP2P_SOURCE *S,CONST CR_SMP2P_SNAPSHOT *Out,SMP_PASS *P) {
  UINT32 I;CR_STATUS R;Zero(P,sizeof(*P));P->Count=5;
  R=S->Io.Probe(S->Io.Context,&P->View);
  if(ViewRange(&P->View) && ViewAliased(S,Out,&P->View)) {P->Aliased=TRUE;return;}
  if(R!=CR_SUCCESS) {Failure(P,R);return;}
  if(!ViewRange(&P->View) || ((UINTN)P->View.Address&3) || P->View.Size<20) {
    P->Outcome=CrSmp2pMalformed;P->Error=CR_COMPROMISED_DATA;return;
  }
  S->Io.Fence(S->Io.Context);
  for(I=0;I<5;++I) {
    ++P->Reads;R=S->Io.Read32(S->Io.Context,&P->View,I*4,&P->Words[I]);
    if(R!=CR_SUCCESS){Failure(P,R);return;}
  }
  ParseHeader(P);S->Io.Fence(S->Io.Context);
  for(I=5;I<P->Count;++I) {
    ++P->Reads;R=S->Io.Read32(S->Io.Context,&P->View,I*4,&P->Words[I]);
    if(R!=CR_SUCCESS){Failure(P,R);return;}
  }
  S->Io.Fence(S->Io.Context);
  for(I=0;I<5;++I) {
    ++P->Reads;R=S->Io.Read32(S->Io.Context,&P->View,I*4,&P->Tail[I]);
    if(R!=CR_SUCCESS){Failure(P,R);return;}
  }
  S->Io.Fence(S->Io.Context);P->Bracketed=TRUE;
  if(!Equal(P->Words,P->Tail,sizeof(P->Tail))) {
    P->Outcome=CrSmp2pTransient;P->Error=CR_NOT_READY;
  }
}
static BOOLEAN NameMatches(CONST UINT32 *W) {
  static CONST UINT8 Name[16]={'s','l','a','v','e','-','k','e','r','n','e','l',0,0,0,0};
  UINT32 I;for(I=0;I<16;++I)if(((W[I/4]>>(8*(I%4)))&255)!=Name[I])return FALSE;
  return TRUE;
}
CR_STATUS CrSmp2pCapture(CR_SMP2P_SOURCE *S,UINT64 EventSequence,CR_SMP2P_SNAPSHOT *Out) {
  SMP_PASS A,B;CR_SMP2P_SNAPSHOT V;UINT32 I,Found=0;
  if(!SourceValid(S) || !SourceOutput(S,Out,sizeof(*Out)) || !EventSequence)return CR_INVALID_PARAMETER;
  if(!S->Attached)return CR_NOT_READY;
  if(S->Exhausted || S->CaptureSerial==CR_MAX_UINT64) {
    S->Exhausted=TRUE;S->ExactSnapshot=NULL;return CR_OUT_OF_RESOURCES;
  }
  Zero(&V,sizeof(V));V.Self=(UINTN)Out;V.Source=S;V.Incarnation=S->Io.Incarnation;
  V.EventSequence=EventSequence;V.EntryIndex=CR_MAX_UINT32;
  if(S->Lost) {
    V.CaptureSerial=++S->CaptureSerial;
    V.Outcome=CrSmp2pSourceLost;V.Error=CR_NOT_READY;
    *Out=V;S->Receipt=V;S->ExactSnapshot=Out;return V.Error;
  }
  Pass(S,Out,&A);V.View=A.View;V.Reads=A.Reads;
  if(A.Aliased)return CR_INVALID_PARAMETER;
  for(I=0;I<5;++I){V.Header[I]=A.Words[I];V.ConfirmHeader[I]=A.Tail[I];}
  V.Outcome=A.Outcome;V.Error=A.Error;
  if(A.Bracketed && A.Outcome!=CrSmp2pTransient) {
    Pass(S,Out,&B);V.Reads+=B.Reads;
    if(B.Aliased)return CR_INVALID_PARAMETER;
    for(I=0;I<5;++I)V.ConfirmHeader[I]=B.Tail[I];
    if(B.Outcome==CrSmp2pSourceLost || B.Outcome==CrSmp2pReadFailure) {
      V.Outcome=B.Outcome;V.Error=B.Error;
    } else if(!B.Bracketed || B.Outcome!=A.Outcome ||
        A.View.Address!=B.View.Address || A.View.Size!=B.View.Size ||
        A.Count!=B.Count || !Equal(A.Words,B.Words,A.Count*4U)) {
      V.Outcome=CrSmp2pTransient;V.Error=CR_NOT_READY;
    } else {
      V.Stable=TRUE;
      if(V.Outcome==CrSmp2pValid) {
        for(I=5;I<A.Count;I+=5)if(NameMatches(&A.Words[I])) {
          ++Found;V.EntryIndex=(I-5)/5;V.Value=A.Words[I+4];
        }
        if(!Found){V.Outcome=CrSmp2pMissing;V.Error=CR_NOT_FOUND;}
        if(Found>1){V.Outcome=CrSmp2pMalformed;V.Error=CR_COMPROMISED_DATA;}
      }
    }
  }
  V.CaptureSerial=++S->CaptureSerial;
  if(V.Outcome==CrSmp2pSourceLost)S->Lost=TRUE;
  *Out=V;S->Receipt=V;S->ExactSnapshot=Out;return V.Error;
}
CR_STATUS CrSmp2pJournalInit(CR_SMP2P_JOURNAL *J) {
  if(!J || ((UINTN)J&7))return CR_INVALID_PARAMETER;
  if(!Empty(J,sizeof(*J)))return CR_ALREADY_STARTED;
  J->Self=(UINTN)J;J->NextToken=1;return CR_SUCCESS;
}
CR_STATUS CrSmp2pArm(CR_SMP2P_JOURNAL *J,CONST VOID *Owner,CR_SMP2P_SOURCE *S,
                    UINT64 Controller,UINT64 Action,UINT64 Sequence,CR_SMP2P_LEASE *L) {
  if(!JournalValid(J) || !Owner || !SourceValid(S) || !S->Attached ||
     !JournalOutput(J,L,sizeof(*L)) || Overlap(J,sizeof(*J),S,sizeof(*S)) ||
     Overlap(S,sizeof(*S),L,sizeof(*L)) ||
     (S->ExactSnapshot && Overlap(S->ExactSnapshot,sizeof(*S->ExactSnapshot),L,sizeof(*L))) ||
     !Empty(L,sizeof(*L)) ||
     !Controller || !Action || !Sequence || Sequence==CR_MAX_UINT64 ||
     S->Io.Incarnation<=J->LastIncarnation || Controller<J->LastController ||
     Action<=J->LastStartAction)return CR_INVALID_PARAMETER;
  if(J->ExactLease || S->Journal)return CR_BUSY;
  if(S->Lost)return CR_NOT_READY;
  if(J->Exhausted || S->Exhausted || J->NextToken==CR_MAX_UINT64 || !J->NextToken)return CR_OUT_OF_RESOURCES;
  Zero(&J->State,sizeof(J->State));Zero(&J->Previous,sizeof(J->Previous));
  J->HavePrevious=FALSE;J->SeenReady=FALSE;J->Source=S;J->Owner=Owner;J->ExactLease=L;
  J->LastIncarnation=S->Io.Incarnation;J->LastController=Controller;J->LastStartAction=Action;
  J->LastRecordedCapture=S->CaptureSerial;
  J->State.Token=J->NextToken++;J->State.Incarnation=S->Io.Incarnation;
  J->State.ControllerSerial=Controller;J->State.StartActionSerial=Action;
  J->State.EventSequence=Sequence;J->State.Armed=TRUE;J->State.Pending=TRUE;
  L->Journal=J;L->Owner=Owner;L->Token=J->State.Token;L->Incarnation=S->Io.Incarnation;
  L->ControllerSerial=Controller;L->StartActionSerial=Action;S->Journal=J;return CR_SUCCESS;
}
static VOID Fault(CR_SMP2P_JOURNAL *J,UINT32 Reasons,CR_STATUS Error) {
  if(!J->State.Faulted)J->State.Error=Error;
  J->State.FaultReasons|=Reasons;J->State.Faulted=TRUE;
  J->State.Pending=TRUE;J->State.FreshAfterBind=FALSE;
}
CR_STATUS CrSmp2pRecord(CR_SMP2P_JOURNAL *J,CONST VOID *Owner,
                       CONST CR_SMP2P_LEASE *L,CONST CR_SMP2P_SNAPSHOT *V) {
  CR_SMP2P_EVENT E;UINT32 Reasons=0,Old;CR_SMP2P_SOURCE *S;
  if(!Owns(J,Owner,L) || !V || ((UINTN)V&7))return CR_INVALID_PARAMETER;
  S=J->Source;
  if(V!=S->ExactSnapshot || V->Self!=(UINTN)V || V->Source!=S ||
     !Equal(V,&S->Receipt,sizeof(*V)) || V->Incarnation!=J->State.Incarnation ||
     V->CaptureSerial<=J->LastRecordedCapture)return CR_INVALID_PARAMETER;
  if(V->EventSequence<J->State.EventSequence)return CR_NOT_READY;
  if(J->Exhausted || J->Sequence==CR_MAX_UINT64) {
    J->Exhausted=TRUE;Fault(J,CR_SMP2P_FAULT_EXHAUSTED,CR_OUT_OF_RESOURCES);return CR_OUT_OF_RESOURCES;
  }
  Old=J->State.Value;J->LastRecordedCapture=V->CaptureSerial;
  J->State.CaptureSerial=V->CaptureSerial;J->State.EventSequence=V->EventSequence;
  J->State.Pending=TRUE;J->State.Stable=FALSE;J->State.FreshAfterBind=FALSE;
  if(V->EventSequence==CR_MAX_UINT64)Reasons|=CR_SMP2P_FAULT_EXHAUSTED;
  if(V->Outcome==CrSmp2pValid && V->Stable) {
    J->State.Stable=TRUE;J->State.Value=V->Value;
    J->State.ReadyObserved=!!(V->Value&CR_SMP2P_READY);
    if(V->Value&CR_SMP2P_FATAL)Reasons|=CR_SMP2P_FAULT_FATAL;
    if(J->SeenReady && !(V->Value&CR_SMP2P_READY))Reasons|=CR_SMP2P_FAULT_READY_LOST;
    if(J->HavePrevious) {
      CONST CR_SMP2P_SNAPSHOT *P=&J->Previous;
      if(((P->Header[4]^V->Header[4])&1) && ((P->Header[1]>>8)&1))Reasons|=CR_SMP2P_FAULT_RESTART;
      if(P->Header[1]!=V->Header[1] || P->Header[2]!=V->Header[2] ||
         (P->Header[3]&65535)!=(V->Header[3]&65535) ||
         (P->Header[3]>>16)>(V->Header[3]>>16) || P->EntryIndex!=V->EntryIndex ||
         P->View.Address!=V->View.Address)Reasons|=CR_SMP2P_FAULT_IDENTITY;
    }
    J->SeenReady|=J->State.ReadyObserved;J->Previous=*V;J->HavePrevious=TRUE;
    if(!Reasons && !J->State.Faulted) {
      J->State.Error=CR_SUCCESS;
      J->State.FreshAfterBind=J->State.Bound && V->CaptureSerial>J->State.BindCaptureSerial;
      if(!J->State.Bound || J->State.FreshAfterBind) {
        J->State.ConsumedEventSequence=V->EventSequence;J->State.Pending=FALSE;
      }
    }
  } else {
    switch(V->Outcome) {
      case CrSmp2pMissing: if(J->State.Bound || J->SeenReady)Reasons|=CR_SMP2P_FAULT_MISSING;break;
      case CrSmp2pTransient:break;
      case CrSmp2pMalformed:Reasons|=CR_SMP2P_FAULT_MALFORMED;break;
      case CrSmp2pUnsupported:Reasons|=CR_SMP2P_FAULT_UNSUPPORTED;break;
      case CrSmp2pSourceLost:Reasons|=CR_SMP2P_FAULT_SOURCE_LOST;break;
      default:Reasons|=CR_SMP2P_FAULT_READ;break;
    }
    if(!J->State.Faulted)J->State.Error=V->Error;
  }
  if(Reasons)Fault(J,Reasons,V->Error==CR_SUCCESS?CR_DEVICE_ERROR:V->Error);
  Zero(&E,sizeof(E));E.Serial=++J->Sequence;E.Token=J->State.Token;
  E.Incarnation=J->State.Incarnation;E.ControllerSerial=J->State.ControllerSerial;
  E.StartActionSerial=J->State.StartActionSerial;E.ImageSerial=J->State.ImageSerial;
  E.BindingSerial=J->State.BindingSerial;E.BootSerial=J->State.BootSerial;
  E.EventSequence=V->EventSequence;E.CaptureSerial=V->CaptureSerial;E.Outcome=V->Outcome;
  E.FaultReasons=J->State.FaultReasons;E.PreviousValue=Old;E.Value=V->Value;
  if(V->Outcome==CrSmp2pValid){E.Rising=V->Value&~Old;E.Falling=Old&~V->Value;}
  for(Old=0;Old<5;++Old)E.Header[Old]=V->Header[Old];E.Error=J->State.Error;
  J->Events[(E.Serial-1)%CR_SMP2P_JOURNAL_ENTRIES]=E;return CR_SUCCESS;
}
CR_STATUS CrSmp2pBind(CR_SMP2P_JOURNAL *J,CONST VOID *Owner,CONST CR_SMP2P_LEASE *L,
                     UINT64 Controller,UINT64 Action,UINT64 Image,UINT64 Binding,UINT64 Boot) {
  if(!Owns(J,Owner,L) || Controller!=J->State.ControllerSerial ||
     Action!=J->State.StartActionSerial || !Image || !Binding || !Boot)return CR_INVALID_PARAMETER;
  if(J->State.Bound)return J->State.ImageSerial==Image && J->State.BindingSerial==Binding &&
      J->State.BootSerial==Boot?CR_SUCCESS:CR_INVALID_PARAMETER;
  if(Image<=J->LastImage || Binding<=J->LastBinding || Boot<=J->LastBoot)return CR_INVALID_PARAMETER;
  J->LastImage=Image;J->LastBinding=Binding;J->LastBoot=Boot;
  J->State.ImageSerial=Image;J->State.BindingSerial=Binding;J->State.BootSerial=Boot;
  J->State.Bound=TRUE;J->State.Pending=TRUE;J->State.Stable=FALSE;J->State.FreshAfterBind=FALSE;
  J->State.BindCaptureSerial=J->Source->CaptureSerial;return CR_SUCCESS;
}
CR_STATUS CrSmp2pObserve(CONST CR_SMP2P_JOURNAL *J,CONST VOID *Owner,
                        CONST CR_SMP2P_LEASE *L,CR_SMP2P_OBSERVATION *Out) {
  if(!Owns(J,Owner,L) || !JournalOutput(J,Out,sizeof(*Out)))return CR_INVALID_PARAMETER;
  *Out=J->State;return CR_SUCCESS;
}
CR_STATUS CrSmp2pReadEvent(CONST CR_SMP2P_JOURNAL *J,UINT64 Serial,CR_SMP2P_EVENT *Out) {
  CONST CR_SMP2P_EVENT *E;
  if(!JournalValid(J) || !Serial || !JournalOutput(J,Out,sizeof(*Out)))return CR_INVALID_PARAMETER;
  if(Serial>J->Sequence || J->Sequence-Serial>=CR_SMP2P_JOURNAL_ENTRIES)return CR_NOT_FOUND;
  E=&J->Events[(Serial-1)%CR_SMP2P_JOURNAL_ENTRIES];
  if(E->Serial!=Serial)return CR_NOT_FOUND;
  *Out=*E;return CR_SUCCESS;
}
CR_STATUS CrSmp2pClose(CR_SMP2P_JOURNAL *J,CONST VOID *Owner,CR_SMP2P_LEASE *L) {
  if(!Owns(J,Owner,L))return CR_INVALID_PARAMETER;
  J->Source->Journal=NULL;J->Source=NULL;J->ExactLease=NULL;J->Owner=NULL;
  J->State.Armed=FALSE;J->State.Pending=TRUE;J->State.FreshAfterBind=FALSE;
  Zero(L,sizeof(*L));return CR_SUCCESS;
}
