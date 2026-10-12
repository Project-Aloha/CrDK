/** Actual SCSI submit, completion, timeout and staging ownership paths. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "UfsPassThruHci.c"
#include "UfsRequestResources.inc"
EFI_BOOT_SERVICES *gBS;
EDKII_UFS_HC_PLATFORM_PROTOCOL *mUfsHcPlatform;
static UFS_PASS_THRU_PRIVATE_DATA Owner;
static UINT32 Doorbell, Hce=1;
static UINT32 InterruptStatus, FatalOnSubmit;
static BOOLEAN FatalDoorbellCleared;
static unsigned DelayUs, FatalSubmissions, FatalStops, FatalUnmapWithoutStop;
static EFI_TPL Tpl=TPL_APPLICATION;
static BOOLEAN FailUnmap, Hang, FailFence, FailSignal, StopDuringMap, BadOcs, FailAllUnmap, ShortCommon;
static UINT64 FailWriteOffset;
static BOOLEAN DeferCompletion, MissingWriteback;
static unsigned BadResponseIdentity;
static unsigned TagSlotMismatches, DuplicateTags;
static UINT8 ResponseCode=0x21;
static UINT8 ResponseFlags;
static UINT32 ResponseResidual;
static BOOLEAN BadManagementResponse;
static BOOLEAN InvalidQueryIndex;
static unsigned DescriptorMode;
static unsigned WireReads, WireWrites;
static unsigned Mappings, Buffers, Signals, Pools;
static unsigned QuerySubmissions, DiagnosticRegisters;
typedef struct { VOID *Host; UINTN Bytes; BOOLEAN Read; } MAP;
static MAP *DataMap;
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) {}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return FALSE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID EFIAPI DebugAssert (CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description) { fprintf(stderr,"%s:%lu %s\n",File,(unsigned long)Line,Description); abort(); }
VOID *EFIAPI ZeroMem (VOID *P, UINTN N) { return memset(P,0,N); }
VOID *EFIAPI CopyMem (VOID *P, CONST VOID *Q, UINTN N) { return memcpy(P,Q,N); }
VOID *EFIAPI AllocateZeroPool (UINTN N) { Pools++; return calloc(1,N); }
VOID EFIAPI FreePool (VOID *P) { assert(Pools); Pools--; free(P); }
UINT64 EFIAPI MultU64x32 (UINT64 A, UINT32 B) { return A*B; }
UINT64 EFIAPI DivU64x32 (UINT64 A, UINT32 B) { return A/B; }
UINT64 EFIAPI RShiftU64 (UINT64 A, UINTN B) { return A>>B; }
UINTN EFIAPI MicroSecondDelay (UINTN N) { DelayUs+=(unsigned)N; return N; }
LIST_ENTRY *EFIAPI InitializeListHead (LIST_ENTRY *L) { L->ForwardLink=L->BackLink=L; return L; }
LIST_ENTRY *EFIAPI InsertTailList (LIST_ENTRY *H, LIST_ENTRY *E) { E->BackLink=H->BackLink; E->ForwardLink=H; H->BackLink->ForwardLink=E; H->BackLink=E; return H; }
LIST_ENTRY *EFIAPI RemoveEntryList (CONST LIST_ENTRY *E) { E->BackLink->ForwardLink=E->ForwardLink; E->ForwardLink->BackLink=E->BackLink; return E->ForwardLink; }
static EFI_TPL EFIAPI Raise (EFI_TPL T) { EFI_TPL Old=Tpl; assert(T>=Old); Tpl=T; return Old; }
static VOID EFIAPI Restore (EFI_TPL T) { assert(T<=Tpl); Tpl=T; }
static EFI_STATUS EFIAPI Signal (EFI_EVENT E) {
 EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Reentrant={0};
 assert(Tpl==TPL_NOTIFY && Owner.CleanupInProgress);
 assert(UfsExecScsiCmds(&Owner,0,&Reentrant,NULL)==EFI_NOT_READY);
 if(FailSignal) return EFI_DEVICE_ERROR;
 Signals++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Allocate (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, EFI_ALLOCATE_TYPE T, EFI_MEMORY_TYPE M, UINTN P, VOID **A, UINT64 Attr) {
 *A=aligned_alloc(EFI_PAGE_SIZE,P*EFI_PAGE_SIZE); assert(*A); memset(*A,0,P*EFI_PAGE_SIZE); Buffers++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, UINTN Pages, VOID *P) { assert(Buffers); Buffers--; free(P); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Map (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, EDKII_UFS_HOST_CONTROLLER_OPERATION Op, VOID *P, UINTN *N, EFI_PHYSICAL_ADDRESS *D, VOID **Token) {
 MAP *M=malloc(sizeof(*M)); assert(M); M->Host=P; M->Bytes=*N; M->Read=Op==EdkiiUfsHcOperationBusMasterWrite;
 if(Op!=EdkiiUfsHcOperationBusMasterCommonBuffer) { DataMap=M; if(StopDuringMap) Owner.IoUnavailable=TRUE; }
 *D=(UINTN)P; *Token=M; Mappings++; if(ShortCommon && Op==EdkiiUfsHcOperationBusMasterCommonBuffer) *N/=2; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, VOID *Token) {
 MAP *M=Token;
 if((InterruptStatus & 0x71880U) && Hce) FatalUnmapWithoutStop++;
 if(FailAllUnmap || (M==DataMap && FailUnmap)) return EFI_DEVICE_ERROR;
 if(M->Read) memset(M->Host,0x5a,M->Bytes); // delayed provider bounce copyback
 if(M==DataMap) DataMap=NULL;
 Mappings--; free(M); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH W, UINT64 O, UINTN N, VOID *P) {
 UINT32 V=0;
 if(O>=UFS_HC_UECPA_OFFSET && O<=UFS_HC_UECDME_OFFSET) {
   if(Hang) { assert(Doorbell); DiagnosticRegisters++; }
 }
 if(O==UFS_HC_IS_OFFSET) V=InterruptStatus;
 else if(O==UFS_HC_UTRLDBR_OFFSET) V=Doorbell;
 else if(O==UFS_HC_ENABLE_OFFSET) V=Hce;
 else if(O==UFS_HC_UTRLRSR_OFFSET) V=1;
 *(UINT32 *)P=V; return EFI_SUCCESS;
}
static void Complete (UINT32 V) {
 if(MissingWriteback) { Doorbell&=~V; return; }
     for(UINTN I=0;I<32;I++) if(V&(1U<<I)) {
       UTP_TRD *T=(UTP_TRD *)Owner.UtpTrlBase+I;
       UINT8 *Cmd=(VOID *)(UINTN)(((UINT64)T->UcdBaU<<32)|((UINT64)T->UcdBa<<7));
       UTP_RESPONSE_UPIU *Rsp=(VOID *)(Cmd+T->RuO*4);
       Rsp->Lun=Cmd[2]^(BadResponseIdentity==2);
       Rsp->TaskTag=Cmd[3]^(BadResponseIdentity==1);
       T->Ocs=BadOcs?1:0;
       if(Cmd[0]==1) {
         /* Decode wire bytes independently of the production bitfields. */
         UINT32 Length=((UINT32)Cmd[12]<<24)|((UINT32)Cmd[13]<<16)|((UINT32)Cmd[14]<<8)|Cmd[15];
         UINT32 *Prd=(VOID *)(Cmd+T->PrdtO*4);
         UINTN Total=0;
         assert(DataMap && Length<=DataMap->Bytes && DataMap->Bytes-Length<4);
         assert(T->Dd==(DataMap->Read ? 2 : 1));
         assert((Cmd[1]&0x60)==(DataMap->Read ? 0x40 : 0x20));
         if(Cmd[16]==0x12) {
           assert(DataMap->Read && Cmd[20]==Length);
         } else {
           UINT32 Blocks=((UINT32)Cmd[23]<<8)|Cmd[24];
           assert(Cmd[16]==(DataMap->Read ? 0x28 : 0x2a));
           assert(Blocks==513 && Blocks*512==Length);
         }
         for(UINTN J=0;J<T->PrdtL;J++) {
           UINT64 Address=((UINT64)Prd[J*4+1]<<32)|Prd[J*4];
           UINT32 Bytes=Prd[J*4+3]+1;
           assert(Address==(UINTN)DataMap->Host+Total);
           assert(Prd[J*4+2]==0 && Bytes<=262144 && !(Bytes&3));
           Total+=Bytes;
         }
         assert(Total==DataMap->Bytes);
         if(DataMap->Read) WireReads++;
         else { for(UINTN J=0;J<Length;J++) assert(((UINT8 *)DataMap->Host)[J]==0xa6); WireWrites++; }
         Rsp->TransCode=ResponseCode;
         /* The response flags have different meanings from COMMAND flags:
            byte 1 bit5=underflow, bit6=overflow (public Linux ufs.h).
            Construct residual bytes independently of native UPIU structs. */
         UINT8 *Wire=(UINT8 *)Rsp;
         Wire[1]=ResponseFlags;
         Wire[12]=(UINT8)(ResponseResidual>>24);
         Wire[13]=(UINT8)(ResponseResidual>>16);
         Wire[14]=(UINT8)(ResponseResidual>>8);
         Wire[15]=(UINT8)ResponseResidual;
       } else if(Cmd[0]==0x16) {
         UTP_QUERY_RESP_UPIU *Q=(VOID *)Rsp;
         /* UFSHCI DW0[26:25] is zero for every management command,
            including descriptors whose data segment lives in the UPIU. */
         assert((((UINT8 *)T)[3]&0x06)==0);
         assert(T->PrdtL==0 && T->PrdtO==256 && Cmd[1]==0 && Cmd[2]==0);
         if(DescriptorMode==3) {
           assert(Cmd[5]==0x81 && Cmd[12]==UtpQueryFuncOpcodeWrDesc && Cmd[13]==UfsConfigDesc);
           assert(Cmd[14]==0 && Cmd[15]==0 && Cmd[10]==0 && Cmd[11]==7);
           assert(Cmd[18]==0 && Cmd[19]==7 && T->RuO*4==512 && T->RuL*4==512);
           for(UINTN J=0;J<7;J++) assert(Cmd[32+J]==(UINT8)(0x70+J));
           ((UINT8 *)Q)[18]=0; ((UINT8 *)Q)[19]=7;
         } else if(DescriptorMode) {
           assert(Cmd[12]==UtpQueryFuncOpcodeRdDesc && Cmd[13]==UfsUnitDesc);
           assert(Cmd[5]==1 && Cmd[10]==0 && Cmd[11]==0 && Cmd[14]==0 && Cmd[15]==0);
           assert(Cmd[18]==0 && Cmd[19]==8 && T->RuO*4==512 && T->RuL*4==512);
           UINT8 *Wire=(VOID *)Q; UINT8 *Payload=(VOID *)(Q+1);
           Wire[10]=0; Wire[11]=DescriptorMode==1 ? 4 : 9;
           Wire[18]=0; Wire[19]=8; /* OSF length differs from received bytes. */
           Payload[0]=4; Payload[1]=UfsUnitDesc; Payload[2]=0; Payload[3]=1;
         } else assert(Cmd[12]==UtpQueryFuncOpcodeRdFlag && Cmd[13]==UfsFlagDevInit);
         Q->TransCode=BadManagementResponse ? 0x3f : 0x36;
         Q->QueryResp=(BadManagementResponse || InvalidQueryIndex) ? UfsUtpQueryResponseInvalidSelector : 0;
         ((UINT8 *)&Q->Tsf.Value)[3]=1;
       } else { assert(Cmd[0]==0); Rsp->TransCode=BadManagementResponse ? 0 : 0x20; }
     }
     Doorbell&=~V;
}
static EFI_STATUS EFIAPI Write (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH W, UINT64 O, UINTN N, VOID *P) {
 UINT32 V=*(UINT32 *)P;
 if(FailWriteOffset && O==FailWriteOffset) return EFI_DEVICE_ERROR;
 if(O==UFS_HC_UTRLDBR_OFFSET) {
   Doorbell|=V;
   for(UINTN I=0;I<32;I++) if(V&(1U<<I)) {
     UTP_TRD *T=(UTP_TRD *)Owner.UtpTrlBase+I;
     UINT8 *Cmd=(VOID *)(UINTN)(((UINT64)T->UcdBaU<<32)|((UINT64)T->UcdBa<<7));
     if(Cmd[3]!=I) TagSlotMismatches++;
     for(UINTN J=0;J<32;J++) if(J!=I && (Doorbell & (1U<<J))) {
       UTP_TRD *Other=(UTP_TRD *)Owner.UtpTrlBase+J;
       UINT8 *OtherCmd=(VOID *)(UINTN)(((UINT64)Other->UcdBaU<<32)|((UINT64)Other->UcdBa<<7));
       if(Cmd[3]==OtherCmd[3]) DuplicateTags++;
     }
     if(Cmd[0]==0x16) QuerySubmissions++;
   }
   if(FatalOnSubmit) {
     InterruptStatus=FatalOnSubmit; FatalSubmissions++;
     if(FatalDoorbellCleared) Complete(V);
   } else if(!Hang && !DeferCompletion) Complete(V);
 } else if(O==UFS_HC_UTRLCLR_OFFSET) { if(!FailFence) Doorbell&=V; }
 else if(O==UFS_HC_ENABLE_OFFSET && !FailFence) { Hce=V; if(!V) { if(InterruptStatus & 0x71880U) FatalStops++; Doorbell=0; } }
 return EFI_SUCCESS;
}
static void Clean (void) { assert(!Mappings && !Buffers && !Pools && !Owner.ReservedSlots && Owner.Queue.ForwardLink==&Owner.Queue); }
static void Reset (void) { Clean(); EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H=Owner.UfsHostController; VOID *Trl=Owner.UtpTrlBase, *Ucd=Owner.UtpUcdBase; ZeroMem(&Owner,sizeof(Owner)); Owner.UfsHostController=H; Owner.UtpTrlBase=Trl; Owner.UtpUcdBase=Ucd; Owner.Nutrs=32; Owner.UcdPoolSize=98304; Owner.UcdDeviceAddress=(UINTN)Ucd; Owner.UfsHcInfo.Capabilities=31|UFS_HC_CAP_64ADDR; InitializeListHead(&Owner.Queue); InterruptStatus=FatalOnSubmit=0; FatalDoorbellCleared=FALSE; Hce=1; Hang=FailFence=FailUnmap=FailSignal=StopDuringMap=FALSE; }
int main (void) {
 EFI_BOOT_SERVICES Bs={.RaiseTPL=Raise,.RestoreTPL=Restore,.SignalEvent=Signal};
 EDKII_UFS_HOST_CONTROLLER_PROTOCOL H={.Read=Read,.Write=Write,.Map=Map,.Unmap=Unmap,.AllocateBuffer=Allocate,.FreeBuffer=Free};
 UTP_TRD Trd[32]={0}; UINT8 Cdb[16]={0x12,0,0,0,16}, Data[16]; unsigned Before;
 EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET P={.Cdb=Cdb,.CdbLength=6,.DataDirection=EFI_EXT_SCSI_DATA_DIRECTION_READ,.InDataBuffer=Data,.InTransferLength=16,.Timeout=1000};
 gBS=&Bs; Owner.UtpUcdBase=aligned_alloc(4096,98304); assert(Owner.UtpUcdBase); Owner.Nutrs=32; Owner.UcdPoolSize=98304; Owner.UcdDeviceAddress=(UINTN)Owner.UtpUcdBase; Owner.UfsHostController=&H; Owner.UtpTrlBase=Trd; Owner.UfsHcInfo.Capabilities=31|UFS_HC_CAP_64ADDR; InitializeListHead(&Owner.Queue);
 memset(Data,0xcc,sizeof(Data)); P.HostAdapterStatus=0xff;
 assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_SUCCESS);
 assert(P.HostAdapterStatus==EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK && Data[0]==0x5a); Clean();
 /* Actual serializer, response validator, unmap and caller copyback. The
    literal 96-byte fixtures avoid assuming that logged "60" is decimal.
    Removing overflow rejection or copying requested instead of actual bytes
    must fail here for both synchronous and asynchronous requests. */
 {
   static const struct { UINT8 Flags; UINT32 Residual, Actual; BOOLEAN Invalid; } Cases[]={
     {0x00,0,96,FALSE}, {0x20,4,92,FALSE}, {0x00,4,92,FALSE},
     {0x20,96,0,FALSE}, {0x20,0,96,FALSE},
     {0x40,4,0,TRUE}, {0x60,4,0,TRUE}, {0x40,0,0,TRUE},
     {0x20,97,0,TRUE}, {0x00,0xffffffffU,0,TRUE}
   };
   UINT8 Inquiry[98];
   P.InDataBuffer=Inquiry+1; Cdb[4]=96;
   for(unsigned Async=0;Async<2;Async++) for(UINTN I=0;I<ARRAY_SIZE(Cases);I++) {
     ResponseFlags=Cases[I].Flags; ResponseResidual=Cases[I].Residual;
     P.InTransferLength=96; memset(Inquiry,0xcc,sizeof(Inquiry)); Before=Signals;
     assert(UfsExecScsiCmds(&Owner,0,&P,Async ? (VOID *)1 : NULL)==
       (Async || !Cases[I].Invalid ? EFI_SUCCESS : EFI_DEVICE_ERROR));
     if(Async) ProcessAsyncTaskList(NULL,&Owner);
     assert(P.HostAdapterStatus==(Cases[I].Invalid ?
       EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR : EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK));
     assert(P.InTransferLength==Cases[I].Actual && P.TargetStatus==0);
     assert(Signals==Before+Async && Inquiry[0]==0xcc && Inquiry[97]==0xcc);
     for(UINTN J=0;J<96;J++) assert(Inquiry[1+J]==(J<Cases[I].Actual ? 0x5a : 0xcc));
     Clean();
   }
   ResponseFlags=0; ResponseResidual=0;
   P.InDataBuffer=Data; Cdb[4]=16;
 }
 /* Keep slot 0 outstanding while slot 1 completes >256 requests. An
    incrementing UINT8 tag wraps onto the still-active command's identity.
    Android/Linux bind both request UPIU and doorbell to the reserved slot. */
 {
   EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Held=P;
   UINT8 HeldData[16]; MAP *HeldMap;
   Held.InDataBuffer=HeldData; Held.InTransferLength=16;
   memset(HeldData,0xcc,sizeof(HeldData)); Before=Signals;
   DeferCompletion=TRUE;
   assert(UfsExecScsiCmds(&Owner,0,&Held,(VOID *)1)==EFI_SUCCESS);
   assert(Doorbell==1 && Owner.ReservedSlots==1 && Signals==Before);
   HeldMap=DataMap;
   DeferCompletion=FALSE;
   for(unsigned I=0;I<300;I++) {
     P.InTransferLength=16;
     assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_SUCCESS);
     assert(Doorbell==1 && Owner.ReservedSlots==1 && Signals==Before);
   }
   fprintf(stderr,"UFS task tag regression: slot mismatches=%u active duplicate tags=%u\n",
           TagSlotMismatches,DuplicateTags);
   assert(DuplicateTags==0 && TagSlotMismatches==0);
   DataMap=HeldMap; Complete(1); ProcessAsyncTaskList(NULL,&Owner);
   assert(Signals==Before+1 && HeldData[0]==0x5a); Clean();
 }
 /* Actual READ(10)/WRITE(10) descriptors crossing a 256 KiB PRD boundary. */
 {
   unsigned BeforeReads=WireReads;
   UINT8 *Large=malloc(262656); assert(Large); memset(Large,0xa6,262656);
   memset(Cdb,0,sizeof(Cdb)); Cdb[0]=0x2a; Cdb[7]=2; Cdb[8]=1; P.CdbLength=10; P.DataDirection=EFI_EXT_SCSI_DATA_DIRECTION_WRITE;
   P.OutDataBuffer=Large; P.OutTransferLength=262656;
   assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_SUCCESS); Clean();
   Cdb[0]=0x28; P.DataDirection=EFI_EXT_SCSI_DATA_DIRECTION_READ;
   P.InDataBuffer=Large; P.InTransferLength=262656;
   assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_SUCCESS);
   assert(Large[0]==0x5a && Large[262655]==0x5a && WireReads==BeforeReads+1 && WireWrites==1); Clean();
   memset(Cdb,0,sizeof(Cdb)); Cdb[0]=0x12; Cdb[4]=16; P.CdbLength=6;
   free(Large); P.InDataBuffer=Data; P.OutDataBuffer=NULL; P.OutTransferLength=0;
 }
 /* A successful OCS with an absent or REJECT UPIU is not SCSI success. */
 for(unsigned Async=0;Async<2;Async++) for(unsigned Fault=0;Fault<2;Fault++) {
   ResponseCode=Fault ? 0x3f : 0; P.InTransferLength=16; memset(Data,0xcc,16);
   assert(UfsExecScsiCmds(&Owner,0,&P,Async ? (VOID *)1 : NULL)==(Async ? EFI_SUCCESS : EFI_DEVICE_ERROR));
   if(Async) ProcessAsyncTaskList(NULL,&Owner);
   assert(P.HostAdapterStatus==EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR && P.InTransferLength==0);
   for(UINTN J=0;J<sizeof(Data);J++) assert(Data[J]==0xcc);
   Clean();
 }
 ResponseCode=0x21;
 /* Completion for the wrong request must not expose staged data. The
    missing-writeback case retains initial OCS=F and an all-zero response,
    matching the log signature without assuming its physical cause. */
 for(unsigned Async=0;Async<2;Async++) for(unsigned Fault=1;Fault<=3;Fault++) {
   BadResponseIdentity=Fault<3 ? Fault : 0; MissingWriteback=Fault==3;
   P.InTransferLength=16; memset(Data,0xcc,16); Before=Signals;
   assert(UfsExecScsiCmds(&Owner,7,&P,Async ? (VOID *)1 : NULL)==(Async ? EFI_SUCCESS : EFI_DEVICE_ERROR));
   if(Async) ProcessAsyncTaskList(NULL,&Owner);
   assert(P.HostAdapterStatus==EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR && P.InTransferLength==0);
   assert(Signals==Before+Async);
   for(UINTN J=0;J<sizeof(Data);J++) assert(Data[J]==0xcc);
   Clean();
 }
 BadResponseIdentity=0; MissingWriteback=FALSE;
 /* Every hardware slot, including bit 31, uses its own tag for all three
    transaction families. Software-held slots remain owned after cleanup. */
 for(unsigned Slot=0;Slot<32;Slot++) {
   UINT32 Held=Slot ? (1U<<Slot)-1U : 0; UINT8 Flag=0;
   Owner.ReservedSlots=Held;
   assert(UfsExecNopCmds(&Owner)==EFI_SUCCESS && Owner.ReservedSlots==Held);
   assert(UfsRwFlags(&Owner,TRUE,UfsFlagDevInit,&Flag)==EFI_SUCCESS && Flag==1);
   P.InTransferLength=16;
   assert(UfsExecScsiCmds(&Owner,7,&P,NULL)==EFI_SUCCESS && Owner.ReservedSlots==Held);
   Owner.ReservedSlots=0; Clean();
 }
 BadResponseIdentity=1;
 assert(UfsExecNopCmds(&Owner)==EFI_DEVICE_ERROR); Clean();
 { UINT8 Flag=0; assert(UfsRwFlags(&Owner,TRUE,UfsFlagDevInit,&Flag)==EFI_DEVICE_ERROR); Clean(); }
 BadResponseIdentity=0;
 /* DBR/RSR zero alone cannot locate the failure in the submission sequence.
    Counters distinguish pre-submit admission from a completed doorbell write. */
 Reset(); InterruptStatus=0x20405;
 assert(UfsExecNopCmds(&Owner)==EFI_DEVICE_ERROR);
 assert(!Owner.DoorbellAttempts && !Owner.DoorbellWrites && !Owner.LastDoorbellMask);
 assert(!Hce); Clean();
 Reset(); FatalOnSubmit=0x20405; FatalDoorbellCleared=TRUE;
 assert(UfsExecNopCmds(&Owner)==EFI_DEVICE_ERROR);
 assert(Owner.DoorbellAttempts==1 && Owner.DoorbellWrites==1 && Owner.LastDoorbellMask==1);
 assert(!Doorbell && !Hce); Clean();
 /* A bus fatal error during Query, after a successful NOP, must fence the
    entire HCI without waiting for the deadline or issuing four more queries. */
 Reset(); assert(UfsExecNopCmds(&Owner)==EFI_SUCCESS); Clean();
 {
   unsigned Q=QuerySubmissions, D=DelayUs, Stops=FatalStops;
   FatalOnSubmit=0x00020405; /* Exact IS from waipiolog2 SET_FLAG failure. */
   EFI_STATUS Status=UfsSetFlag(&Owner,UfsFlagDevInit);
   fprintf(stderr,"UFS bus-fatal regression: status=0x%llx queries=%u delay-us=%u HCE=%u\n",
     (unsigned long long)Status,QuerySubmissions-Q,DelayUs-D,Hce);
   assert(Status==EFI_DEVICE_ERROR);
   assert(QuerySubmissions==Q+1 && DelayUs==D && FatalStops==Stops+1);
   assert(Owner.IoUnavailable && !Hce && !FatalUnmapWithoutStop);
   assert(UfsExecNopCmds(&Owner)==EFI_NOT_READY); Clean();
 }
 /* Fatal takes precedence even when the same read observes a cleared doorbell
    and plausible response. Exercise each Linux INT_FATAL_ERRORS bit. */
 for(unsigned Bit=0;Bit<6;Bit++) for(unsigned Clear=0;Clear<2;Clear++)
   for(unsigned Async=0;Async<2;Async++) {
     static const UINT32 FatalBits[]={0x80,0x800,0x1000,0x10000,0x20000,0x40000};
     Reset(); FatalOnSubmit=FatalBits[Bit]; FatalDoorbellCleared=(BOOLEAN)Clear;
     P.InDataBuffer=Data; P.InTransferLength=16; memset(Data,0xcc,16);
     unsigned D=DelayUs, Stops=FatalStops; Before=Signals;
     assert(UfsExecScsiCmds(&Owner,0,&P,Async ? (VOID *)1 : NULL)==
            (Async ? EFI_SUCCESS : EFI_DEVICE_ERROR));
     if(Async) ProcessAsyncTaskList(NULL,&Owner);
     assert(Owner.IoUnavailable && !Hce && FatalStops==Stops+1 && DelayUs==D);
     assert(P.HostAdapterStatus==EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR);
     assert(P.InTransferLength==0 && Signals==Before+Async);
     for(UINTN J=0;J<sizeof(Data);J++) assert(Data[J]==0xcc);
     Clean();
   }
 /* One fatal condition retires two outstanding asynchronous requests through
    one whole-controller fence, without exposing either staged payload. */
 {
   Reset(); DeferCompletion=TRUE;
   EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Second=P; UINT8 SecondData[16];
   P.InDataBuffer=Data; P.InTransferLength=16; memset(Data,0xcc,16);
   Second.InDataBuffer=SecondData; Second.InTransferLength=16; memset(SecondData,0xcc,16);
   Before=Signals; unsigned Stops=FatalStops;
   assert(UfsExecScsiCmds(&Owner,0,&P,(VOID *)1)==EFI_SUCCESS);
   assert(UfsExecScsiCmds(&Owner,0,&Second,(VOID *)1)==EFI_SUCCESS);
   assert(Doorbell==3); InterruptStatus=0x20000;
   ProcessAsyncTaskList(NULL,&Owner);
   assert(Signals==Before+2 && FatalStops==Stops+1 && !FatalUnmapWithoutStop);
   assert(P.InTransferLength==0 && Second.InTransferLength==0);
   assert(Data[0]==0xcc && SecondData[0]==0xcc); Clean(); DeferCompletion=FALSE;
 }
 /* A failed global stop keeps all DMA ownership pinned and stops retries. */
 Reset(); FatalOnSubmit=0x20000; FailFence=TRUE;
 P.InDataBuffer=Data; P.InTransferLength=16;
 assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_DEVICE_ERROR);
 assert(Owner.IoUnavailable && Owner.FenceFailed && Hce && Mappings && Buffers);
 Before=FatalSubmissions; ProcessAsyncTaskList(NULL,&Owner);
 assert(FatalSubmissions==Before && Mappings && Buffers);
 FailFence=FALSE; assert(UfsControllerStop(&Owner)==EFI_SUCCESS);
 (UFS_PASS_THRU_TRANS_REQ_FROM_THIS(Owner.Queue.ForwardLink))->DmaFenced=TRUE;
 ProcessAsyncTaskList(NULL,&Owner); Clean(); Reset();
 /* Failed synchronous copyback detaches all caller references before return. */
 P.InDataBuffer=malloc(16); assert(P.InDataBuffer); P.InTransferLength=16; memset(P.InDataBuffer,0xcc,16); FailUnmap=TRUE;
 assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_DEVICE_ERROR);
 assert(P.InTransferLength==0 && ((UINT8 *)P.InDataBuffer)[0]==0xcc && Owner.ReservedSlots);
 free(P.InDataBuffer); P.InDataBuffer=NULL;
 FailUnmap=FALSE; ProcessAsyncTaskList(NULL,&Owner); Clean();
 /* Async success plus event failure retry, exactly one successful event. */
 Reset(); P.InDataBuffer=Data; P.InTransferLength=16; Before=Signals;
 assert(UfsExecScsiCmds(&Owner,0,&P,(VOID *)1)==EFI_SUCCESS);
 FailSignal=TRUE; ProcessAsyncTaskList(NULL,&Owner); assert(Signals==Before && Owner.ReservedSlots);
 FailSignal=FALSE; ProcessAsyncTaskList(NULL,&Owner); assert(Signals==Before+1); Clean();
 /* Async failed copyback signals failure once and releases caller lifetime. */
 Reset(); P.InDataBuffer=malloc(16); P.InTransferLength=16; Before=Signals; FailUnmap=TRUE;
 assert(UfsExecScsiCmds(&Owner,0,&P,(VOID *)1)==EFI_SUCCESS);
 ProcessAsyncTaskList(NULL,&Owner);
 assert(Signals==Before+1 && P.InTransferLength==0 && Owner.ReservedSlots);
 free(P.InDataBuffer); P.InDataBuffer=NULL;
 FailUnmap=FALSE; ProcessAsyncTaskList(NULL,&Owner);
 assert(Signals==Before+1); Clean();
 /* Unfenced synchronous timeout retains DMA pages, even after caller free. */
 Reset(); Hang=FailFence=TRUE; P.InDataBuffer=malloc(16); P.InTransferLength=16;
 assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_TIMEOUT);
 assert(Owner.IoUnavailable && Owner.ReservedSlots && Mappings && Buffers);
 free(P.InDataBuffer); P.InDataBuffer=NULL;
 assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_NOT_READY);
 FailFence=FALSE; ProcessAsyncTaskList(NULL,&Owner); assert(Mappings && Owner.FenceFailed);
 /* Explicit Stop fences hardware; timer no longer attempts repeated recovery. */
 assert(UfsControllerStop(&Owner)==EFI_SUCCESS);
 (UFS_PASS_THRU_TRANS_REQ_FROM_THIS(Owner.Queue.ForwardLink))->DmaFenced=TRUE;
 ProcessAsyncTaskList(NULL,&Owner); Clean();
 /* A fenced synchronous WRITE timeout clears the write transfer length too. */
 Reset(); Hang=TRUE; P.DataDirection=EFI_EXT_SCSI_DATA_DIRECTION_WRITE;
 P.InDataBuffer=NULL; P.InTransferLength=0; P.OutDataBuffer=Data; P.OutTransferLength=16;
 assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_TIMEOUT);
 assert(P.OutTransferLength==0 && P.InTransferLength==0); Clean();
 P.DataDirection=EFI_EXT_SCSI_DATA_DIRECTION_READ; P.OutDataBuffer=NULL; P.OutTransferLength=0;
 /* A timer/global stop during Map cannot be followed by a new doorbell. */
 Reset(); StopDuringMap=TRUE; P.InDataBuffer=Data; P.InTransferLength=16;
 assert(UfsExecScsiCmds(&Owner,0,&P,NULL)==EFI_NOT_READY && Doorbell==0); Clean();
 Reset();
 /* Reuse a slot whose prior SCSI request left a PRDT behind. NOP and
    management requests must explicitly publish a zero PRDT length. */
 Trd[0].PrdtL = 7; Trd[0].PrdtO = 99;
 assert(UfsExecNopCmds(&Owner)==EFI_SUCCESS);
 assert(Trd[0].PrdtL == 0 && Trd[0].PrdtO == 256); Clean();
 BadManagementResponse=TRUE;
 assert(UfsExecNopCmds(&Owner)==EFI_DEVICE_ERROR); Clean();
 {
   UINT8 Flag=0xcc;
   Trd[0].PrdtL = 3; Trd[0].PrdtO = 12;
   assert(UfsReadFlag(&Owner,UfsFlagDevInit,&Flag)==EFI_DEVICE_ERROR && Flag==0xcc);
   assert(Trd[0].PrdtL == 0 && Trd[0].PrdtO == 256); Clean();
   BadManagementResponse=FALSE;
   assert(UfsReadFlag(&Owner,UfsFlagDevInit,&Flag)==EFI_SUCCESS && Flag==1); Clean();
   InvalidQueryIndex=TRUE; Before=QuerySubmissions;
   UFS_DEVICE_MANAGEMENT_REQUEST_PACKET Dm={.Opcode=UtpQueryFuncOpcodeRdFlag,
     .DescId=UfsFlagDevInit,.DataBuffer=&Flag,.TransferLength=1,.Timeout=1000};
   assert(UfsSendDmRequest(&Owner,&Dm)==EFI_INVALID_PARAMETER);
   assert(QuerySubmissions==Before+1); Clean(); InvalidQueryIndex=FALSE;
   Hang=TRUE; Before=QuerySubmissions;
   assert(UfsSendDmRequest(&Owner,&Dm)==EFI_TIMEOUT);
   assert(QuerySubmissions==Before+5 && DiagnosticRegisters==5*5);
   assert(!Doorbell); Clean(); Hang=FALSE;
 }
 {
   UINT8 Desc[8]; UINT32 Size;
   for(DescriptorMode=1;DescriptorMode<=2;DescriptorMode++) {
     memset(Desc,0xcc,sizeof(Desc)); Size=sizeof(Desc);
     assert(UfsRwDeviceDesc(&Owner,TRUE,UfsUnitDesc,0,0,Desc,&Size)==(DescriptorMode==1 ? EFI_SUCCESS : EFI_DEVICE_ERROR));
     if(DescriptorMode==1) {
       assert(Size==4 && Desc[0]==4 && Desc[1]==UfsUnitDesc && Desc[3]==1);
       for(UINTN J=4;J<sizeof(Desc);J++) assert(Desc[J]==0xcc);
     } else { assert(Size==0); for(UINTN J=0;J<sizeof(Desc);J++) assert(Desc[J]==0xcc); }
     Clean();
   }
   DescriptorMode=3; Size=7;
   for(UINTN J=0;J<Size;J++) Desc[J]=(UINT8)(0x70+J);
   assert(UfsRwDeviceDesc(&Owner,FALSE,UfsConfigDesc,0,0,Desc,&Size)==EFI_SUCCESS && Size==7);
   Clean();
   DescriptorMode=0;
 }
 BadOcs=TRUE; assert(UfsExecNopCmds(&Owner)==EFI_DEVICE_ERROR); Clean(); BadOcs=FALSE;
 /* Failed common-buffer revoke retains an owner without stealing a slot. */
 {
   VOID *Host, *Token; EFI_PHYSICAL_ADDRESS Device;
   ShortCommon=FailAllUnmap=TRUE; Owner.ReservedSlots=1U<<5;
   assert(UfsAllocateAlignCommonBuffer(&Owner,128,&Host,&Device,&Token)==EFI_BAD_BUFFER_SIZE);
   assert(!Host && !Token && !Device && Mappings==1 && Buffers==1);
   ShortCommon=FailAllUnmap=FALSE; ProcessAsyncTaskList(NULL,&Owner);
   assert(Owner.ReservedSlots==(1U<<5) && !Mappings && !Buffers && !Pools);
   Owner.ReservedSlots=0; Clean();
 }
 /* Ring ownership is published before the first potentially failing MMIO. */
 free(Owner.UtpUcdBase); Owner.UtpUcdBase=NULL; Owner.UtpTrlBase=NULL;
 FailWriteOffset=UFS_HC_UTRLBA_OFFSET;
 assert(UfsInitTransferRequestList(&Owner)==EFI_DEVICE_ERROR);
 assert(Owner.TrlMapping && Owner.UtpTrlBase!=Trd && Owner.Nutrs==32);
 assert(Unmap(&H,Owner.TrlMapping)==EFI_SUCCESS); Owner.TrlMapping=NULL;
 assert(Free(&H,1,Owner.UtpTrlBase)==EFI_SUCCESS); Owner.UtpTrlBase=Trd;
 assert(Unmap(&H,Owner.UcdMapping)==EFI_SUCCESS); Owner.UcdMapping=NULL;
 assert(Free(&H,24,Owner.UtpUcdBase)==EFI_SUCCESS); Owner.UtpUcdBase=NULL;
 FailWriteOffset=0; Clean();
 assert(TagSlotMismatches==0 && DuplicateTags==0);
 puts("UFS completion: task tag slot identity/wrap, wire READ/WRITE PRDT, NOP/query/SCSI response types, copyback, async retry and timeout retention PASS");
 return 0;
}
