/** Compile the unchanged MU Start function; mock lower-level command services. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#undef NULL
#include "UfsPassThru.h"
UFS_PASS_THRU_PRIVATE_DATA gUfsPassThruTemplate;
EDKII_UFS_HC_PLATFORM_PROTOCOL *mUfsHcPlatform;
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEdkiiUfsHostControllerProtocolGuid, gEdkiiUfsHcPlatformProtocolGuid;
EFI_GUID gEfiUfsDeviceConfigProtocolGuid, gEfiExtScsiPassThruProtocolGuid;
static unsigned allocations, frees, unmapped, buffers_freed, closed, postlinks, queries, writes;
static unsigned phase;
static BOOLEAN fail_info, fail_read, fail_write, fail_callback, fail_stop;
static unsigned stopped, signaled, timer_cancels, timer_closes, opens;
static BOOLEAN fail_uninstall, fail_unmap, fail_cmd_free, fail_signal, fail_ucd_unmap, fail_ucd_free;
static VOID *fail_request_map;
static unsigned data_unmaps, cmd_unmaps, cmd_frees, bounce_frees;
static UFS_PASS_THRU_PRIVATE_DATA *active;
static BOOLEAN fenced;
static unsigned descriptor_queries, config_installs, config_removals;
static EDKII_UFS_HOST_CONTROLLER_PROTOCOL host;
EFI_STATUS UfsFinishDeviceInitialization (UFS_PASS_THRU_PRIVATE_DATA *Private);
#include "UfsOwnerStart.inc"
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return FALSE; }
VOID EFIAPI DebugAssert (CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description) { abort(); }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID *EFIAPI AllocateCopyPool (UINTN Size, CONST VOID *Source) {
 VOID *P=malloc(Size); assert(P); memcpy(P,Source,Size); allocations++; return P;
}
VOID EFIAPI FreePool (VOID *P) { frees++; free(P); }
LIST_ENTRY *EFIAPI InitializeListHead (LIST_ENTRY *List) { List->ForwardLink=List->BackLink=List; return List; }
VOID UfsReleaseSlot (UFS_PASS_THRU_PRIVATE_DATA *P, UINT8 Slot) { P->ReservedSlots &= ~(1U << Slot); }
EFI_STATUS GetUfsHcInfo (UFS_PASS_THRU_PRIVATE_DATA *P) { assert(phase++==0); return fail_info ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
EFI_STATUS UfsControllerInit (UFS_PASS_THRU_PRIVATE_DATA *P) {
 assert(phase++==1); fenced=FALSE; P->TmrlMapping=(VOID *)1; P->TrlMapping=(VOID *)2;
 P->UcdMapping=(VOID *)10; P->UtpUcdBase=(VOID *)11; P->UcdPoolSize=64*3072;
 P->UtpTmrlBase=(VOID *)3; P->UtpTrlBase=(VOID *)4; P->Nutmrs=1; P->Nutrs=64;
 return EFI_SUCCESS;
}
EFI_STATUS UfsControllerStop (UFS_PASS_THRU_PRIVATE_DATA *P) { if(fail_stop) return EFI_TIMEOUT; stopped++; fenced=TRUE; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI signal (EFI_EVENT Event) {
 UFS_PASS_THRU_PRIVATE_DATA *P=active;
 EFI_DRIVER_BINDING_PROTOCOL Binding={0};
 assert(fenced && P && P->CleanupInProgress && UfsFindRetained(P->Handle)==P);
 assert(UfsPassThruDriverBindingStart(&Binding,P->Handle,NULL)==EFI_NOT_READY);
 assert(UfsPassThruDriverBindingStop(&Binding,P->Handle,0,NULL)==EFI_NOT_READY);
 assert(P->CleanupInProgress && UfsFindRetained(P->Handle)==P);
 if(fail_signal) return EFI_DEVICE_ERROR;
 signaled++; return EFI_SUCCESS;
}
VOID *EFIAPI CopyMem (VOID *P, CONST VOID *Q, UINTN Size) { return memcpy(P,Q,Size); }
VOID *EFIAPI ZeroMem (VOID *P, UINTN Size) { return memset(P,0,Size); }
VOID EFIAPI FreeAlignedPages (VOID *P, UINTN Pages) { assert(((UINT8 *)P)[0]==0); bounce_frees++; free(P); }
LIST_ENTRY *EFIAPI RemoveEntryList (CONST LIST_ENTRY *Entry) {
 Entry->BackLink->ForwardLink=Entry->ForwardLink;
 Entry->ForwardLink->BackLink=Entry->BackLink;
 return Entry->ForwardLink;
}
EFI_STATUS UfsExecNopCmds (UFS_PASS_THRU_PRIVATE_DATA *P) { assert(phase++==2); return EFI_SUCCESS; }
EFI_STATUS UfsFinishDeviceInitialization (UFS_PASS_THRU_PRIVATE_DATA *P) { assert(phase++==3); return EFI_SUCCESS; }
EFI_STATUS EFIAPI UfsHcDriverInterfaceExecUicCommand (EDKII_UFS_HC_DRIVER_INTERFACE *I, EDKII_UIC_COMMAND *C) { return EFI_SUCCESS; }
EFI_STATUS UfsRwAttributes (UFS_PASS_THRU_PRIVATE_DATA *P, BOOLEAN Read,
 UINT8 Attr, UINT8 Index, UINT8 Selector, UINT32 *Value) {
 assert(Attr==UfsAttrRefClkFreq);
 if(Read) { queries++; assert(phase++==4); *Value=EdkiiUfsCardRefClkFreq26Mhz; return fail_read ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
 writes++; assert(phase++==5 && *Value==EdkiiUfsCardRefClkFreq19p2Mhz);
 return fail_write ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
EFI_STATUS UfsRwDeviceDesc (UFS_PASS_THRU_PRIVATE_DATA *P, BOOLEAN Read,
 UINT8 Id, UINT8 Index, UINT8 Selector, VOID *Desc, UINT32 *Size) {
 assert(phase==7 && Read && Id==UfsUnitDesc && Index==0);
 descriptor_queries++; return EFI_TIMEOUT;
}
VOID EFIAPI ProcessAsyncTaskList (EFI_EVENT Event, VOID *Context) { abort(); }
static EFI_STATUS EFIAPI callback (EFI_HANDLE Handle, EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase, VOID *Data) {
 EDKII_UFS_HC_DRIVER_INTERFACE *I=Data;
 assert(Phase==EdkiiUfsHcPostLinkStartup && phase++==6);
 assert(I->UfsHcProtocol==&host && I->UfsExecUicCommand==UfsHcDriverInterfaceExecUicCommand);
 postlinks++; return fail_callback ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI getbar (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, UINTN *Bar) { *Bar=0x1d84000; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI unmap (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, VOID *Map) {
 assert(fenced);
 if(Map==(VOID *)7 || Map==(VOID *)8) {
   if(Map==fail_request_map) return EFI_DEVICE_ERROR;
   if(Map==(VOID *)7) data_unmaps++; else cmd_unmaps++;
   return EFI_SUCCESS;
 }
 assert(Map==(VOID *)1 || Map==(VOID *)2 || Map==(VOID *)10);
 if(Map==(VOID *)10 && fail_ucd_unmap) return EFI_DEVICE_ERROR;
 if(fail_unmap) return EFI_DEVICE_ERROR;
 unmapped++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI freebuffer (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, UINTN Pages, VOID *Buffer) {
 assert(fenced);
 if(Buffer==(VOID *)6) {
   assert(Pages==2);
   if(fail_cmd_free) return EFI_DEVICE_ERROR;
   cmd_frees++; return EFI_SUCCESS;
 }
 if(Buffer==(VOID *)11) { assert(Pages==48); if(fail_ucd_free) return EFI_DEVICE_ERROR; buffers_freed++; return EFI_SUCCESS; }
 if(Buffer!=(VOID *)3 && Buffer!=(VOID *)4) { assert(((UINT8 *)Buffer)[0]==0); bounce_frees++; free(Buffer); return EFI_SUCCESS; }
 assert(Buffer==(VOID *)3 || Buffer==(VOID *)4);
 if(Buffer==(VOID *)3) assert(Pages==EFI_SIZE_TO_PAGES(sizeof(UTP_TMRD)));
 else { assert(EFI_SIZE_TO_PAGES(64*sizeof(UTP_TRD))!=EFI_SIZE_TO_PAGES(64*sizeof(UTP_TMRD)));
        assert(Pages==EFI_SIZE_TO_PAGES(64*sizeof(UTP_TRD))); }
 buffers_freed++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI openprotocol (EFI_HANDLE H, EFI_GUID *Guid, VOID **Interface, EFI_HANDLE Agent, EFI_HANDLE Controller, UINT32 Attr) {
 opens++;
 if(Attr==EFI_OPEN_PROTOCOL_GET_PROTOCOL) { assert(active); *Interface=&active->ExtScsiPassThru; }
 else *Interface=&host;
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI install (EFI_HANDLE *H, ...) { config_installs++; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI uninstall (EFI_HANDLE H, ...) { config_removals++; return fail_uninstall ? EFI_ACCESS_DENIED : EFI_SUCCESS; }
static EFI_STATUS EFIAPI settimer (EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Trigger) { assert(Type==TimerCancel); timer_cancels++; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI closeevent (EFI_EVENT Event) { timer_closes++; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI closeprotocol (EFI_HANDLE H, EFI_GUID *Guid, EFI_HANDLE Agent, EFI_HANDLE Controller) { closed++; return EFI_SUCCESS; }
int main (void) {
 EFI_BOOT_SERVICES Bs={.OpenProtocol=openprotocol,.CloseProtocol=closeprotocol,
  .UninstallMultipleProtocolInterfaces=uninstall,.SetTimer=settimer,.CloseEvent=closeevent,.SignalEvent=signal};
 EFI_DRIVER_BINDING_PROTOCOL Binding={0};
 EDKII_UFS_HC_PLATFORM_PROTOCOL Platform={.Version=3,.Callback=callback,.RefClkFreq=EdkiiUfsCardRefClkFreq19p2Mhz};
 unsigned Case;
 host.GetUfsHcMmioBar=getbar; host.Unmap=unmap; host.FreeBuffer=freebuffer;
 gBS=&Bs; mUfsHcPlatform=&Platform;
 for(Case=0;Case<3;Case++) {
   phase=0; fail_read=Case==0; fail_write=Case==1; fail_callback=Case==2;
   assert(UfsPassThruDriverBindingStart(&Binding,(VOID *)1,NULL)==EFI_DEVICE_ERROR);
   assert(allocations==Case+1 && frees==Case+1 && closed==Case+1);
   assert(unmapped==3*(Case+1) && buffers_freed==3*(Case+1));
   assert(postlinks==(Case==2 ? 1U : 0U));
 }
 assert(queries==3 && writes==2);
 /* GetUfsHcInfo may fail after GetUfsHcMmioBar has prepared the host.  The
    attempted flag must still drive the controller stop cleanup. */
 {
   unsigned OldAllocations=allocations, OldFrees=frees, OldClosed=closed;
   unsigned OldMaps=unmapped, OldBuffers=buffers_freed, OldStops=stopped;
   phase=0; fail_info=TRUE;
   assert(UfsPassThruDriverBindingStart(&Binding,(VOID *)4,NULL)==EFI_DEVICE_ERROR);
   assert(stopped==OldStops+1 && allocations==OldAllocations+1 && frees==OldFrees+1);
   assert(closed==OldClosed+1 && unmapped==OldMaps && buffers_freed==OldBuffers);
   fail_info=FALSE;
 }
 /* Independent controllers retain private objects and DMA pages on failed fences. */
 {
   unsigned OldFree=frees, OldMaps=unmapped, OldClosed=closed, OldOpens;
   UFS_PASS_THRU_PRIVATE_DATA *First;
   fail_stop=TRUE; phase=0;
   assert(UfsPassThruDriverBindingStart(&Binding,(VOID *)1,NULL)==EFI_TIMEOUT);
   First=UfsFindRetained((VOID *)1); assert(First && First->TmrlMapping);
   phase=0;
   assert(UfsPassThruDriverBindingStart(&Binding,(VOID *)2,NULL)==EFI_TIMEOUT);
   assert(UfsFindRetained((VOID *)1)==First && UfsFindRetained((VOID *)2)!=NULL);
   assert(frees==OldFree && unmapped==OldMaps && closed==OldClosed);
   OldOpens=opens;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)1,0,NULL)==EFI_TIMEOUT);
   assert(opens==OldOpens && UfsFindRetained((VOID *)1)==First);
   fail_stop=FALSE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)1,0,NULL)==EFI_SUCCESS);
   assert(!UfsFindRetained((VOID *)1) && UfsFindRetained((VOID *)2));
   /* Start retry cleans its old owner before acquiring a new one. */
   phase=0;
   assert(UfsPassThruDriverBindingStart(&Binding,(VOID *)2,NULL)==EFI_DEVICE_ERROR);
   assert(!mFailedStart && frees==OldFree+3 && closed==OldClosed+3);
 }
 /* Stop refuses consumers before touching live queue/timer or hardware. */
 {
   EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet={0};
   UFS_PASS_THRU_TRANS_REQ *Request=calloc(1,sizeof(*Request));
   unsigned OldStops=stopped, OldFree=frees;
   assert(Request);
   active=AllocateCopyPool(sizeof(*active),&gUfsPassThruTemplate);
   active->Signature=UFS_PASS_THRU_SIG; active->Handle=(VOID *)3;
   active->UfsHostController=&host; active->TimerEvent=(VOID *)5;
   InitializeListHead(&active->Queue);
   Request->Signature=UFS_PASS_THRU_TRANS_REQ_SIG; Request->Packet=&Packet; Request->CallerEvent=(VOID *)9;
   Request->DataBufMapping=(VOID *)7; Request->CmdDescMapping=(VOID *)8;
   Request->CmdDescHost=(VOID *)6; Request->CmdDescSize=5000;
   Request->AlignedDataBuf=malloc(16); assert(Request->AlignedDataBuf);
   memset(Request->AlignedDataBuf,0xaa,16); Request->AlignedDataBufSize=16;
   Request->TransferList.ForwardLink=Request->TransferList.BackLink=&active->Queue;
   active->Queue.ForwardLink=active->Queue.BackLink=&Request->TransferList;
   fenced=FALSE; fail_uninstall=TRUE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)3,0,NULL)==EFI_ACCESS_DENIED);
   assert(stopped==OldStops && signaled==0 && timer_cancels==0 && frees==OldFree);
   fail_uninstall=FALSE; fail_stop=TRUE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)3,0,NULL)==EFI_TIMEOUT);
   assert(signaled==0 && timer_cancels==1 && timer_closes==1 && frees==OldFree);
   assert(UfsFindRetained((VOID *)3)==active);
   fail_stop=FALSE; fail_request_map=(VOID *)7;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)3,0,NULL)==EFI_DEVICE_ERROR);
   assert(data_unmaps==0 && cmd_unmaps==0 && signaled==0 && frees==OldFree);
   assert(active->Queue.ForwardLink==&Request->TransferList && !active->CleanupInProgress);
   fail_request_map=(VOID *)8;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)3,0,NULL)==EFI_DEVICE_ERROR);
   assert(data_unmaps==1 && !Request->DataBufMapping && Request->CmdDescMapping==(VOID *)8);
   fail_request_map=NULL; fail_cmd_free=TRUE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)3,0,NULL)==EFI_DEVICE_ERROR);
   assert(cmd_unmaps==1 && !Request->CmdDescMapping && cmd_frees==0 && bounce_frees==0);
   fail_cmd_free=FALSE; fail_signal=TRUE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)3,0,NULL)==EFI_DEVICE_ERROR);
   assert(cmd_frees==1 && bounce_frees==1 && signaled==0 && frees==OldFree);
   assert(!Request->CmdDescHost && !Request->AlignedDataBuf && !active->CleanupInProgress);
   fail_signal=FALSE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)3,0,NULL)==EFI_SUCCESS);
   assert(data_unmaps==1 && cmd_unmaps==1 && cmd_frees==1 && bounce_frees==1);
   assert(signaled==1 && frees==OldFree+2 && !mFailedStart);
  active=NULL;
 }
 {
   unsigned OldFree=frees, OldClosed=closed, OldUnmap=unmapped, OldBuffers=buffers_freed;
   unsigned OldRemove=config_removals;
   phase=0; fail_info=fail_read=fail_write=fail_callback=FALSE;
   Bs.InstallMultipleProtocolInterfaces=install;
   assert(UfsPassThruDriverBindingStart(&Binding,(VOID *)9,NULL)==EFI_TIMEOUT);
   assert(descriptor_queries==1 && config_installs==1 && config_removals==OldRemove+1);
   assert(fenced && unmapped==OldUnmap+3 && buffers_freed==OldBuffers+3);
   assert(frees==OldFree+1 && closed==OldClosed+1 && !mFailedStart);
 }
 // Stop or UCD revocation/free failure must retain the controller pool.
 {
   unsigned OldFrees=frees, OldMaps=unmapped, OldBuffers=buffers_freed;
   phase=0; fail_stop=TRUE;
   assert(UfsPassThruDriverBindingStart(&Binding,(VOID *)12,NULL)==EFI_TIMEOUT);
   active=UfsFindRetained((VOID *)12); assert(active && active->UcdMapping==(VOID *)10 && active->UtpUcdBase==(VOID *)11);
   assert(frees==OldFrees && unmapped==OldMaps && buffers_freed==OldBuffers);
   fail_stop=FALSE; fail_ucd_unmap=TRUE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)12,0,NULL)==EFI_DEVICE_ERROR);
   assert(active->UcdMapping==(VOID *)10 && active->UtpUcdBase==(VOID *)11);
   assert(!active->TmrlMapping && !active->TrlMapping && frees==OldFrees);
   fail_ucd_unmap=FALSE; fail_ucd_free=TRUE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)12,0,NULL)==EFI_DEVICE_ERROR);
   assert(!active->UcdMapping && active->UtpUcdBase==(VOID *)11 && active->UcdPoolSize==64*3072 && frees==OldFrees);
   fail_ucd_free=FALSE;
   assert(UfsPassThruDriverBindingStop(&Binding,(VOID *)12,0,NULL)==EFI_SUCCESS);
   assert(!UfsFindRetained((VOID *)12) && frees==OldFrees+1 && unmapped==OldMaps+3 && buffers_freed==OldBuffers+3);
   active=NULL;
 }
 puts("MU UFS owner lifecycle: refclk read/write and PostLink errors abort before enumeration; fenced cleanup, per-controller retention, Start/Stop retry and queue lifetime");
 return 0;
}
