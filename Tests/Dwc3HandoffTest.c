/* Actual USBH ACPI storage, SMMU callbacks, publication and failure lifetime. MIT */
#ifndef _PCD_VALUE_PcdUsbLegacyPhysicalDmaHandoff
#define _PCD_VALUE_PcdUsbLegacyPhysicalDmaHandoff FALSE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Driver/Dwc3CrDxe/Dwc3Handoff.c"

EFI_GUID gEfiAcpiTableProtocolGuid,gEfiEndOfDxeEventGroupGuid,gEfiEventReadyToBootGuid;
STATIC EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
STATIC EFI_DXE_SERVICES Ds;EFI_DXE_SERVICES *gDS=&Ds;
STATIC EFI_SMMU_CR_PROTOCOL Smmu;
STATIC EFI_ACPI_TABLE_PROTOCOL Acpi;
STATIC EFI_SMMU_CR_EXIT_PREPARE Before;
STATIC EFI_SMMU_CR_EXIT_OBSERVER After;
STATIC VOID *BeforeContext,*AfterContext,*Page,*Pool;
typedef struct {EFI_EVENT_NOTIFY Fn;VOID *Context;} EVENT;
STATIC EVENT *Events[3];
STATIC UINTN Calls,FailAt,Pages,Pools,Tables,EventCount,Flushes,Counter;
STATIC UINTN Queries,Closes,FailClose;
STATIC EFI_STATUS Failure,UnregisterStatus,QueryStatus,FreezeStatus;
STATIC EFI_STATUS InstallCommittedStatus,RemoveStatus;
STATIC BOOLEAN Ebs,Done,InterruptPrepare,BadGcd,BadPage,BadTable,Duplicate,ShortMap;
STATIC UINT32 BadProof;
STATIC CR_USB_NATIVE_LOCATOR Locator;

STATIC EFI_STATUS Step(VOID){return ++Calls==FailAt?Failure:EFI_SUCCESS;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID EFIAPI MemoryFence(VOID){}
UINT8 EFIAPI CalculateCheckSum8(CONST UINT8 *P,UINTN N){UINT8 V=0;for(UINTN I=0;I<N;++I)V+=P[I];return (UINT8)(0-V);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N){
  assert(Page && (UINTN)P>=(UINTN)Page && N<=4096 && (UINTN)P-(UINTN)Page<=4096-N);++Flushes;
  if(Ebs){
    CR_USB_NATIVE_HANDOFF *R=Page;assert(R->Sequence==1||R->Sequence==2);
    if(R->Sequence==2)assert(CrUsbHandoffValidate(R)==CR_SUCCESS);
  }
  return P;
}
STATIC EFI_STATUS EFIAPI GetCount(UINT64 *N){assert(!Ebs);EFI_STATUS S=Step();if(S==EFI_SUCCESS)*N=++Counter;return S;}
STATIC EFI_STATUS EFIAPI AllocPages(EFI_ALLOCATE_TYPE A,EFI_MEMORY_TYPE T,UINTN N,EFI_PHYSICAL_ADDRESS *P){
  assert(!Ebs && !Pages && A==AllocateAnyPages && T==EfiReservedMemoryType && N==1);
  EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;
  Page=aligned_alloc(4096,4096);assert(Page);memset(Page,0xa7,4096);*P=(UINTN)Page;Pages=1;
  if(InterruptPrepare){Ebs=TRUE;assert(Before);assert(Before(BeforeContext)==EFI_NOT_READY);}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FreePages(EFI_PHYSICAL_ADDRESS P,UINTN N){
  assert(!Ebs && P==(UINTN)Page && N==1 && Pages && !Tables);
  EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;free(Page);Page=NULL;Pages=0;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AllocPool(EFI_MEMORY_TYPE T,UINTN N,VOID **P){
  assert(!Ebs && !Pools && T==EfiBootServicesData && N>=65536);
  EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;Pool=malloc(N);assert(Pool);*P=Pool;Pools=1;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FreePoolMock(VOID *P){
  assert(!Ebs && P==Pool && Pools && !Tables);EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;
  free(P);Pool=NULL;Pools=0;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;memset(D,0,sizeof(*D));
  D->BaseAddress=A;D->Length=0x1000000;D->GcdMemoryType=EfiGcdMemoryTypeReserved;
  D->Capabilities=EFI_MEMORY_UC|EFI_MEMORY_WB;D->Attributes=BadGcd?EFI_MEMORY_UC:EFI_MEMORY_WB;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Map(UINTN *Size,EFI_MEMORY_DESCRIPTOR *D,UINTN *Key,UINTN *Stride,UINT32 *Version){
  *Key=1;*Stride=sizeof(*D);*Version=EFI_MEMORY_DESCRIPTOR_VERSION;
  EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;
  if(!D){*Size=sizeof(*D)*3;return EFI_BUFFER_TOO_SMALL;}
  if(ShortMap){*Size+=4096;return EFI_BUFFER_TOO_SMALL;}
  assert(*Size>=sizeof(*D)*3);memset(D,0,sizeof(*D)*3);
  D[0].Type=BadPage?EfiBootServicesData:EfiReservedMemoryType;D[0].PhysicalStart=(UINTN)Page;D[0].NumberOfPages=1;
  D[1].Type=BadTable?EfiConventionalMemory:EfiReservedMemoryType;D[1].PhysicalStart=0x83000000;D[1].NumberOfPages=34;
  // Capability UC does not mean active Device memory; GCD qualifies that.
  D[0].Attribute=D[1].Attribute=EFI_MEMORY_WB|EFI_MEMORY_UC;
  *Size=2*sizeof(*D);if(Duplicate){D[2]=D[0];*Size+=sizeof(*D);}return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Install(EFI_ACPI_TABLE_PROTOCOL *T,VOID *P,UINTN N,UINTN *Key){
  assert(!Ebs && T==&Acpi && N==60 && !Tables);EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;
  assert(CalculateCheckSum8(P,N)==0);memcpy(&Locator,P,N);*Key=55;Tables=1;return InstallCommittedStatus;
}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_ACPI_TABLE_PROTOCOL *T,UINTN Key){
  assert(!Ebs && T==&Acpi && Key==55 && Tables);EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;
  if(RemoveStatus!=EFI_SUCCESS)return RemoveStatus;Tables=0;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){
  assert(!Ebs && G==&gEfiAcpiTableProtocolGuid && !R);EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;*P=&Acpi;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL L,EFI_EVENT_NOTIFY Fn,VOID *C,EFI_EVENT *Out){
  assert(!Ebs && T==EVT_NOTIFY_SIGNAL && L==TPL_CALLBACK && EventCount<3);
  EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;
  EVENT *E=malloc(sizeof(*E));assert(E);*E=(EVENT){Fn,C};Events[EventCount++]=E;*Out=E;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CreateEx(UINT32 T,EFI_TPL L,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){
  assert(G==&gEfiEndOfDxeEventGroupGuid||G==&gEfiEventReadyToBootGuid);return Create(T,L,Fn,(VOID*)C,Out);
}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT Event){
  assert(!Ebs);if(++Closes==FailClose)return EFI_DEVICE_ERROR;
  // Closing must make a queued Prepare harmless before any release completes.
  EVENT *E=Event;UINTN N=Calls;E->Fn(E,E->Context);assert(Calls==N);
  for(UINTN I=0;I<3;++I)if(Events[I]==E){free(E);Events[I]=NULL;return EFI_SUCCESS;}abort();
}
STATIC EFI_STATUS EFIAPI Notify(EFI_GUID *G,EFI_EVENT E,VOID **R){assert(!Ebs && G==&gEfiAcpiTableProtocolGuid);*R=E;return Step();}
STATIC EFI_STATUS EFIAPI SetBefore(EFI_SMMU_CR_PROTOCOL *P,VOID *Owner,EFI_SMMU_CR_EXIT_PREPARE C,VOID *Context){
  assert(!Ebs && P==&Smmu && Owner==&mUsbHandoff);
  EFI_STATUS S=C?Step():UnregisterStatus;if(S!=EFI_SUCCESS)return S;Before=C;BeforeContext=Context;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetAfter(EFI_SMMU_CR_PROTOCOL *P,VOID *Owner,EFI_SMMU_CR_EXIT_OBSERVER C,VOID *Context){
  assert(!Ebs && P==&Smmu && Owner==&mUsbHandoff);
  EFI_STATUS S=C?Step():UnregisterStatus;if(S!=EFI_SUCCESS)return S;After=C;AfterContext=Context;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Query(EFI_SMMU_CR_PROTOCOL *P,UINT16 Segment,UINT16 Bdf,UINT16 Sid,SmmuMappingEvidence *E){
  assert(Ebs && Done && P==&Smmu && !Segment && Bdf==0xffff && !Sid);++Queries;memset(E,0,sizeof(*E));
  if(QueryStatus!=EFI_SUCCESS)return QueryStatus;
  E->Base=0x15000000;E->BankBase=0x15041000;E->Bank=1;E->Stream=3;E->Asid=5;
  E->Tables=0x83000000;E->TableBytes=34*4096;E->Iova=0x10000000;E->Bytes=0x4000000;
  E->Smr=0x80000000;E->S2cr=1;E->Sctlr=0x1027;E->Cbar=0x1f300;E->Cba2r=1;
  E->Tcr=0x80351c;E->Tcr2=0x38012;E->Ttbr0=E->Tables|((UINT64)5<<48);E->Ttbr1=(UINT64)5<<48;E->Mair0=0x44;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FreezeWriters(CR_USB_NATIVE_HANDOFF *R){
  assert(Ebs);R->FrozenWriters=R->DrainedWriters=7;
  R->Stop=(CR_USB_CONTROLLER_STOP){.Role=1,.Id=0x33310000,.Revision=0x3139302a,.GlobalControl=1U<<12,.HostStatus=1,.EventBuffers=1};
  R->HaltStatus=R->InterruptStatus=EFI_SUCCESS;
  if(BadProof==1)R->ActiveCalls[1]=1;
  if(BadProof==2)R->DrainedWriters=3;
  if(BadProof==3)R->HaltStatus=EFI_TIMEOUT;
  if(BadProof==4)R->InterruptStatus=EFI_WARN_UNKNOWN_GLYPH;
  if(BadProof==5)R->Stop.HostCommand=1;
  return FreezeStatus;
}
STATIC VOID Setup(VOID){
  assert(!Pages&&!Pools&&!Tables);for(UINTN I=0;I<3;++I)assert(!Events[I]);
  memset(&mUsbHandoff,0,sizeof(mUsbHandoff));memset(&Bs,0,sizeof(Bs));memset(&Ds,0,sizeof(Ds));
  Calls=FailAt=EventCount=Flushes=Counter=Queries=Closes=FailClose=0;Before=NULL;After=NULL;
  Failure=EFI_DEVICE_ERROR;UnregisterStatus=QueryStatus=FreezeStatus=EFI_SUCCESS;
  InstallCommittedStatus=RemoveStatus=EFI_SUCCESS;
  Ebs=Done=InterruptPrepare=BadGcd=BadPage=BadTable=Duplicate=ShortMap=FALSE;BadProof=0;
  Bs.AllocatePages=AllocPages;Bs.FreePages=FreePages;Bs.AllocatePool=AllocPool;Bs.FreePool=FreePoolMock;
  Bs.GetNextMonotonicCount=GetCount;Bs.GetMemoryMap=Map;Bs.LocateProtocol=Locate;
  Bs.CreateEvent=Create;Bs.CreateEventEx=CreateEx;Bs.CloseEvent=Close;Bs.RegisterProtocolNotify=Notify;
  Ds.GetMemorySpaceDescriptor=Gcd;Acpi=(EFI_ACPI_TABLE_PROTOCOL){Install,Uninstall};
  Smmu=(EFI_SMMU_CR_PROTOCOL){.Revision=EFI_SMMU_CR_PROTOCOL_REVISION,.SetExitPrepareObserver=SetBefore,.SetExitObserver=SetAfter,.QueryExitRevokedDomain=Query};
}
STATIC VOID Reboot(VOID){
  // Simulated reboot only; EBS has no release path in production.
  free(Page);free(Pool);Page=Pool=NULL;Pages=Pools=Tables=0;
  for(UINTN I=0;I<3;++I){free(Events[I]);Events[I]=NULL;}memset(&mUsbHandoff,0,sizeof(mUsbHandoff));
}
STATIC VOID ExitImage(BOOLEAN Early){
  Ebs=TRUE;if(Early)CrUsbNativeExit();assert(Before(BeforeContext)==FreezeStatus);
  assert(!mUsbHandoff.Published);Done=TRUE;After(AfterContext,EFI_SUCCESS);
  UINTN N=Flushes;CrUsbNativeExit();After(AfterContext,EFI_SUCCESS);assert(Flushes==N && Queries==1);
  assert(CrUsbNativeRelease()==EFI_ACCESS_DENIED && Pages==1 && Pools==1 && Tables==1);
}
int main(int argc,char **argv){
  assert(argc==1 || argc==3);
  Setup();assert(CrUsbNativeInit(&Smmu,FreezeWriters)==EFI_SUCCESS);UINTN Total=Calls;
  assert(mUsbHandoff.Installed && Locator.Record==(UINTN)Page && Locator.Length==320 && Locator.Generation==1);
  assert(!memcmp(Locator.Header.OemId,"ALOHA ",6)&&!memcmp(&Locator.Header.OemTableId,"USBHDK84",8));
  assert(CrUsbNativeInit(&Smmu,FreezeWriters)==EFI_ALREADY_STARTED);assert(CrUsbNativeRelease()==EFI_SUCCESS);
  for(UINTN Warn=0;Warn<2;++Warn)for(UINTN I=1;I<=Total;++I){
    Setup();FailAt=I;Failure=Warn?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
    EFI_STATUS S=CrUsbNativeInit(&Smmu,FreezeWriters);
    if(S==EFI_SUCCESS)assert(!mUsbHandoff.Installed); // Storage retries are deliberately nonfatal.
    FailAt=0;assert(CrUsbNativeRelease()==EFI_SUCCESS);
  }
  for(UINTN Early=0;Early<2;++Early){
    Setup();assert(CrUsbNativeInit(&Smmu,FreezeWriters)==EFI_SUCCESS);ExitImage((BOOLEAN)Early);
    assert(mUsbHandoff.Published && CrUsbHandoffCanTakeover(Page,Locator.Generation)==CR_SUCCESS);
    if(argc==3 && !Early){
      FILE *F=fopen(argv[1],"wb");assert(F && fwrite(Page,1,320,F)==320 && !fclose(F));
      F=fopen(argv[2],"wb");assert(F && fwrite(&Locator,1,sizeof(Locator),F)==sizeof(Locator) && !fclose(F));
    }
    assert(CrUsbHandoffCanTakeover(Page,Locator.Generation+1)==CR_NOT_READY);Reboot();
  }
  // Actual MU commits a key before PublishTables can fail. Never duplicate its
  // locator on retry or free its referenced page after a non-exact removal.
  for(UINTN Warn=0;Warn<2;++Warn)for(UINTN Retry=0;Retry<2;++Retry){
    Setup();InstallCommittedStatus=Warn?EFI_WARN_UNKNOWN_GLYPH:EFI_ABORTED;
    assert(CrUsbNativeInit(&Smmu,FreezeWriters)==EFI_SUCCESS);
    assert(!mUsbHandoff.Installed && mUsbHandoff.TableOwned && Tables && Pages);
    VOID *Saved=Page;RemoveStatus=Warn?EFI_WARN_UNKNOWN_GLYPH:EFI_NOT_FOUND;
    if(Retry){Prepare(NULL,NULL);assert(!mUsbHandoff.Installed && Tables && Page==Saved);}
    else assert(CrUsbNativeRelease()!=EFI_SUCCESS && Tables && Page==Saved && Pools);
    RemoveStatus=InstallCommittedStatus=EFI_SUCCESS;
    if(Retry){Prepare(NULL,NULL);assert(mUsbHandoff.Installed && Tables && Page==Saved);}
    assert(CrUsbNativeRelease()==EFI_SUCCESS);
  }
  for(UINTN Case=0;Case<13;++Case){
    Setup();assert(CrUsbNativeInit(&Smmu,FreezeWriters)==EFI_SUCCESS);
    if(Case<5)BadProof=(UINT32)Case+1;
    if(Case==5)FreezeStatus=EFI_NOT_READY;
    if(Case==6)QueryStatus=EFI_DEVICE_ERROR;
    if(Case==7)QueryStatus=EFI_WARN_UNKNOWN_GLYPH;
    if(Case==8)BadPage=TRUE;if(Case==9)BadTable=TRUE;
    if(Case==10)BadGcd=TRUE;if(Case==11)Duplicate=TRUE;if(Case==12)ShortMap=TRUE;
    ExitImage(FALSE);
    assert(CrUsbHandoffCanTakeover(Page,Locator.Generation)!=CR_SUCCESS);
    if(mUsbHandoff.Published)assert(CrUsbHandoffValidate(Page)==CR_SUCCESS && ((CR_USB_NATIVE_HANDOFF*)Page)->State==CR_USBH_UNCERTAIN);
    Reboot();
  }
  // Late ACPI/storage retry interrupted by EBS must not memset a just-reserved page.
  Setup();FailAt=7;assert(CrUsbNativeInit(&Smmu,FreezeWriters)==EFI_SUCCESS && !mUsbHandoff.PageOwned);
  FailAt=0;InterruptPrepare=TRUE;Prepare(NULL,NULL);assert(mUsbHandoff.Frozen && Page && ((UINT8*)Page)[0]==0xa7);Reboot();
  Setup();assert(CrUsbNativeInit(&Smmu,FreezeWriters)==EFI_SUCCESS);
  UnregisterStatus=EFI_WARN_UNKNOWN_GLYPH;assert(CrUsbNativeRelease()==EFI_NOT_READY && Before && After && Pages && Pools && Tables);
  UnregisterStatus=EFI_SUCCESS;FailClose=1;assert(CrUsbNativeRelease()==EFI_DEVICE_ERROR && Pages && Pools && Tables && !Before && !After);
  FailClose=0;assert(CrUsbNativeRelease()==EFI_SUCCESS);
  printf("USBH native: %zu setup fault phases, both EBS orders,13 false-proof/memory cases,4 committed-install retries, interrupted prepare and retained cleanup PASS\n",(size_t)(Total*2));
  return 0;
}
