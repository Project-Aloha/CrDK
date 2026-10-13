/* Linux cmdDB ARC tables and absolute max-owner active votes. MIT */
#include <Library/RpmhArcVoteLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>
STATIC CR_STATUS Check(CR_RPMH_ARC_VOTER *V) {
  return !CR_ERROR(CrMutexCheckLevel()) && V!=NULL && V->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC BOOLEAN NameValid(CONST CHAR8 *Name) {
  if(Name==NULL) return FALSE;
  for(UINT32 I=0;I<9;++I) {if(Name[I]==0) return I!=0;if((UINT8)Name[I]<33 || (UINT8)Name[I]>126) return FALSE;}
  return FALSE;
}
STATIC BOOLEAN ValidCorner(CONST CR_RPMH_ARC_RESOURCE *R,UINT32 Corner) {
  return R->Kind==CrRpmhArcXo ? Corner==0 || Corner==3 : Corner<R->LevelCount;
}
CR_STATUS CrRpmhArcQualify(CR_RPMH_ARC_RESOURCE *R,CONST CHAR8 *Name,UINT32 Address,
    CR_RPMH_ARC_KIND Kind,CONST UINT8 *Aux,UINT32 Length) {
  if(CR_ERROR(CrMutexCheckLevel()) || R==NULL || !NameValid(Name) || (Address&0xffff0003U)!=0x00030000U ||
     (Kind!=CrRpmhArcPower && Kind!=CrRpmhArcXo) || Length>64 || (Length&1)!=0 ||
     (Length!=0 && Aux==NULL) || (Kind==CrRpmhArcPower && Length<4)) return CR_INVALID_PARAMETER;
  CR_RPMH_ARC_RESOURCE Copy={0};
  for(UINT32 I=0;Name[I]!=0;++I) Copy.Name[I]=Name[I];
  Copy.Address=Address;Copy.Kind=Kind;
  BOOLEAN Padding=FALSE;UINT32 Previous=0;
  for(UINT32 I=0;I<Length/2;++I) {
    UINT32 Level=Aux[I*2]|((UINT32)Aux[I*2+1]<<8);
    if(I==0 && Level!=0) return CR_COMPROMISED_DATA;
    if(I!=0 && Level==0) Padding=TRUE;
    if(Padding) {if(Level!=0) return CR_COMPROMISED_DATA;continue;}
    if(Level<Previous) return CR_COMPROMISED_DATA;
    Copy.Levels[I]=(UINT16)Level;Copy.LevelCount=I+1;Previous=Level;
  }
  if(Kind==CrRpmhArcPower && Copy.LevelCount<2) return CR_COMPROMISED_DATA;
  *R=Copy;return CR_SUCCESS;
}
STATIC BOOLEAN Overlap(UINTN A,UINTN As,UINTN B,UINTN Bs) {return A<B+Bs && B<A+As;}
CR_STATUS CrRpmhArcVotesInit(CR_RPMH_ARC_VOTER *V,CONST CR_RPMH_ARC_IO *Io,
    CONST CR_RPMH_ARC_RESOURCE *Resources,UINT32 Count,CR_RPMH_ARC_ENTRY *Entries,UINT32 Capacity) {
  if(CR_ERROR(CrMutexCheckLevel()) || V==NULL || Io==NULL || Io->Write==NULL || Resources==NULL || Entries==NULL ||
     Count==0 || Count>CR_RPMH_ARC_MAX_RESOURCES || Capacity==0 || Capacity>65536 ||
     (UINTN)Resources%sizeof(UINT32)!=0 || (UINTN)Entries%sizeof(VOID *)!=0) return CR_INVALID_PARAMETER;
  if(V->Initialized) return CR_ALREADY_STARTED;
  UINTN Vs=sizeof(*V),Rs=Count*sizeof(*Resources),Es=Capacity*sizeof(*Entries);
  UINTN A=(UINTN)V,B=(UINTN)Resources,C=(UINTN)Entries;
  if(A>~(UINTN)0-Vs || B>~(UINTN)0-Rs || C>~(UINTN)0-Es ||
     Overlap(A,Vs,B,Rs) || Overlap(A,Vs,C,Es) || Overlap(B,Rs,C,Es)) return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<Count;++I) {
    CONST CR_RPMH_ARC_RESOURCE *R=&Resources[I];
    if(!NameValid(R->Name) || (R->Address&0xffff0003U)!=0x00030000U ||
       (R->Kind!=CrRpmhArcPower && R->Kind!=CrRpmhArcXo) || R->LevelCount>32 ||
       (R->Kind==CrRpmhArcPower && R->LevelCount<2)) return CR_INVALID_PARAMETER;
    for(UINT32 L=0;L<R->LevelCount;++L)
      if((L==0 && R->Levels[L]!=0) || (L!=0 && (R->Levels[L]==0 || R->Levels[L]<R->Levels[L-1]))) return CR_INVALID_PARAMETER;
    for(UINT32 P=0;P<I;++P)
      if(R->Address==Resources[P].Address || cr_strcmp(R->Name,Resources[P].Name)==0) return CR_INVALID_PARAMETER;
  }
  CR_RPMH_ARC_IO Copy=*Io;cr_memset(V,0,Vs);cr_memset(Entries,0,Es);
  V->Io=Copy;V->Resources=Resources;V->Count=Count;V->Entries=Entries;V->Capacity=Capacity;V->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrRpmhArcImportBaseline(CR_RPMH_ARC_VOTER *V,VOID *Owner,CONST UINT32 *Corners,UINT32 Count) {
  CR_STATUS Status=Check(V);
  if(CR_ERROR(Status) || Owner==NULL || Corners==NULL || Count!=V->Count) return CR_INVALID_PARAMETER;
  if(Count>V->Capacity) return CR_OUT_OF_RESOURCES;
  for(UINT32 I=0;I<V->Capacity;++I) if(V->Entries[I].Owner!=NULL) return CR_BUSY;
  for(UINT32 I=0;I<Count;++I) {
    if(!ValidCorner(&V->Resources[I],Corners[I])) return CR_INVALID_PARAMETER;
    if(V->States[I].Known || V->States[I].Pending || V->States[I].Touched) return CR_BUSY;
  }
  for(UINT32 I=0;I<Count;++I) {
    V->Entries[I]=(CR_RPMH_ARC_ENTRY){Owner,I,{Corners[I],TRUE},FALSE,TRUE};
    V->States[I]=(CR_RPMH_ARC_STATE){Corners[I],TRUE,FALSE,FALSE};
  }
  return CR_SUCCESS;
}
STATIC CR_STATUS Index(CR_RPMH_ARC_VOTER *V,CONST CHAR8 *Name,UINT32 *Out) {
  CR_STATUS Status=Check(V);if(CR_ERROR(Status) || !NameValid(Name)) return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<V->Count;++I) if(cr_strcmp(V->Resources[I].Name,Name)==0) {*Out=I;return CR_SUCCESS;}
  return CR_NOT_FOUND;
}
CR_STATUS CrRpmhArcDescribe(CR_RPMH_ARC_VOTER *V,CONST CHAR8 *Name,CR_RPMH_ARC_RESOURCE *Out) {
  if(Out==NULL) return CR_INVALID_PARAMETER;
  UINT32 I;CR_STATUS Status=Index(V,Name,&I);if(!CR_ERROR(Status)) *Out=V->Resources[I];return Status;
}
CR_STATUS CrRpmhArcResolveLevel(CR_RPMH_ARC_VOTER *V,CONST CHAR8 *Name,UINT32 Level,UINT32 *Out) {
  if(Out==NULL) return CR_INVALID_PARAMETER;
  UINT32 I;CR_STATUS Status=Index(V,Name,&I);if(CR_ERROR(Status)) return Status;
  CONST CR_RPMH_ARC_RESOURCE *R=&V->Resources[I];
  if(Level==0) {*Out=0;return CR_SUCCESS;}
  for(UINT32 C=1;C<R->LevelCount;++C) if(R->Levels[C]>=Level) {
    if(!ValidCorner(R,C)) return CR_UNSUPPORTED;*Out=C;return CR_SUCCESS;
  }
  return CR_UNSUPPORTED;
}
STATIC CR_RPMH_ARC_ENTRY *Find(CR_RPMH_ARC_VOTER *V,VOID *Owner,UINT32 Resource) {
  for(UINT32 I=0;I<V->Capacity;++I) if(V->Entries[I].Owner==Owner && V->Entries[I].Resource==Resource) return &V->Entries[I];
  return NULL;
}
CR_STATUS CrRpmhArcReserve(CR_RPMH_ARC_VOTER *V,VOID *Owner,CONST CHAR8 *Name) {
  if(Owner==NULL) return CR_INVALID_PARAMETER;
  UINT32 R;CR_STATUS Status=Index(V,Name,&R);if(CR_ERROR(Status)) return Status;
  if(Find(V,Owner,R)!=NULL) return CR_SUCCESS;
  for(UINT32 I=0;I<V->Capacity;++I) if(V->Entries[I].Owner==NULL) {
    V->Entries[I].Owner=Owner;V->Entries[I].Resource=R;return CR_SUCCESS;
  }
  return CR_OUT_OF_RESOURCES;
}
CR_STATUS CrRpmhArcGet(CR_RPMH_ARC_VOTER *V,VOID *Owner,CONST CHAR8 *Name,CR_RPMH_ARC_VALUE *Value,BOOLEAN *Pending) {
  if(Owner==NULL || Value==NULL || Pending==NULL) return CR_INVALID_PARAMETER;
  UINT32 R;CR_STATUS Status=Index(V,Name,&R);if(CR_ERROR(Status)) return Status;
  CR_RPMH_ARC_ENTRY *E=Find(V,Owner,R);if(E==NULL) return CR_NOT_FOUND;
  *Value=E->Value;*Pending=!V->States[R].Known || V->States[R].Pending;return CR_SUCCESS;
}
STATIC UINT32 Aggregate(CR_RPMH_ARC_VOTER *V,UINT32 R,CONST CR_RPMH_ARC_ENTRY *Replace,CONST CR_RPMH_ARC_VALUE *Value) {
  UINT32 Result=0;
  for(UINT32 I=0;I<V->Capacity;++I) {
    CONST CR_RPMH_ARC_ENTRY *E=&V->Entries[I];if(E->Owner==NULL || E->Resource!=R) continue;
    CONST CR_RPMH_ARC_VALUE *Vote=E==Replace ? Value : &E->Value;
    if(Vote->Present && Vote->Corner>Result) Result=Vote->Corner;
  }
  return Result;
}
STATIC CR_STATUS Write(CR_RPMH_ARC_VOTER *V,UINT32 R,UINT32 Corner) {
  CR_RPMH_ARC_STATE *S=&V->States[R];S->Pending=S->Touched=TRUE;S->Known=FALSE;
  CR_STATUS Status=V->Io.Write(V->Io.Context,V->Resources[R].Address,Corner);
  if(!CR_ERROR(Status)) {S->Applied=Corner;S->Known=TRUE;S->Pending=FALSE;}
  return Status;
}
CR_STATUS CrRpmhArcSet(CR_RPMH_ARC_VOTER *V,VOID *Owner,CONST CHAR8 *Name,CONST CR_RPMH_ARC_VALUE *Input) {
  if(Owner==NULL || Input==NULL || Input->Present>1 || (!Input->Present && Input->Corner!=0)) return CR_INVALID_PARAMETER;
  CR_RPMH_ARC_VALUE Value=*Input;UINT32 R;
  CR_STATUS Status=Index(V,Name,&R);if(CR_ERROR(Status)) return Status;
  if(!ValidCorner(&V->Resources[R],Value.Corner)) return CR_INVALID_PARAMETER;
  CR_RPMH_ARC_ENTRY *E=Find(V,Owner,R);if(E==NULL) return CR_NOT_FOUND;
  if(E->Inherited) return CR_BUSY;
  if(V->States[R].Pending) {
    Status=Write(V,R,Aggregate(V,R,NULL,NULL));
    if(CR_ERROR(Status)) {E->RecoveryPending=TRUE;return Status;}
    for(UINT32 I=0;I<V->Capacity;++I)
      if(V->Entries[I].Owner!=NULL && V->Entries[I].Resource==R) V->Entries[I].RecoveryPending=FALSE;
  }
  UINT32 Desired=Aggregate(V,R,E,&Value);
  if(!V->States[R].Known || Desired!=V->States[R].Applied) {
    Status=Write(V,R,Desired);
    if(CR_ERROR(Status)) {E->RecoveryPending=TRUE;return Status;}
  }
  E->Value=Value;return CR_SUCCESS;
}
CR_STATUS CrRpmhArcReleaseOwner(CR_RPMH_ARC_VOTER *V,VOID *Owner) {
  CR_STATUS Status=Check(V);if(CR_ERROR(Status) || Owner==NULL) return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<V->Capacity;++I) if(V->Entries[I].Owner==Owner && V->Entries[I].Inherited) return CR_BUSY;
  CR_STATUS Result=CR_SUCCESS;CR_RPMH_ARC_VALUE Absent={0};
  for(UINT32 I=V->Capacity;I!=0;--I) {
    CR_RPMH_ARC_ENTRY *E=&V->Entries[I-1];if(E->Owner!=Owner) continue;
    /* Metadata-only reservations did not acquire hardware ownership. Do not
       withdraw unknown boot votes or repair another owner's failed operation
       while releasing such an empty slot. The failed writer retains its slot. */
    Status=E->Value.Present || E->RecoveryPending ?
      CrRpmhArcSet(V,Owner,V->Resources[E->Resource].Name,&Absent) : CR_SUCCESS;
    if(CR_ERROR(Status)) {if(!CR_ERROR(Result)) Result=Status;continue;}
    cr_memset(E,0,sizeof(*E));
  }
  return Result;
}
CR_STATUS CrRpmhArcDrain(CR_RPMH_ARC_VOTER *V) {
  CR_STATUS Status=Check(V);if(CR_ERROR(Status)) return Status;
  for(UINT32 I=0;I<V->Capacity;++I) if(V->Entries[I].Owner!=NULL) {
    Status=CrRpmhArcReleaseOwner(V,V->Entries[I].Owner);if(CR_ERROR(Status)) return Status;
  }
  for(UINT32 I=0;I<V->Count;++I) if(V->States[I].Touched && (!V->States[I].Known || V->States[I].Pending || V->States[I].Applied!=0)) {
    Status=Write(V,I,0);if(CR_ERROR(Status)) return Status;
  }
  return CR_SUCCESS;
}
CR_STATUS CrRpmhArcUninit(CR_RPMH_ARC_VOTER *V) {
  CR_STATUS Status=Check(V);if(CR_ERROR(Status)) return Status;
  for(UINT32 I=0;I<V->Capacity;++I) if(V->Entries[I].Owner!=NULL) return CR_BUSY;
  for(UINT32 I=0;I<V->Count;++I) if(V->States[I].Touched && (!V->States[I].Known || V->States[I].Pending || V->States[I].Applied!=0)) return CR_NOT_READY;
  cr_memset(V,0,sizeof(*V));return CR_SUCCESS;
}
