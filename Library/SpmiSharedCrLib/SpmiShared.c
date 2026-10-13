/** @file
  Client lifetime and per-bit IRQ restoration for a shared Qualcomm arbiter.
  SPDX-License-Identifier: MIT
**/
#include <Library/SpmiSharedCrLib.h>
#include <oskal/cr_string.h>
#ifdef _KERNEL_MODE
#if !defined(CR_OSK_LOG_WITHOUT_WPP)
#include "SpmiShared.tmh"
#endif
#endif
STATIC BOOLEAN Overlap(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn) {
  UINTN a=(UINTN)A,b=(UINTN)B;
  return An!=0 && Bn!=0 && (a>~(UINTN)0-(An-1) || b>~(UINTN)0-(Bn-1) ||
                          (a<=b+Bn-1 && b<=a+An-1));
}
STATIC BOOLEAN Safe(CR_SPMI_SHARED *P,CONST VOID *B,UINTN N) {
  if(!B || !N || Overlap(P,sizeof(*P),B,N) || Overlap(P->Spmi,sizeof(*P->Spmi),B,N)) return FALSE;
  for(UINTN I=0;I<CR_SPMI_MAX_CLIENTS;I++)
    if(P->Owners[I].Identity && (Overlap(B,N,P->Owners[I].Identity,sizeof(CR_SPMI_CLIENT)) ||
      Overlap(B,N,P->Owners[I].BusIdentity,sizeof(CR_SPMI_BUS))))return FALSE;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++)
    if(P->Irqs[I].Identity && Overlap(B,N,P->Irqs[I].Identity,sizeof(CR_SPMI_IRQ)))return FALSE;
  return TRUE;
}
STATIC CR_SPMI_OWNER *Owner(VOID *Context) {
  CR_SPMI_BUS *B=Context;
  CR_SPMI_OWNER *O=B?B->Context:NULL;
  if(!O || !O->Provider || !O->Provider->Initialized || !O->Identity || O->BusIdentity!=B || B->Generation!=O->Generation ||
     O->Identity->Provider!=O->Provider || O->Identity->Owner!=O->Owner ||
     O->Identity->Generation!=O->Generation) return NULL;
  return O;
}
STATIC CR_SPMI_SUBSCRIPTION *Irq(CR_SPMI_OWNER *O,CR_SPMI_IRQ *T) {
  if(!T || T->Provider!=O->Provider || !T->Generation)return NULL;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++) {
    CR_SPMI_SUBSCRIPTION *S=&O->Provider->Irqs[I];
    if(S->Identity==T && S->Owner==O && S->Generation==T->Generation)return S;
  }
  return NULL;
}
STATIC BOOLEAN Allowed(CR_SPMI_OWNER *O,UINT8 Sid,UINT8 Periph,UINT8 Access,UINT8 Bit) {
  for(UINTN I=0;I<O->RegionCount;I++) {
    CR_SPMI_REGION *R=&O->Regions[I];
    if(R->Sid==Sid && R->Peripheral==Periph && (R->Access&Access)==Access &&
       (Bit==0 || (R->Irqs&Bit)!=0))return TRUE;
  }
  return FALSE;
}
STATIC CR_STATUS Transfer(CR_SPMI_OWNER *O,UINT8 Sid,UINT16 A,UINT8 *B,
                           CONST UINT8 *M,UINTN N,BOOLEAN Write) {
  if(!O || !O->Provider || (O->Provider->Closed && !O->Retiring) || O->Provider->Dispatching)return CR_NOT_READY;
  if(N==0 || N>8 || (A&255)+N>256 || !Allowed(O,Sid,(UINT8)(A>>8),Write?CR_SPMI_WRITE:CR_SPMI_READ,0) ||
     !Safe(O->Provider,B,N) || (M && !Safe(O->Provider,M,N)))return CR_INVALID_PARAMETER;
  if(Write && (A&255)<=0x18 && (A&255)+N>0x10)return CR_BUSY;
  if(!Write)return SpmiRead(O->Provider->Spmi,Sid,A,B,N);
  if(M)return SpmiMaskedWrite(O->Provider->Spmi,Sid,A,B,M,N);
  return SpmiWrite(O->Provider->Spmi,Sid,A,B,N);
}
STATIC CR_STATUS Read(VOID *C,UINT8 S,UINT16 A,UINT8 *B,UINTN N){return Transfer(Owner(C),S,A,B,NULL,N,FALSE);}
STATIC CR_STATUS Write(VOID *C,UINT8 S,UINT16 A,CONST UINT8 *B,UINTN N){return Transfer(Owner(C),S,A,(UINT8 *)B,NULL,N,TRUE);}
STATIC CR_STATUS Masked(VOID *C,UINT8 S,UINT16 A,CONST UINT8 *B,CONST UINT8 *M,UINTN N){
  if(!M)return CR_INVALID_PARAMETER;return Transfer(Owner(C),S,A,(UINT8 *)B,M,N,TRUE);
}
STATIC UINT32 ReadPic(CR_SPMI_SHARED *P,UINT16 A) {
  UINTN Address=P->Spmi->PmicArb.Ops->GetAccEnable(P->Spmi,A);
  return P->Spmi->Io.Read32?P->Spmi->Io.Read32(P->Spmi->Io.Context,Address):CrMmioRead32(Address);
}
STATIC VOID WritePic(CR_SPMI_SHARED *P,UINT16 A,UINT32 V) {
  UINTN Address=P->Spmi->PmicArb.Ops->GetAccEnable(P->Spmi,A);
  if(P->Spmi->Io.Write32)P->Spmi->Io.Write32(P->Spmi->Io.Context,Address,V);else CrMmioWrite32(Address,V);
}
STATIC BOOLEAN PicFits(CR_SPMI_SHARED *P,UINT16 Apid) {
  UINTN A=P->Spmi->PmicArb.Ops->GetAccEnable(P->Spmi,Apid);
  if(!A || (A&3) || A>~(UINTN)0-3)return FALSE;
  for(UINTN I=0;I<SPMI_MEMORY_REGION_TYPE_MAX;I++) {
    SpmiMemoryRegion *R=&P->Spmi->Regions[I];
    if(R->Size>=4 && A>=R->BaseAddress && A-R->BaseAddress<=R->Size-4)return TRUE;
  }
  return FALSE;
}
STATIC CR_STATUS ReconcilePic(CR_SPMI_SHARED *P,UINT16 Apid,UINT32 Initial) {
  UINT32 Want=Initial&1;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++) {
    SpmiIrqEntry *E=&P->Spmi->IrqEntries[I];
    if(E->Handler && E->Apid==Apid && (E->Enabled || E->EnableUncertain))Want=1;
  }
  WritePic(P,Apid,(ReadPic(P,Apid)&~1U)|Want);
  return (ReadPic(P,Apid)&1)==Want?CR_SUCCESS:CR_DEVICE_ERROR;
}
STATIC VOID Notify(VOID *Context) {
  CR_SPMI_SUBSCRIPTION *S=Context;
  if(!S->Identity || !S->Registered || S->Provider->Closed || S->Invoking)return;
  S->Invoking=TRUE;S->Handler(S->Context);S->Invoking=FALSE;
}
STATIC CR_STATUS Subscribe(VOID *C,UINT8 Sid,UINT8 Periph,UINT8 Bit,
                           CR_INTERRUPT_TRIGGER_TYPE Trigger,CR_INTERRUPT_HANDLER H,
                           VOID *Context,CR_SPMI_IRQ *Token) {
  CR_SPMI_OWNER *O=Owner(C); CR_SPMI_SUBSCRIPTION *Slot=NULL; UINT16 Apid; UINTN Pic=0;
  if(!O || O->Retiring || O->Provider->Closed || O->Provider->Dispatching)return CR_NOT_READY;
  CR_SPMI_SHARED *P=O->Provider;
  if(!H || !Token || Bit>7 || (UINT32)Trigger>CR_INTERRUPT_TRIGGER_EDGE_BOTH ||
     !Allowed(O,Sid,Periph,CR_SPMI_READ,(UINT8)BIT(Bit)) ||
     !Safe(P,Token,sizeof(*Token)) || Token->Provider || Token->Generation)return CR_INVALID_PARAMETER;
  if(P->NextGeneration==~(UINT64)0)return CR_OUT_OF_RESOURCES;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++) {
    CR_SPMI_SUBSCRIPTION *S=&P->Irqs[I];
    if(S->Identity && S->Sid==Sid && S->Peripheral==Periph && S->Bit==Bit)return CR_BUSY;
    if(!S->Identity && !Slot)Slot=S;
  }
  if(!Slot)return CR_OUT_OF_RESOURCES;
  CR_STATUS Status=SpmiGetApid(P->Spmi,Sid,Periph,&Apid);
  if(Status!=CR_SUCCESS)return Status;
  /* Validate ownership before reading the PIC or changing the PMIC. */
  if(!PicFits(P,Apid))return CR_INVALID_PARAMETER;
  if(P->Spmi->Bus.ApidData[Apid].IrqEe!=P->Spmi->ActiveEE)return CR_BUSY;
  UINT8 Snapshot[4];
  Status=SpmiRead(P->Spmi,Sid,(UINT16)((Periph<<8)+0x11),Snapshot,3);
  if(Status==CR_SUCCESS)Status=SpmiRead(P->Spmi,Sid,(UINT16)((Periph<<8)+0x15),Snapshot+3,1);
  if(Status!=CR_SUCCESS)return Status;
  for(Pic=0;Pic<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;Pic++)
    if(P->Pic[Pic].References && P->Pic[Pic].Apid==Apid)break;
  if(Pic==SPMI_PMIC_ARB_MAX_IRQ_ENTRIES) {
    for(Pic=0;Pic<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;Pic++)if(!P->Pic[Pic].References)break;
    if(Pic==SPMI_PMIC_ARB_MAX_IRQ_ENTRIES)return CR_OUT_OF_RESOURCES;
    P->Pic[Pic].Apid=Apid;P->Pic[Pic].Initial=ReadPic(P,Apid);
  }
  ++P->Pic[Pic].References;
  cr_memset(Slot,0,sizeof(*Slot));Slot->Provider=P;Slot->Owner=O;Slot->Identity=Token;
  Slot->Generation=++P->NextGeneration;Slot->Sid=Sid;Slot->Peripheral=Periph;
  Slot->Bit=Bit;Slot->Apid=Apid;Slot->Handler=H;Slot->Context=Context;
  cr_memcpy(Slot->Snapshot,Snapshot,sizeof(Snapshot));Slot->Saved=TRUE;
  Token->Provider=P;Token->Generation=Slot->Generation;
  Status=SpmiRegisterIrq(P->Spmi,Sid,Periph,Bit,Trigger,Notify,Slot);
  if(Status==CR_SUCCESS)Slot->Registered=TRUE;
  return Status;
}
STATIC CR_STATUS Enable(VOID *C,CR_SPMI_IRQ *T,BOOLEAN On) {
  CR_SPMI_OWNER *O=Owner(C);
  if(!O || O->Provider->Dispatching)return CR_NOT_READY;
  CR_SPMI_SUBSCRIPTION *S=Irq(O,T);
  if(!S || !S->Registered || On>TRUE)return CR_INVALID_PARAMETER;
  if(On && (O->Provider->Closed || O->Retiring))return CR_NOT_READY;
  CR_STATUS Status=SpmiEnableIrq(O->Provider->Spmi,S->Sid,S->Peripheral,S->Bit,On);
  if(Status!=CR_SUCCESS)return Status;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++)
    if(O->Provider->Pic[I].References && O->Provider->Pic[I].Apid==S->Apid)
      return ReconcilePic(O->Provider,S->Apid,O->Provider->Pic[I].Initial);
  return CR_DEVICE_ERROR;
}
STATIC CR_STATUS Unsubscribe(VOID *C,CR_SPMI_IRQ *T) {
  CR_SPMI_OWNER *O=Owner(C);CR_STATUS Status;
  if(!O || O->Provider->Dispatching)return CR_NOT_READY;
  CR_SPMI_SUBSCRIPTION *S=Irq(O,T);
  if(!S || S->Invoking)return CR_INVALID_PARAMETER;
  CR_SPMI_SHARED *P=O->Provider;
  if(S->Registered) {
    Status=SpmiUnregisterIrq(P->Spmi,S->Sid,S->Peripheral,S->Bit);
    if(Status!=CR_SUCCESS)return Status;
    S->Registered=FALSE;
  }
  UINT8 Mask=(UINT8)BIT(S->Bit),Value;
  for(UINTN I=0;I<3;I++) {
    Status=SpmiMaskedWrite(P->Spmi,S->Sid,(UINT16)((S->Peripheral<<8)+0x11+I),&S->Snapshot[I],&Mask,1);
    if(Status!=CR_SUCCESS)return Status;
    Status=SpmiRead(P->Spmi,S->Sid,(UINT16)((S->Peripheral<<8)+0x11+I),&Value,1);
    if(Status!=CR_SUCCESS)return Status;
    if((Value&Mask)!=(S->Snapshot[I]&Mask))return CR_DEVICE_ERROR;
  }
  Status=SpmiWrite(P->Spmi,S->Sid,(UINT16)((S->Peripheral<<8)+((S->Snapshot[3]&Mask)?0x15:0x16)),&Mask,1);
  if(Status!=CR_SUCCESS)return Status;
  Status=SpmiRead(P->Spmi,S->Sid,(UINT16)((S->Peripheral<<8)+0x15),&Value,1);
  if(Status!=CR_SUCCESS)return Status;
  if((Value&Mask)!=(S->Snapshot[3]&Mask))return CR_DEVICE_ERROR;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++)if(P->Pic[I].References && P->Pic[I].Apid==S->Apid) {
    Status=ReconcilePic(P,S->Apid,P->Pic[I].Initial);
    if(Status!=CR_SUCCESS)return Status;
    --P->Pic[I].References;break;
  }
  cr_memset(T,0,sizeof(*T));cr_memset(S,0,sizeof(*S));return CR_SUCCESS;
}
STATIC CR_STATUS Check(VOID *C) {
  CR_SPMI_OWNER *O=Owner(C);return O && !O->Retiring && !O->Provider->Closed?CR_SUCCESS:CR_NOT_READY;
}
STATIC CR_STATUS Retire(VOID *C) {
  CR_SPMI_OWNER *O=Owner(C);if(!O || O->Provider->Dispatching)return CR_NOT_READY;
  O->Retiring=TRUE;return CR_SUCCESS;
}
CR_STATUS CrSpmiSharedInit(CR_SPMI_SHARED *P,SpmiDeviceContext *S) {
  if(!P || !S || !S->Initialized || !S->HasInterrupt || !S->InterruptExternallyManaged || !S->SharedParentIrqScan ||
     S->InterruptRegistered || Overlap(P,sizeof(*P),S,sizeof(*S)))return CR_INVALID_PARAMETER;
  if(P->Initialized)return CR_ALREADY_STARTED;
  for(UINTN I=0;I<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;I++)if(S->IrqEntries[I].Handler)return CR_BUSY;
  /* Do not rewind generation across uninit/reinit of resident storage. */
  UINT64 Generation=P->NextGeneration;cr_memset(P,0,sizeof(*P));P->NextGeneration=Generation;
  P->Spmi=S;P->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrSpmiSharedAcquire(CR_SPMI_SHARED *P,VOID *Who,CONST CR_SPMI_REGION *Regions,
                              UINTN N,CR_SPMI_CLIENT *Token,CR_SPMI_BUS *Out) {
  if(!P || !P->Initialized || P->Closed || P->Dispatching)return CR_NOT_READY;
  if(!Who || !Regions || N==0 || N>CR_SPMI_MAX_REGIONS || !Token || !Out ||
     !Safe(P,Token,sizeof(*Token)) || !Safe(P,Out,sizeof(*Out)) || !Safe(P,Regions,N*sizeof(*Regions)) ||
     Overlap(Token,sizeof(*Token),Out,sizeof(*Out)) ||
     Overlap(Regions,N*sizeof(*Regions),Token,sizeof(*Token)) ||
     Overlap(Regions,N*sizeof(*Regions),Out,sizeof(*Out)) ||
     Token->Provider || Token->Owner || Token->Generation)return CR_INVALID_PARAMETER;
  if(P->NextGeneration==~(UINT64)0)return CR_OUT_OF_RESOURCES;
  for(UINTN I=0;I<N;I++) {
    if(Regions[I].Sid>15 || !Regions[I].Access || (Regions[I].Access&~3))return CR_INVALID_PARAMETER;
    for(UINTN J=0;J<I;J++)if(Regions[J].Sid==Regions[I].Sid && Regions[J].Peripheral==Regions[I].Peripheral)return CR_INVALID_PARAMETER;
  }
  for(UINTN I=0;I<CR_SPMI_MAX_CLIENTS;I++)if(!P->Owners[I].Identity) {
    CR_SPMI_OWNER *O=&P->Owners[I];O->Provider=P;O->Identity=Token;O->BusIdentity=Out;O->Owner=Who;
    O->Generation=++P->NextGeneration;O->RegionCount=(UINT8)N;cr_memcpy(O->Regions,Regions,N*sizeof(*Regions));
    Token->Provider=P;Token->Owner=Who;Token->Generation=O->Generation;
    CR_SPMI_BUS Bus={O,O->Generation,Read,Write,Masked,Subscribe,Enable,Unsubscribe,Check,Retire};*Out=Bus;return CR_SUCCESS;
  }
  return CR_OUT_OF_RESOURCES;
}
CR_STATUS CrSpmiSharedRelease(CR_SPMI_SHARED *P,CR_SPMI_CLIENT *T) {
  if(!P || !P->Initialized || P->Dispatching)return CR_NOT_READY;
  for(UINTN I=0;I<CR_SPMI_MAX_CLIENTS;I++)if(P->Owners[I].Identity==T && Owner(P->Owners[I].BusIdentity)) {
    for(UINTN J=0;J<SPMI_PMIC_ARB_MAX_IRQ_ENTRIES;J++)if(P->Irqs[J].Identity && P->Irqs[J].Owner==&P->Owners[I])return CR_BUSY;
    cr_memset(P->Owners[I].BusIdentity,0,sizeof(CR_SPMI_BUS));
    cr_memset(T,0,sizeof(*T));cr_memset(&P->Owners[I],0,sizeof(P->Owners[I]));return CR_SUCCESS;
  }
  return CR_INVALID_PARAMETER;
}
CR_STATUS CrSpmiSharedDispatch(CR_SPMI_SHARED *P) {
  if(!P || !P->Initialized || P->Closed || P->Dispatching)return CR_NOT_READY;
  P->Dispatching=TRUE;CR_STATUS S=SpmiDispatchIrqs(P->Spmi);P->Dispatching=FALSE;return S;
}
CR_STATUS CrSpmiSharedClose(CR_SPMI_SHARED *P) {
  if(!P || !P->Initialized)return CR_INVALID_PARAMETER;
  P->Closed=TRUE;return P->Dispatching?CR_NOT_READY:CR_SUCCESS;
}
CR_STATUS CrSpmiSharedUninit(CR_SPMI_SHARED *P) {
  if(!P || !P->Initialized)return CR_INVALID_PARAMETER;
  if(!P->Closed || P->Dispatching)return CR_NOT_READY;
  for(UINTN I=0;I<CR_SPMI_MAX_CLIENTS;I++)if(P->Owners[I].Identity)return CR_BUSY;
  P->Initialized=FALSE;P->Spmi=NULL;return CR_SUCCESS;
}
