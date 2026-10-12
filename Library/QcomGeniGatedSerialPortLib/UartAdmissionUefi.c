/* Inactive actual UEFI constructor/event/destructor implementation. MIT */
#include <Library/UartAdmissionUefiLib.h>
#include <Guid/EventGroup.h>
#include <Library/SynchronizationLib.h>

static CONST EFI_GUID TableGuid = CR_UART_ADMISSION_TABLE_GUID;
static UINT32 Load(volatile UINT32 *P) { return InterlockedCompareExchange32(P,0,0); }
static BOOLEAN Equal(CONST EFI_GUID *A, CONST EFI_GUID *B) {
  CONST UINT8 *X=(CONST UINT8 *)A,*Y=(CONST UINT8 *)B;
  for(UINTN I=0;I<sizeof(*A);++I)if(X[I]!=Y[I])return FALSE;
  return TRUE;
}
static EFI_STATUS Find(EFI_SYSTEM_TABLE *ST,CR_UART_ADMISSION **Gate) {
  UINTN Found=0;
  if(ST->NumberOfTableEntries>4096||
     (ST->NumberOfTableEntries&&!ST->ConfigurationTable))return EFI_COMPROMISED_DATA;
  for(UINTN I=0;I<ST->NumberOfTableEntries;++I)
    if(Equal(&ST->ConfigurationTable[I].VendorGuid,&TableGuid)) {
      ++Found;*Gate=ST->ConfigurationTable[I].VendorTable;
    }
  if(Found>1)return EFI_COMPROMISED_DATA;
  return Found?EFI_SUCCESS:EFI_NOT_FOUND;
}
static VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context) {
  CR_UART_UEFI_CLIENT *C=Context;
  if(Event!=C->Event)return;
  InterlockedCompareExchange32(&C->Closed,0,1);
  C->AtRuntime=TRUE;
  /* Even if another writer is still active this closes every image's common
   * admission. The separate producer must wait/retire and refuse false proof. */
  if(C->Gate)CrUartAdmissionClose(C->Gate);
}
EFI_STATUS CrUartUefiConstruct(CR_UART_UEFI_CLIENT *C,EFI_SYSTEM_TABLE *ST,
    BOOLEAN Runtime) {
  if(!C||!ST||!ST->BootServices||(Runtime!=FALSE&&Runtime!=TRUE))
    return EFI_INVALID_PARAMETER;
  if(C->Constructing||C->Boot||C->Gate||C->Event||C->Bound||C->AtRuntime)
    return EFI_ALREADY_STARTED;
  C->Constructing=TRUE;C->Closed=1;C->Boot=ST->BootServices;
  if(!C->Boot->RaiseTPL||!C->Boot->RestoreTPL||!C->Boot->AllocatePool||
     !C->Boot->FreePool||!C->Boot->InstallConfigurationTable||
     !C->Boot->CreateEventEx||!C->Boot->CloseEvent) {
    C->Constructing=FALSE;return EFI_INVALID_PARAMETER;
  }
  /* Serialized through actual UEFI notify TPL: no callback can recursively
   * load another instance between allocation and global gate publication. */
  EFI_TPL OldTpl=C->Boot->RaiseTPL(TPL_NOTIFY);
  CR_UART_ADMISSION *Gate=NULL;
  EFI_STATUS S=Find(ST,&Gate);
  if(S==EFI_NOT_FOUND) {
    S=C->Boot->AllocatePool(EfiRuntimeServicesData,sizeof(*Gate),(VOID **)&Gate);
    if(S!=EFI_SUCCESS)goto Out;
    for(UINTN I=0;I<sizeof(*Gate)/sizeof(UINT32);++I)((UINT32 *)Gate)[I]=0;
    S=CrUartAdmissionInitialize(Gate);
    if(S!=EFI_SUCCESS){C->Boot->FreePool(Gate);goto Out;}
    S=C->Boot->InstallConfigurationTable((EFI_GUID *)&TableGuid,Gate);
    if(S!=EFI_SUCCESS) {
      /* A failing firmware publication may have committed. Never free a
       * globally discoverable page or replace it with a second gate. */
      CR_UART_ADMISSION *Published=NULL;
      EFI_STATUS F=Find(ST,&Published);
      if(F!=EFI_SUCCESS||Published!=Gate) {
        if(F==EFI_NOT_FOUND)C->Boot->FreePool(Gate);
        goto Out;
      }
      C->Gate=Gate;goto Out;
    }
  } else if(S!=EFI_SUCCESS)goto Out;
  C->Gate=Gate;
  S=C->Boot->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,C,
      &gEfiEventExitBootServicesGuid,&C->Event);
  if(S!=EFI_SUCCESS)goto Out;
  S=CrUartAdmissionBind(Gate,Runtime);
  if(S!=EFI_SUCCESS)goto Out;
  C->Bound=TRUE;
  InterlockedCompareExchange32(&C->Closed,1,0);
Out:
  /* Constructor errors own no live I/O. Retire this private event while its
   * actual callback address/context are still resident. Common gate pages
   * that were globally published remain permanent for all other images. */
  if(S!=EFI_SUCCESS&&C->Event) {
    EFI_STATUS Cleanup=C->Boot->CloseEvent(C->Event);
    if(Cleanup==EFI_SUCCESS)C->Event=NULL;
  }
  C->Constructing=FALSE;
  C->Boot->RestoreTPL(OldTpl);
  return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;
}
BOOLEAN CrUartUefiEnter(CR_UART_UEFI_CLIENT *C) {
  /* Closed is deliberately inspected BEFORE every pointer/publication field. */
  if(!C||Load(&C->Closed)||InterlockedCompareExchange32(&C->Calls,0,1)!=0)
    return FALSE;
  if(Load(&C->Closed)||!C->Bound||!CrUartAdmissionEnter(C->Gate)) {
    InterlockedCompareExchange32(&C->Calls,1,0);return FALSE;
  }
  return TRUE;
}
EFI_STATUS CrUartUefiLeave(CR_UART_UEFI_CLIENT *C) {
  if(!C||!Load(&C->Calls))return EFI_NOT_READY;
  EFI_STATUS S=CrUartAdmissionLeave(C->Gate);
  if(S==EFI_SUCCESS)InterlockedCompareExchange32(&C->Calls,1,0);
  return S;
}
EFI_STATUS CrUartUefiDestruct(CR_UART_UEFI_CLIENT *C) {
  if(!C)return EFI_INVALID_PARAMETER;
  if(C->AtRuntime)return EFI_UNSUPPORTED; /* no boot/gate pointer at runtime */
  InterlockedCompareExchange32(&C->Closed,0,1);
  if(Load(&C->Calls)||C->Constructing)return EFI_NOT_READY;
  if(!C->Boot)return C->Event?EFI_COMPROMISED_DATA:EFI_SUCCESS;
  EFI_TPL OldTpl=C->Boot->RaiseTPL(TPL_NOTIFY);
  if(C->Event) {
    EFI_STATUS S=C->Boot->CloseEvent(C->Event);
    if(S!=EFI_SUCCESS) {
      C->Boot->RestoreTPL(OldTpl);return EFI_ERROR(S)?S:EFI_NOT_READY;
    }
    C->Event=NULL;
  }
  C->Boot->RestoreTPL(OldTpl);
  C->Bound=FALSE;C->Gate=NULL;C->Boot=NULL;
  return EFI_SUCCESS;
}
BOOLEAN CrUartUefiCanUnload(CONST CR_UART_UEFI_CLIENT *C) {
  return C&&!C->Constructing&&!C->AtRuntime&&!C->Event&&
      !Load((volatile UINT32 *)&C->Calls)&&!C->Bound;
}
