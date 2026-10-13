/* No hardware operations or allocations. SPDX-License-Identifier: MIT */
#include <Library/UsbHandoffCrLib.h>
#include <oskal/cr_memory.h>

STATIC VOID Copy(VOID *D,CONST VOID *S,UINTN N){
  for(UINTN I=0;I<N;++I)((UINT8 *)D)[I]=((CONST UINT8 *)S)[I];
}
STATIC BOOLEAN Zero(CONST VOID *P,UINTN N){
  for(UINTN I=0;I<N;++I)if(((CONST UINT8 *)P)[I])return FALSE;return TRUE;
}
STATIC BOOLEAN Equal(CONST VOID *A,CONST VOID *B,UINTN N){
  for(UINTN I=0;I<N;++I)if(((CONST UINT8 *)A)[I]!=((CONST UINT8 *)B)[I])return FALSE;return TRUE;
}
STATIC BOOLEAN Range(UINT64 B,UINT64 N){return B&&N&&B<=CR_MAX_UINT64-N;}
STATIC BOOLEAN Overlap(UINT64 A,UINT64 N,UINT64 B,UINT64 M){return A<B+M&&B<A+N;}
STATIC CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
STATIC BOOLEAN Role(UINT32 R){return R==CR_USBH_HOST||R==CR_USBH_FUNCTION;}

CR_STATUS CrUsbControllerStopped(CONST CR_USB_CONTROLLER_STOP *S){
  if(!S||!Role(S->Role)||((S->Id>>16)!=0x5533&&(S->Id>>16)!=0x3331)||
     ((S->GlobalControl>>12)&3U)!=S->Role||!S->EventBuffers||S->EventBuffers>32||
     S->MaskedEventBuffers>S->EventBuffers)return CR_INVALID_PARAMETER;
  /* USBCMD.INTE plus RunStop must be clear; HCHalted must be observed.
     Function mode additionally closes every device/endpoint interrupt source
     and masks each reported GEVNT buffer. FIFO emptiness is not DMA proof. */
  if(S->Role==CR_USBH_HOST)
    return !(S->HostCommand&5U)&&(S->HostStatus&1U)?CR_SUCCESS:CR_NOT_READY;
  return !(S->DeviceControl&0x80000000U)&&(S->DeviceStatus&0x400000U)&&
    !S->DeviceEvents&&!S->DeviceEndpoints&&S->MaskedEventBuffers==S->EventBuffers?
    CR_SUCCESS:CR_NOT_READY;
}
STATIC BOOLEAN Revoked(CONST SmmuMappingEvidence *E){
  return E->Base==0x15000000ULL&&E->BankBase>=E->Base&&
    E->BankBase-E->Base<=0x100000U-0x800U&&!(E->BankBase&4095)&&
    Range(E->Tables,E->TableBytes)&&!(E->Tables&4095)&&
    E->TableBytes==SMMU_TABLE_PAGES*SMMU_PAGE_SIZE&&
    E->Sid==0&&E->Bank<256&&E->Stream<128&&E->Asid&&E->Asid<256&&
    E->Smr==0x80000000U&&E->S2cr==E->Bank&&
    !(E->SctlrIgnoreMask&~0x1000U)&&
    (E->Sctlr&~E->SctlrIgnoreMask)==(0x1027U&~E->SctlrIgnoreMask)&&
    E->Cbar==0x1f300U&&E->Cba2r==1&&
    (E->Tcr&~63U)==0x803500U&&(E->Tcr&63U)>=25&&(E->Tcr&63U)<=32&&
    (E->Tcr2&0x38010U)==0x38010U&&(E->Tcr2&15U)<=5&&
    E->Ttbr0==(E->Tables|((UINT64)E->Asid<<48))&&
    E->Ttbr1==((UINT64)E->Asid<<48)&&E->Mair0==0x44&&!E->Mair1&&
    E->Iova==SMMU_IOVA_BASE&&E->Bytes==SMMU_IOVA_SIZE&&!E->Physical&&!E->Access;
}
STATIC BOOLEAN Facts(CONST CR_USB_NATIVE_HANDOFF *R){
  return R->ExpectedWriters==CR_USBH_ALL_WRITERS&&
    R->FrozenWriters==R->ExpectedWriters&&R->DrainedWriters==R->ExpectedWriters&&
    !R->FailedWriters&&Zero(R->ActiveCalls,sizeof(R->ActiveCalls))&&
    !R->FreezeStatus&&!R->HaltStatus&&!R->InterruptStatus&&!R->RevocationStatus&&
    CrUsbControllerStopped(&R->Stop)==CR_SUCCESS&&Revoked(&R->Revoked);
}
STATIC BOOLEAN Schema(CONST CR_USB_NATIVE_HANDOFF *R){
  return R&&R->Signature==CR_USBH_SIGNATURE&&R->Revision==CR_USBH_REVISION&&
    R->Length==sizeof(*R)&&R->Sequence&&!(R->Sequence&1)&&R->BootGeneration&&R->TransferId&&
    R->Soc==0x8450&&R->Board==1&&R->Controller==CR_USBH_CONTROLLER&&
    R->ControllerBytes==CR_USBH_CONTROLLER_BYTES&&R->Qscratch==CR_USBH_QSCRATCH&&
    R->QscratchBytes==CR_USBH_QSCRATCH_BYTES&&!R->Sid&&!R->Reserved0&&
    !(R->ExpectedWriters&~CR_USBH_ALL_WRITERS)&&
    !(R->FrozenWriters&~R->ExpectedWriters)&&!(R->DrainedWriters&~R->FrozenWriters)&&
    !(R->FailedWriters&~R->ExpectedWriters)&&R->Stop.Role<=CR_USBH_FUNCTION&&
    R->Stop.EventBuffers<=32&&R->Stop.MaskedEventBuffers<=R->Stop.EventBuffers&&
    (R->State==CR_USBH_UNCERTAIN||(R->State==CR_USBH_RETIRED&&Facts(R)));
}
UINT32 CrUsbHandoffCrc32(CONST CR_USB_NATIVE_HANDOFF *R){
  if(!R)return 0;UINT32 C=~0U;
  for(UINTN I=0;I<sizeof(*R);++I){
    UINT8 B=I>=offsetof(CR_USB_NATIVE_HANDOFF,Crc32)&&I<offsetof(CR_USB_NATIVE_HANDOFF,Crc32)+4?
      0:((CONST UINT8 *)R)[I];C^=B;
    for(UINT32 J=0;J<8;++J)C=(C>>1)^(0xedb88320U&((UINT32)0-(C&1U)));
  }
  return ~C;
}
CR_STATUS CrUsbHandoffSeal(CR_USB_NATIVE_HANDOFF *R,UINT64 Sequence){
  if(!R||!Sequence||(Sequence&1))return CR_INVALID_PARAMETER;
  CR_USB_NATIVE_HANDOFF T;Copy(&T,R,sizeof(T));T.Sequence=Sequence;T.Crc32=0;
  if(!Schema(&T))return CR_COMPROMISED_DATA;
  T.Crc32=CrUsbHandoffCrc32(&T);Copy(R,&T,sizeof(T));return CR_SUCCESS;
}
CR_STATUS CrUsbHandoffValidate(CONST CR_USB_NATIVE_HANDOFF *R){
  return Schema(R)&&R->Crc32==CrUsbHandoffCrc32(R)?CR_SUCCESS:CR_COMPROMISED_DATA;
}
CR_STATUS CrUsbHandoffCanTakeover(CONST CR_USB_NATIVE_HANDOFF *R,UINT64 Boot){
  CR_STATUS S=CrUsbHandoffValidate(R);if(S!=CR_SUCCESS)return S;
  return Boot&&Boot==R->BootGeneration&&R->State==CR_USBH_RETIRED&&Facts(R)?CR_SUCCESS:CR_NOT_READY;
}
CR_STATUS CrUsbHandoffSnapshot(CONST volatile VOID *P,UINTN Bytes,
    CR_USB_NATIVE_HANDOFF *Scratch,CR_USB_NATIVE_HANDOFF *Out){
  if(!P||!Scratch||!Out||Bytes!=sizeof(*Scratch)||((UINTN)P&7)||
     ((UINTN)Scratch&7)||((UINTN)Out&7)||!Range((UINTN)P,Bytes)||
     !Range((UINTN)Scratch,Bytes)||!Range((UINTN)Out,Bytes)||
     Overlap((UINTN)P,Bytes,(UINTN)Scratch,Bytes)||
     Overlap((UINTN)P,Bytes,(UINTN)Out,Bytes)||Overlap((UINTN)Scratch,Bytes,(UINTN)Out,Bytes))
    return CR_INVALID_PARAMETER;
  CONST volatile UINT64 *Seq=(CONST volatile UINT64 *)((CONST volatile UINT8 *)P+16);
  UINT64 Before=*Seq;if(!Before||(Before&1))return CR_NOT_READY;CrMemoryFence();
  for(UINTN I=0;I<Bytes;++I)((UINT8 *)Scratch)[I]=((CONST volatile UINT8 *)P)[I];
  CrMemoryFence();UINT64 After=*Seq;
  if(Before!=After||Scratch->Sequence!=Before)return CR_NOT_READY;
  CR_STATUS S=CrUsbHandoffValidate(Scratch);if(S!=CR_SUCCESS)return S;
  Copy(Out,Scratch,Bytes);return CR_SUCCESS;
}
STATIC CR_STATUS Parent(CONST CR_USB_ROLE_BOUNDARY *B,VOID *Owner){
  if(!B||!Owner||!B->Initialized||B->ParentOwner!=Owner)return CR_INVALID_PARAMETER;
  return B->Quarantined?CR_NOT_READY:CR_SUCCESS;
}
STATIC BOOLEAN Child(CONST CR_USB_ROLE_BOUNDARY *B,VOID *Owner,CONST CR_USB_ROLE_TOKEN *T){
  return B&&B->Initialized&&Owner&&Owner==B->ChildOwner&&T&&T==B->Token&&
    Equal(T,&B->TokenImage,sizeof(*T));
}
CR_STATUS CrUsbRoleInit(CR_USB_ROLE_BOUNDARY *B,VOID *Owner,UINT64 Boot,UINT64 Assignment){
  if(!B||!Owner||!Boot||!Assignment)return CR_INVALID_PARAMETER;
  if(!Zero(B,sizeof(*B)))return CR_ALREADY_STARTED;
  B->ParentOwner=Owner;B->BootGeneration=Boot;B->Assignment=Assignment;B->Initialized=TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrUsbRoleImportNative(CR_USB_ROLE_BOUNDARY *B,VOID *Owner,CONST CR_USB_NATIVE_HANDOFF *R){
  CR_STATUS S=Parent(B,Owner);if(S!=CR_SUCCESS)return S;
  S=CrUsbHandoffCanTakeover(R,B->BootGeneration);if(S!=CR_SUCCESS)return S;
  if(B->Imported)return Equal(&B->NativeRecord,R,sizeof(*R))?CR_SUCCESS:CR_ALREADY_STARTED;
  if(B->ChildOwner||B->PendingRole||B->Role)return CR_BUSY;
  Copy(&B->NativeRecord,R,sizeof(*R));
  B->TransferId=R->TransferId;B->Imported=B->Halted=TRUE;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleCanHalt(CONST CR_USB_ROLE_BOUNDARY *B,VOID *Owner){
  CR_STATUS S=Parent(B,Owner);if(S!=CR_SUCCESS)return S;
  if(!B->Imported)return CR_NOT_READY;
  return !B->Calls&&(!B->ChildOwner||B->Stopped)?CR_SUCCESS:CR_BUSY;
}
CR_STATUS CrUsbRoleConfirmHalt(CR_USB_ROLE_BOUNDARY *B,VOID *Owner,CONST CR_USB_CONTROLLER_STOP *R){
  CR_STATUS S=CrUsbRoleCanHalt(B,Owner);if(S!=CR_SUCCESS)return S;
  /* A partially programmed role can still be the former role. The report's
     actual GCTL mode, not the desired request, selects the halt register. */
  if(!R||(!B->PendingRole&&B->Role&&R->Role!=B->Role))return CR_INVALID_PARAMETER;
  S=CrUsbControllerStopped(R);if(S==CR_SUCCESS)B->Halted=TRUE;return S;
}
CR_STATUS CrUsbRoleBeginConfigure(CR_USB_ROLE_BOUNDARY *B,VOID *Owner,UINT32 R){
  CR_STATUS S=Parent(B,Owner);if(S!=CR_SUCCESS)return S;
  if(!Role(R))return CR_INVALID_PARAMETER;
  if(!B->Imported||!B->Halted)return CR_NOT_READY;
  if(B->ChildOwner||B->Calls||(B->PendingRole&&B->PendingRole!=R))return CR_BUSY;
  B->PendingRole=R;B->Halted=FALSE;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleCommitConfigure(CR_USB_ROLE_BOUNDARY *B,VOID *Owner,CR_STATUS Configure,
    CONST CR_USB_CONTROLLER_STOP *R){
  CR_STATUS S=Parent(B,Owner);if(S!=CR_SUCCESS)return S;
  if(!B->PendingRole||B->ChildOwner||!R||R->Role!=B->PendingRole)return CR_INVALID_PARAMETER;
  S=Exact(Configure);if(S!=CR_SUCCESS)return S;
  S=CrUsbControllerStopped(R);if(S!=CR_SUCCESS)return S;
  B->Role=B->PendingRole;B->PendingRole=0;B->Halted=TRUE;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleCleanupComplete(CR_USB_ROLE_BOUNDARY *B,VOID *Owner,CR_STATUS Cleanup){
  CR_STATUS S=Parent(B,Owner);if(S!=CR_SUCCESS)return S;
  if(B->ChildOwner||B->Calls||!B->Halted)return CR_BUSY;
  S=Exact(Cleanup);if(S!=CR_SUCCESS)return S;
  B->PendingRole=B->Role=0;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleReserveChild(CR_USB_ROLE_BOUNDARY *B,VOID *Owner,VOID *C,
    UINT64 Assignment,UINT32 R,CR_USB_ROLE_TOKEN *T){
  CR_STATUS S=Parent(B,Owner);if(S!=CR_SUCCESS)return S;
  if(!C||C==Owner||!T||((UINTN)T&7)||!Range((UINTN)T,sizeof(*T))||
     !Range((UINTN)B,sizeof(*B))||Overlap((UINTN)T,sizeof(*T),(UINTN)B,sizeof(*B))||
     !Zero(T,sizeof(*T))||!Role(R)||Assignment!=B->Assignment)return CR_INVALID_PARAMETER;
  if(!B->Imported||!B->Halted||B->Role!=R)return CR_NOT_READY;
  if(B->ChildOwner||B->PendingRole||B->Calls)return CR_BUSY;
  if(B->NextIncarnation==CR_MAX_UINT64)return CR_OUT_OF_RESOURCES;
  B->TokenImage.Incarnation=++B->NextIncarnation;B->TokenImage.Assignment=Assignment;
  B->TokenImage.Role=R;B->TokenImage.Reserved=0;Copy(T,&B->TokenImage,sizeof(*T));
  B->ChildOwner=C;B->Token=T;B->Closing=B->Stopped=FALSE;B->Halted=FALSE;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleEnter(CR_USB_ROLE_BOUNDARY *B,VOID *C,CONST CR_USB_ROLE_TOKEN *T){
  if(!Child(B,C,T))return CR_INVALID_PARAMETER;
  if(B->Closing||B->Quarantined)return CR_NOT_READY;
  if(B->Calls==CR_MAX_UINT32)return CR_OUT_OF_RESOURCES;
  ++B->Calls;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleLeave(CR_USB_ROLE_BOUNDARY *B,VOID *C,CONST CR_USB_ROLE_TOKEN *T){
  if(!Child(B,C,T)||!B->Calls)return CR_INVALID_PARAMETER;
  --B->Calls;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleStopChild(CR_USB_ROLE_BOUNDARY *B,VOID *C,CONST CR_USB_ROLE_TOKEN *T){
  if(!Child(B,C,T))return CR_INVALID_PARAMETER;
  B->Closing=TRUE;if(B->Quarantined)return CR_NOT_READY;
  if(B->Calls)return CR_BUSY;B->Stopped=TRUE;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleRestartChild(CR_USB_ROLE_BOUNDARY *B,VOID *C,CONST CR_USB_ROLE_TOKEN *T,UINT64 Assignment){
  if(!Child(B,C,T)||Assignment!=B->Assignment)return CR_INVALID_PARAMETER;
  if(B->Quarantined)return CR_NOT_READY;
  if(!B->Stopped||B->Calls||B->PendingRole)return CR_BUSY;
  B->Closing=B->Stopped=B->Halted=FALSE;return CR_SUCCESS;
}
CR_STATUS CrUsbRoleRemoveChild(CR_USB_ROLE_BOUNDARY *B,VOID *C,CR_USB_ROLE_TOKEN *T){
  if(!Child(B,C,T))return CR_INVALID_PARAMETER;
  B->Closing=TRUE;if(B->Quarantined)return CR_NOT_READY;
  if(B->Calls)return CR_BUSY;
  B->ChildOwner=NULL;B->Token=NULL;B->Stopped=FALSE;
  for(UINTN I=0;I<sizeof(*T);++I)((UINT8 *)T)[I]=0;
  for(UINTN I=0;I<sizeof(B->TokenImage);++I)((UINT8 *)&B->TokenImage)[I]=0;
  return CR_SUCCESS;
}
CR_STATUS CrUsbRoleQuarantine(CR_USB_ROLE_BOUNDARY *B,VOID *Owner){
  if(!B||!Owner||!B->Initialized||B->ParentOwner!=Owner)return CR_INVALID_PARAMETER;
  B->Quarantined=B->Closing=TRUE;return CR_SUCCESS;
}
