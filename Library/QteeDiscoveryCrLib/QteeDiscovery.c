/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Crane contributors.
 * Independent implementation of source-qualified Qualcomm QTEE discovery.
 * No SMC instruction or arbitrary secure-call interface is present here.
 */
#include <Library/QteeDiscoveryCrLib.h>
static void Zero(void *P,UINTN N){UINT8 *B=P;while(N--)*B++=0;}
static void Copy(void *D,const void *S,UINTN N){UINT8 *A=D;const UINT8 *B=S;while(N--)*A++=*B++;}
static int Overlap(const void *A,UINTN N,const void *B,UINTN M){
  UINTN X=(UINTN)A,Y=(UINTN)B;
  if(!A||!B||!N||!M)return 0;
  if(N-1>((UINTN)~(UINTN)0)-X||M-1>((UINTN)~(UINTN)0)-Y)return 1;
  return X<=Y ? Y-X<N : X-Y<M;
}
static UINT32 Read32(const UINT8 *P){return (UINT32)P[0]|(UINT32)P[1]<<8|(UINT32)P[2]<<16|(UINT32)P[3]<<24;}
static void Write32(UINT8 *P,UINT32 V){unsigned I;for(I=0;I<4;I++)P[I]=(UINT8)(V>>(8*I));}
static int Identity(CR_QTEE_DISCOVERY *D){return D&&D->Identity==D;}
static CR_QTEE_STATUS Closed(CR_QTEE_DISCOVERY *D){
  D->Closed=1;D->Io.Quarantine(D->Io.Context,&D->Lease);return CrQteeRetained;
}
static int LeaseEqual(const CR_QTEE_LEASE *A,const CR_QTEE_LEASE *B){
  return A->Provider==B->Provider&&A->Generation==B->Generation&&A->Buffer==B->Buffer&&
    A->Physical==B->Physical&&A->Bytes==B->Bytes&&A->Profile==B->Profile;
}
static CR_QTEE_STATUS Check(CR_QTEE_DISCOVERY *D){
  if(!LeaseEqual(&D->Lease,&D->OriginalLease)||
     D->Io.Check(D->Io.Context,&D->Lease)!=CrQteeOk)return Closed(D);
  return CrQteeOk;
}
static void Call(CR_QTEE_DISCOVERY *D,CR_QTEE_OPERATION Op){
  Zero(&D->LastCall,sizeof(D->LastCall));D->LastCall.Operation=Op;
  D->LastCall.Owner=2;D->LastCall.Service=12;
  if(Op==CrQteeBridgeEnable)D->LastCall.Command=28;
  else if(Op==CrQteeBridgeCreate){
    D->LastCall.Command=30;D->LastCall.ArgInfo=4;
    D->LastCall.Args[0]=D->Lease.Physical|6;D->LastCall.Args[1]=D->Lease.Physical|6;
    D->LastCall.Args[2]=CR_QTEE_BYTES|((UINT64)1)<<9;D->LastCall.Args[3]=3;
  }else if(Op==CrQteeBridgeDelete){
    D->LastCall.Command=29;D->LastCall.ArgInfo=1;D->LastCall.Args[0]=D->BridgeHandle;
  }else{
    D->LastCall.Owner=50;D->LastCall.Service=6;D->LastCall.Command=2;D->LastCall.ArgInfo=0x224;
    D->LastCall.Args[0]=D->Lease.Physical;D->LastCall.Args[1]=CR_QTEE_PAGE;
    D->LastCall.Args[2]=D->Lease.Physical+CR_QTEE_PAGE;D->LastCall.Args[3]=CR_QTEE_PAGE;
  }
}
static CR_QTEE_STATUS Execute(CR_QTEE_DISCOVERY *D){
  if(Check(D)!=CrQteeOk)return CrQteeRetained;
  Zero(&D->LastReply,sizeof(D->LastReply));
  CR_QTEE_STATUS S=D->Io.Execute(D->Io.Context,&D->Lease,&D->LastCall,&D->LastReply);
  if(S==CrQteeNotIssued){
    if(D->LastCall.Operation!=CrQteeBridgeEnable||D->Created||D->ObjectCount||
       !LeaseEqual(&D->Lease,&D->OriginalLease))return Closed(D);
    return CrQteeNotIssued;
  }
  if(S!=CrQteeOk)return Closed(D);
  /* x0 is SCM transport, not QTEE response type. In particular raw WAITQ
     2/3 must never retire pages or be mistaken for x1 object callback 3. */
  if(D->LastReply.X[0]!=0)return Closed(D);
  if(!LeaseEqual(&D->Lease,&D->OriginalLease))return Closed(D);
  if(D->Io.Cancelled(D->Io.Context))D->Cancelled=1;
  return CrQteeOk;
}
static int NewObject(CR_QTEE_DISCOVERY *D,UINT32 H){
  if(D->ObjectCount==CR_QTEE_OBJECTS)return -1;
  UINT32 I=D->ObjectCount++;
  D->Objects[I].Handle=H;D->Objects[I].Live=1;return (int)I;
}
/* Build only the four fixed argument shapes. Original offsets/header/payload
   remain outside firmware-writable pages. OO is always argument 1 at byte20. */
static void Message(CR_QTEE_DISCOVERY *D,UINT32 Slot,UINT32 Op,const char *Name){
  UINT8 *P=D->Expected;UINT32 I,N=0;
  Zero(P,CR_QTEE_PAGE);Write32(P,D->Objects[Slot].Handle);Write32(P+4,Op);
  D->OutputOffset=0;
  if(Op==5){Write32(P+8,0x1100);D->OutputOffset=20;}
  else if(Op==0||Op==2){
    Write32(P+8,0x1001);Write32(P+12,32);D->OutputOffset=20;
    if(Op==0){N=4;Write32(P+32,122);}
    else{while(Name[N])N++;for(I=0;I<N;I++)P[32+I]=(UINT8)Name[I];}
    Write32(P+16,N);
  }
  Copy(D->Lease.Buffer,P,CR_QTEE_PAGE);Zero(D->Lease.Buffer+CR_QTEE_PAGE,CR_QTEE_PAGE);
}
static CR_QTEE_STATUS Invoke(CR_QTEE_DISCOVERY *D,UINT32 Slot,UINT32 Op,const char *Name,int *NewSlot){
  UINT32 H=0,I;int Added=-1;INT32 Result;
  if(Check(D)!=CrQteeOk)return CrQteeRetained;
  Message(D,Slot,Op,Name);Call(D,CrQteeInvoke);
  if(D->Io.Sync(D->Io.Context,&D->Lease,1)!=CrQteeOk)return Closed(D);
  if(Execute(D)!=CrQteeOk)return CrQteeRetained;
  if(D->Io.Sync(D->Io.Context,&D->Lease,0)!=CrQteeOk)return Closed(D);
  /* Save every returned reference in its preallocated slot before validation,
     even an unexpected local or malformed partial result. Never deduplicate. */
  if(D->OutputOffset){
    H=Read32(D->Lease.Buffer+D->OutputOffset);
    if(H)Added=NewObject(D,H);
  }
  if(D->LastReply.X[1]!=0)return Closed(D); /* callback/listener unimplemented */
  /* No local objects are granted by this restricted service. An empty
     piggyback has no interpreted version/op; any entries require retention. */
  if(Read32(D->Lease.Buffer+CR_QTEE_PAGE+8)!=0)return Closed(D);
  for(I=0;I<CR_QTEE_PAGE;I++){
    if(D->OutputOffset&&I>=D->OutputOffset&&I<D->OutputOffset+4)continue;
    if(D->Lease.Buffer[I]!=D->Expected[I])return Closed(D);
  }
  Result=(INT32)(UINT32)D->LastReply.X[2];D->Result.ObjectResult=Result;
  if(H && (Added<0||(H&0x80000000u)))return Closed(D);
  if(Result){
    if(H)return Closed(D);
    return CrQteeObjectError;
  }
  if(D->OutputOffset&&!H)return Closed(D);
  if(NewSlot)*NewSlot=Added;
  return CrQteeOk;
}
static CR_QTEE_STATUS ReleaseObject(CR_QTEE_DISCOVERY *D,UINT32 I){
  CR_QTEE_STATUS S;
  if(!D->Objects[I].Live)return CrQteeOk;
  /* smcinvoke_release_filp: root handle1 is indestructible. Releasing its
     kernel wrapper retires only the local reference, never a secure method. */
  if(D->Objects[I].Handle==1){D->Objects[I].Live=0;return CrQteeOk;}
  S=Invoke(D,I,0xffff,0,0);
  if(S==CrQteeOk)D->Objects[I].Live=0;
  else return Closed(D); /* An error is not evidence that release had no effects. */
  return S;
}
static CR_QTEE_STATUS Retire(CR_QTEE_DISCOVERY *D){
  CR_QTEE_STATUS S;UINT32 I;
  if(D->Closed)return CrQteeRetained;
  if(!D->Active)return CrQteeOk;
  if(!LeaseEqual(&D->Lease,&D->OriginalLease))return Closed(D);
  for(I=D->ObjectCount;I>0;I--){S=ReleaseObject(D,I-1);if(S!=CrQteeOk)return CrQteeRetained;}
  if(D->Created){
    Call(D,CrQteeBridgeDelete);S=Execute(D);if(S!=CrQteeOk)return S;
    /* Android delete returns SCM transport only; x1 is NOT another status. */
    D->Created=0;D->BridgeHandle=0;
  }
  S=D->Io.Release(D->Io.Context,&D->Lease);
  if(S!=CrQteeOk){
    if(!LeaseEqual(&D->Lease,&D->OriginalLease))return Closed(D);
    return CrQteeRetained;
  }
  Zero(&D->Lease,sizeof(D->Lease));Zero(&D->OriginalLease,sizeof(D->OriginalLease));
  D->Active=0;return CrQteeOk;
}
CR_QTEE_STATUS CrQteeInitialize(CR_QTEE_DISCOVERY *D,const CR_QTEE_IO *Io){
  if(!D||!Io||Overlap(D,sizeof(*D),Io,sizeof(*Io))||D->Identity||!Io->Context||
    !Io->Acquire||!Io->Check||!Io->Sync||!Io->Execute||!Io->Release||!Io->Cancelled||!Io->Quarantine)
    return CrQteeInvalid;
  Zero(D,sizeof(*D));D->Identity=D;D->Io=*Io;return CrQteeOk;
}
CR_QTEE_STATUS CrQteeRetire(CR_QTEE_DISCOVERY *D){
  CR_QTEE_STATUS S;if(!Identity(D))return CrQteeInvalid;if(D->Busy)return CrQteeBusy;
  D->Busy=1;S=Retire(D);D->Busy=0;return S;
}
CR_QTEE_STATUS CrQteeDiscover(CR_QTEE_DISCOVERY *D,CR_QTEE_DISCOVERY_RESULT *Out){
  CR_QTEE_STATUS S,T;int Env=-1,Loader=-1,Ta=-1;UINT32 I;INT32 ObjectResult=0;
  static const char *const Names[]={"hdcp2p2","hdcpsrm"};
  if(!Identity(D)||!Out||Overlap(D,sizeof(*D),Out,sizeof(*Out))||
     (D->Active&&Overlap(D->Lease.Buffer,D->Lease.Bytes,Out,sizeof(*Out))))return CrQteeInvalid;
  if(D->Busy)return CrQteeBusy;
  Zero(Out,sizeof(*Out));if(D->Active||D->Closed)return CrQteeRetained;
  D->Busy=1;D->Cancelled=0;D->ObjectCount=0;Zero(D->Objects,sizeof(D->Objects));
  Zero(&D->Result,sizeof(D->Result));D->Result.Revision=1;
  if(D->Io.Cancelled(D->Io.Context)){S=CrQteeCancelled;goto Done;}
  S=D->Io.Acquire(D->Io.Context,&D->Lease);
  D->Active=D->Lease.Provider!=0;D->OriginalLease=D->Lease;
  if(S!=CrQteeOk){if(D->Active)S=CrQteeRetained;goto Done;}
  if(!D->Active){S=CrQteeUnavailable;goto Done;}
  if(!D->Lease.Generation||!D->Lease.Buffer||D->Lease.Bytes!=CR_QTEE_BYTES||D->Lease.Profile!=1||
    ((UINTN)D->Lease.Buffer&(CR_QTEE_PAGE-1))||!D->Lease.Physical||
    (D->Lease.Physical&(CR_QTEE_PAGE-1))||D->Lease.Physical>CR_MAX_UINT64-CR_QTEE_BYTES||
    Overlap(D,sizeof(*D),D->Lease.Buffer,CR_QTEE_BYTES)||
    Overlap(Out,sizeof(*Out),D->Lease.Buffer,CR_QTEE_BYTES)){
    S=CrQteeUnavailable;goto Cleanup;
  }
  if(Check(D)!=CrQteeOk){S=CrQteeRetained;goto Done;}
  if(D->Io.Cancelled(D->Io.Context)){S=CrQteeCancelled;goto Cleanup;}
  Call(D,CrQteeBridgeEnable);S=Execute(D);if(S==CrQteeNotIssued){S=CrQteeUnavailable;goto Cleanup;}if(S!=CrQteeOk)goto Done;
  if(D->LastReply.X[1]){S=CrQteeUnavailable;goto Cleanup;}
  if(D->Cancelled){S=CrQteeCancelled;goto Cleanup;}
  Call(D,CrQteeBridgeCreate);S=Execute(D);if(S!=CrQteeOk)goto Done;
  D->BridgeHandle=D->LastReply.X[2];
  if(D->LastReply.X[1]){
    if(D->BridgeHandle){S=Closed(D);goto Done;}
    S=CrQteeUnavailable;goto Cleanup;
  }
  D->Created=1;
  if(D->Cancelled){S=CrQteeCancelled;goto Cleanup;}
  (void)NewObject(D,1); /* Each kernel client starts with its own root ref. */
  S=Invoke(D,0,5,0,&Env);if(S!=CrQteeOk)goto Cleanup;
  S=ReleaseObject(D,0);if(S!=CrQteeOk)goto Cleanup;
  if(D->Cancelled){S=CrQteeCancelled;goto Cleanup;}
  S=Invoke(D,(UINT32)Env,0,0,&Loader);if(S!=CrQteeOk)goto Cleanup;
  for(I=0;I<2;I++){
    if(D->Cancelled){S=CrQteeCancelled;goto Cleanup;}
    S=Invoke(D,(UINT32)Loader,2,Names[I],&Ta);
    if(S==CrQteeObjectError&&D->Result.ObjectResult==22){S=CrQteeOk;continue;}
    if(S!=CrQteeOk)goto Cleanup;
    if(I==0)D->Result.Hdcp2p2Present=1;else D->Result.HdcpSrmPresent=1;
    S=ReleaseObject(D,(UINT32)Ta);if(S!=CrQteeOk)goto Cleanup;
  }
Cleanup:
  ObjectResult=D->Result.ObjectResult;T=Retire(D);
  if(T!=CrQteeOk)S=CrQteeRetained;
  else if(D->Cancelled)S=CrQteeCancelled;
  D->Result.ObjectResult=ObjectResult;
  if(S==CrQteeOk){D->Result.ObjectResult=0;*Out=D->Result;}
  else if(S==CrQteeObjectError)Out->ObjectResult=ObjectResult;
Done:
  D->Busy=0;return S;
}
