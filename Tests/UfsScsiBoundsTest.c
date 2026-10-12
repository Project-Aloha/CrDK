/** Actual MU descriptor/preparation and sync/async response paths. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#undef NULL
#include "UfsPassThru.h"
static EFI_TPL EFIAPI Raise(EFI_TPL t){return TPL_APPLICATION;}
static VOID EFIAPI Restore(EFI_TPL t){}
static EFI_STATUS EFIAPI Signal(EFI_EVENT e);
static EFI_BOOT_SERVICES services={.RaiseTPL=Raise,.RestoreTPL=Restore,.SignalEvent=Signal};
EFI_BOOT_SERVICES *gBS=&services;
static unsigned mode, callbacks;
typedef struct {
 UINT8 Flags, Code, Status, Result, Ocs, Capacity, SenseCopied, Ehs;
 UINT16 Segment, Sense;
 UINT32 Residual, Length;
 BOOLEAN Invalid;
} RESPONSE_CASE;
static CONST RESPONSE_CASE Cases[]={
 {.Code=0x21,.Flags=0x20,.Residual=8,.Capacity=64,.Length=0},
 {.Code=0x21,.Flags=0x20,.Residual=9,.Capacity=64,.Length=0,.Invalid=TRUE},
 {.Code=0x21,.Flags=0x20,.Capacity=64,.Length=8},
 {.Code=0x21,.Status=2,.Segment=20,.Sense=18,.Capacity=64,.SenseCopied=18,.Length=8},
 {.Code=0x21,.Status=2,.Segment=21,.Sense=19,.Capacity=64,.SenseCopied=18,.Length=8},
 {.Code=0x21,.Status=2,.Segment=19,.Sense=18,.Capacity=64,.Length=0,.Invalid=TRUE},
 {.Code=0x21,.Sense=18,.Capacity=64,.Length=8}, /* No data segment: ignore stale sense. */
 {.Code=0x21,.Status=2,.Segment=1,.Sense=18,.Capacity=64,.Length=0,.Invalid=TRUE},
 {.Code=0x21,.Status=2,.Result=1,.Segment=20,.Sense=18,.Capacity=64,.SenseCopied=18,.Length=8},
 {.Code=0x21,.Status=2,.Segment=20,.Sense=18,.Capacity=8,.SenseCopied=8,.Length=8},
 {.Code=0x21,.Status=2,.Segment=254,.Sense=252,.Capacity=64,.SenseCopied=18,.Length=8},
 {.Code=0x21,.Residual=3,.Capacity=64,.Length=5}, /* Some devices omit underflow. */
 {.Code=0x21,.Flags=0x40,.Residual=3,.Capacity=64,.Length=0,.Invalid=TRUE},
 {.Code=0x3f,.Status=2,.Segment=20,.Sense=18,.Capacity=64,.Length=0,.Invalid=TRUE},
 {.Code=0x21,.Ocs=4,.Status=2,.Segment=20,.Sense=18,.Capacity=64,.Length=0,.Invalid=TRUE},
 {.Code=0x21,.Status=2,.Flags=0x20,.Segment=20,.Sense=18,.Residual=9,.Capacity=64,.SenseCopied=18,.Length=0,.Invalid=TRUE},
 {.Code=0x21,.Segment=2,.Capacity=64,.Length=8},
 {.Code=0x21,.Status=2,.Segment=20,.Sense=18,.Capacity=0,.Length=8},
 {.Code=0x21,.Status=2,.Ehs=1,.Segment=20,.Sense=18,.Capacity=64,.Length=0,.Invalid=TRUE}
};
static EFI_STATUS EFIAPI Signal(EFI_EVENT e){callbacks++;return EFI_SUCCESS;}
static UINTN mappedSize, descriptorSize;
static UTP_TRD trd;
VOID *lastDescriptor;
VOID EFIAPI DebugPrint(UINTN l, CONST CHAR8 *f, ...) {}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN l){return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *f, UINTN l, CONST CHAR8 *d){abort();}
VOID *EFIAPI AllocateZeroPool(UINTN n){return calloc(1,n);}
VOID EFIAPI FreePool(VOID *p){free(p);}
VOID *EFIAPI AllocateAlignedPages(UINTN pages, UINTN alignment){return calloc(pages,EFI_PAGE_SIZE);}
VOID EFIAPI FreeAlignedPages(VOID *p,UINTN pages){free(p);}
VOID *EFIAPI ZeroMem(VOID *p,UINTN n){return memset(p,0,n);}
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n){return memcpy(d,s,n);}
UINT64 EFIAPI DivU64x32(UINT64 n,UINT32 d){return n/d;}
UINT64 EFIAPI RShiftU64(UINT64 n,UINTN d){return n>>d;}
VOID SwapLittleEndianToBigEndian(UINT8 *p,UINT32 n){for(UINT32 i=0;i<n/2;i++){UINT8 t=p[i];p[i]=p[n-i-1];p[n-i-1]=t;}}
EFI_STATUS UfsAllocateAlignCommonBuffer(UFS_PASS_THRU_PRIVATE_DATA *p,UINTN n,VOID **h,EFI_PHYSICAL_ADDRESS *a,VOID **m){*h=calloc(1,n);assert(*h);descriptorSize=n;lastDescriptor=*h;*a=(UINTN)*h;*m=*h;return EFI_SUCCESS;}
VOID UfsInitCommandUpiu(UTP_COMMAND_UPIU *u,UINT8 lun,UINT8 tag,UINT8 *cdb,UINT8 len,UFS_DATA_DIRECTION dir,UINT32 size){}
static EFI_STATUS EFIAPI Map(EDKII_UFS_HOST_CONTROLLER_PROTOCOL *p,EDKII_UFS_HOST_CONTROLLER_OPERATION op,VOID *h,UINTN *n,EFI_PHYSICAL_ADDRESS *a,VOID **m){mappedSize=*n;*a=0x100000;*m=h;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Unmap(EDKII_UFS_HOST_CONTROLLER_PROTOCOL *p,VOID *m){return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Allocate(EDKII_UFS_HOST_CONTROLLER_PROTOCOL *p,EFI_ALLOCATE_TYPE t,EFI_MEMORY_TYPE m,UINTN pages,VOID **h,UINT64 a){*h=calloc(pages,EFI_PAGE_SIZE);return *h?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;}
static EFI_STATUS EFIAPI Free(EDKII_UFS_HOST_CONTROLLER_PROTOCOL *p,UINTN pages,VOID *h){free(h);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Flush(EDKII_UFS_HOST_CONTROLLER_PROTOCOL *p){return EFI_SUCCESS;}
EFI_STATUS UfsFindAvailableSlotInTrl(UFS_PASS_THRU_PRIVATE_DATA *p,UINT8 *s){*s=0;return EFI_SUCCESS;}
EFI_STATUS UfsStartExecCmd(UFS_PASS_THRU_PRIVATE_DATA *p,UINT8 s){return EFI_SUCCESS;}
EFI_STATUS UfsStopExecCmd(UFS_PASS_THRU_PRIVATE_DATA *p,UINT8 s){return EFI_SUCCESS;}
VOID UfsReleaseSlot(UFS_PASS_THRU_PRIVATE_DATA *p,UINT8 s){}
EFI_STATUS UfsControllerStop(UFS_PASS_THRU_PRIVATE_DATA *p){return EFI_SUCCESS;}
EFI_STATUS UfsReleaseRequestResources(UFS_PASS_THRU_PRIVATE_DATA *p,UFS_PASS_THRU_TRANS_REQ *r,BOOLEAN copy){assert(!r->Submitted||r->DmaFenced);if(copy&&r->ResponseValidated&&r->Packet&&r->AlignedDataBuf){assert(r->Packet->InTransferLength<=r->AlignedDataBufSize);memcpy(r->Packet->InDataBuffer,r->AlignedDataBuf,r->Packet->InTransferLength);}free(r->AlignedDataBuf);r->AlignedDataBuf=NULL;assert(r->CmdDescFromPool);r->CmdDescHost=NULL;return EFI_SUCCESS;}
static VOID Response(UFS_PASS_THRU_TRANS_REQ *r){
 CONST RESPONSE_CASE *c=&Cases[mode];
 UINT8 *wire=(UINT8 *)r->CmdDescHost+trd.RuO*4;
 trd.Ocs=c->Ocs;memset(wire,0,52);
 wire[0]=c->Code;wire[1]=c->Flags;wire[6]=c->Result;wire[7]=c->Status;
 wire[8]=c->Ehs;
 wire[10]=(UINT8)(c->Segment>>8);wire[11]=(UINT8)c->Segment;
 wire[12]=(UINT8)(c->Residual>>24);wire[13]=(UINT8)(c->Residual>>16);
 wire[14]=(UINT8)(c->Residual>>8);wire[15]=(UINT8)c->Residual;
 wire[32]=(UINT8)(c->Sense>>8);wire[33]=(UINT8)c->Sense;
 memset(wire+34,0x5a,18);
}
EFI_STATUS UfsWaitMemSet(UFS_PASS_THRU_PRIVATE_DATA *p,UINT32 o,UINT32 m,UINT32 v,UINT64 t){
 UFS_PASS_THRU_TRANS_REQ r={.CmdDescHost=(VOID *)(UINTN)(((UINT64)trd.UcdBaU<<32)|((UINT64)trd.UcdBa<<7)),.Trd=&trd};
 /* Address fields require 128-byte alignment; use saved allocation below. */
 extern VOID *lastDescriptor;r.CmdDescHost=lastDescriptor;Response(&r);return EFI_SUCCESS;
}
VOID UfsReconcileDataTransferBuffer(UFS_PASS_THRU_PRIVATE_DATA *p,UFS_PASS_THRU_TRANS_REQ *r){if(r->AlignedDataBuf){assert(r->Packet->InTransferLength<=r->AlignedDataBufSize);memcpy(r->Packet->InDataBuffer,r->AlignedDataBuf,r->Packet->InTransferLength);free(r->AlignedDataBuf);}}
EFI_STATUS UfsMmioRead32(UFS_PASS_THRU_PRIVATE_DATA *p,UINT32 o,UINT32 *v){*v=0;return EFI_SUCCESS;}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *e){e->BackLink->ForwardLink=e->ForwardLink;e->ForwardLink->BackLink=e->BackLink;return e->ForwardLink;}
BOOLEAN EFIAPI IsListEmpty(CONST LIST_ENTRY *l){return l->ForwardLink==l;}
LIST_ENTRY *EFIAPI InsertTailList(LIST_ENTRY *l,LIST_ENTRY *e){e->ForwardLink=l;e->BackLink=l->BackLink;l->BackLink->ForwardLink=e;l->BackLink=e;return l;}
#include "UfsScsiBounds.inc"
int main(void){
 EDKII_UFS_HOST_CONTROLLER_PROTOCOL host={.AllocateBuffer=Allocate,.Map=Map,.Unmap=Unmap,.FreeBuffer=Free,.Flush=Flush};
 UFS_PASS_THRU_PRIVATE_DATA p={.UfsHostController=&host,.UtpTrlBase=&trd};
 p.UtpUcdBase=aligned_alloc(128,3072);assert(p.UtpUcdBase);p.Nutrs=1;p.UcdPoolSize=3072;p.UcdDeviceAddress=(UINTN)p.UtpUcdBase;lastDescriptor=p.UtpUcdBase;
 p.Queue.ForwardLink=p.Queue.BackLink=&p.Queue;
 UINT8 *buffer=malloc(524300),sense[64],cdb[16]={0};assert(buffer);
 EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET packet={.DataDirection=EFI_EXT_SCSI_DATA_DIRECTION_READ,.InDataBuffer=buffer+1,.Cdb=cdb,.CdbLength=10};
 UINT32 lengths[]={262144,262143,262145,524288};
 for(UINTN i=0;i<4;i++){
  UFS_PASS_THRU_TRANS_REQ r={.Packet=&packet,.Trd=&trd};packet.InTransferLength=lengths[i];
  assert(UfsCreateScsiCommandDesc(&p,0,&packet,&trd,&r.CmdDescHost,&r.CmdDescMapping)==EFI_SUCCESS);
  assert(UfsPrepareDataTransferBuffer(&p,&r)==EFI_SUCCESS);
  assert(mappedSize==((lengths[i]+3U)&~3U));assert(trd.PrdtL==(mappedSize+UFS_MAX_DATA_LEN_PER_PRD-1)/UFS_MAX_DATA_LEN_PER_PRD);free(r.AlignedDataBuf);
 }
 for(mode=0;mode<ARRAY_SIZE(Cases);mode++){
  CONST RESPONSE_CASE *c=&Cases[mode];
  memset(sense,0xcc,sizeof(sense));
  packet.InTransferLength=8;packet.SenseData=c->Capacity ? sense : NULL;packet.SenseDataLength=c->Capacity;packet.HostAdapterStatus=EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
  assert(UfsExecScsiCmds(&p,0,&packet,NULL)==(c->Invalid ? EFI_DEVICE_ERROR:EFI_SUCCESS));
  assert(packet.InTransferLength==c->Length && packet.TargetStatus==c->Status);
  assert(packet.HostAdapterStatus==(c->Invalid ? EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR:EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK));
  assert(packet.SenseDataLength==c->SenseCopied && sense[c->SenseCopied]==0xcc);
  if(c->SenseCopied) assert(sense[0]==0x5a && sense[c->SenseCopied-1]==0x5a);
  memset(sense,0xcc,sizeof(sense));
  packet.InTransferLength=8;packet.SenseDataLength=c->Capacity;packet.HostAdapterStatus=EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
  assert(UfsExecScsiCmds(&p,0,&packet,(EFI_EVENT)1)==EFI_SUCCESS);
  UFS_PASS_THRU_TRANS_REQ *r=UFS_PASS_THRU_TRANS_REQ_FROM_THIS(p.Queue.ForwardLink);Response(r);
  ProcessAsyncTaskList(NULL,&p);
  assert(IsListEmpty(&p.Queue));
  assert(packet.InTransferLength==c->Length && packet.TargetStatus==c->Status);
  assert(packet.HostAdapterStatus==(c->Invalid ? EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR:EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK));
  assert(packet.SenseDataLength==c->SenseCopied && sense[c->SenseCopied]==0xcc);
  if(c->SenseCopied) assert(sense[0]==0x5a && sense[c->SenseCopied-1]==0x5a);
 }
 UINT32 aligned;assert(UfsAlignedTransferLength(MAX_UINT32,&aligned)==EFI_BAD_BUFFER_SIZE);
 // Verify all 128 PRDs fit the fixed slot and reject an oversized request
 // before accessing the data buffer or changing the descriptor.
 packet.InTransferLength=33554432;
 VOID *h,*m;
 assert(UfsCreateScsiCommandDesc(&p,0,&packet,&trd,&h,&m)==EFI_SUCCESS);
 assert(h==p.UtpUcdBase && m==NULL && trd.PrdtL==128 && trd.PrdtO*4+trd.PrdtL*16==3072);
 for(unsigned direction=0;direction<2;direction++) {
   packet.DataDirection=direction ? EFI_EXT_SCSI_DATA_DIRECTION_WRITE : EFI_EXT_SCSI_DATA_DIRECTION_READ;
   packet.InTransferLength=packet.OutTransferLength=33554433;
   assert(UfsCreateScsiCommandDesc(&p,0,&packet,&trd,&h,&m)==EFI_BAD_BUFFER_SIZE);
   assert((direction ? packet.OutTransferLength : packet.InTransferLength)==33554432);
 }
 assert(callbacks==ARRAY_SIZE(Cases));free(p.UtpUcdBase);free(buffer);puts("UFS actual sync/async response, sense truncation and bounce PRDT boundaries PASS");return 0;
}
