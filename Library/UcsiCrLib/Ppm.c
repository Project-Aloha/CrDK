/* Qualcomm PMIC wire protocol; Linux ucsi_glink.c / Android ucsi_glink.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ucsi/Ppm.h>
#include <oskal/cr_string.h>
#define OWNER 32779U
#define READ 0x11U
#define WRITE 0x12U
#define NOTIFY 0x13U
#define TIMEOUT_US 5000000ULL
STATIC UINT32 Get32(CONST UINT8 *P) { UINT32 V; cr_memcpy(&V,P,4);return V; }
STATIC VOID Put32(UINT8 *P,UINT32 V) { cr_memcpy(P,&V,4); }
VOID CrUcsiPpmInitialize(CR_UCSI_PPM *P,CR_UCSI_PPM_SEND Send,VOID *Context) {
  cr_memset(P,0,sizeof(*P));P->Send=Send;P->Context=Context;
}
VOID CrUcsiPpmPeer(CR_UCSI_PPM *P,BOOLEAN Online,UINT32 Generation) {
  if (P->Online==Online && P->Generation==Generation) return;
  if (P->Operation!=CrUcsiPpmIdle && !P->Done) {P->Done=TRUE;P->Status=CR_NOT_READY;}
  if(P->Generation!=Generation)P->Faulted=FALSE;
  P->Online=Online;P->Generation=Generation;P->Version=0;P->NotificationHead=P->NotificationCount=0;
}
STATIC CR_STATUS Start(CR_UCSI_PPM *P,CR_UCSI_PPM_OPERATION Op,CONST UINT8 *Block,UINT64 Now) {
  UINT8 Packet[64];CR_STATUS Status;
  if (P==NULL || P->Send==NULL || (Op==CrUcsiPpmWriting && Block==NULL)) return CR_INVALID_PARAMETER;
  if (!P->Online || P->Faulted) return CR_NOT_READY;
  if (P->Operation!=CrUcsiPpmIdle) return CR_BUSY;
  if (Now>CR_MAX_UINT64-TIMEOUT_US) return CR_INVALID_PARAMETER;
  if (Op==CrUcsiPpmWriting && (P->Version<0x100 || P->Version>=0x200)) return CR_UNSUPPORTED;
  cr_memset(Packet,0,sizeof(Packet));Put32(Packet,OWNER);Put32(Packet+4,1);
  Put32(Packet+8,Op==CrUcsiPpmReading?READ:WRITE);
  /* Only CONTROL and MESSAGE_OUT are writable. VERSION/CCI/MESSAGE_IN are
   * firmware-owned; never reflect a stale Windows snapshot back into them. */
  if (Op==CrUcsiPpmWriting) {cr_memcpy(Packet+20,Block+8,8);cr_memcpy(Packet+44,Block+32,16);}
  P->Operation=Op;P->Done=P->Cancelled=FALSE;P->Status=CR_NOT_READY;
  P->DeadlineUs=Now+TIMEOUT_US;cr_memset(P->Block,0,sizeof(P->Block));
  Status=P->Send(P->Context,Packet,Op==CrUcsiPpmReading?12U:64U);
  if (CR_ERROR(Status)) {
    P->Operation=CrUcsiPpmIdle;
    /* NOT_READY guarantees no queued packet; every other send error can follow
     * a hardware commit. Retain generation uncertainty rather than retrying. */
    if (Status!=CR_NOT_READY) P->Faulted=TRUE;
  }
  return Status;
}
CR_STATUS CrUcsiPpmRead(CR_UCSI_PPM *P,UINT64 Now) {return Start(P,CrUcsiPpmReading,NULL,Now);}
CR_STATUS CrUcsiPpmWrite(CR_UCSI_PPM *P,CONST UINT8 Block[48],UINT64 Now) {return Start(P,CrUcsiPpmWriting,Block,Now);}
VOID CrUcsiPpmReceive(CR_UCSI_PPM *P,CONST UINT8 *Data,UINTN Length) {
  UINT32 Op,Type;
  if (P==NULL || Data==NULL || Length<12 || !P->Online || P->Faulted) return;
  Type=Get32(Data+4);if (Get32(Data)!=OWNER || (Type!=1 && Type!=2)) return;
  Op=Get32(Data+8);
  if (Op==NOTIFY && Length==24 && Get32(Data+16)==0) {
    if (P->NotificationCount==CR_UCSI_NOTIFY_CAPACITY) {P->Faulted=TRUE;return;}
    P->Notifications[(P->NotificationHead+P->NotificationCount++)%CR_UCSI_NOTIFY_CAPACITY]=Get32(Data+12);
    return;
  }
  if (P->Done) return;
  if (Op==READ && P->Operation==CrUcsiPpmReading && (Length==64 || Length==544)) {
    UINT16 Version;UINTN Size=Length-16;
    cr_memcpy(&Version,Data+12,2);
    P->Status=Get32(Data+12+Size)==0?CR_SUCCESS:CR_DEVICE_ERROR;
    if (!CR_ERROR(P->Status)) {
      /* UcmUcsiCx 1.0 exposes exactly 48 bytes. Never truncate UCSI >=2.0. */
      if (Size!=48 || Version<0x100 || Version>=0x200 ||
          (P->Version!=0 && P->Version!=Version)) P->Status=CR_UNSUPPORTED;
      else if (((Get32(Data+16)>>8)&0xffU)>16U) P->Status=CR_COMPROMISED_DATA;
      else {P->Version=Version;cr_memcpy(P->Block,Data+12,48);}
    }
    P->Done=TRUE;
  } else if (Op==WRITE && P->Operation==CrUcsiPpmWriting && Length==16) {
    P->Status=Get32(Data+12)==0?CR_SUCCESS:CR_DEVICE_ERROR;P->Done=TRUE;
  }
}
VOID CrUcsiPpmTick(CR_UCSI_PPM *P,UINT64 Now) {
  if (P->Operation!=CrUcsiPpmIdle && !P->Done && (P->Faulted || Now>=P->DeadlineUs)) {
    P->Faulted=TRUE;P->Done=TRUE;P->Status=CR_TIMEOUT;
  }
}
VOID CrUcsiPpmCancel(CR_UCSI_PPM *P) {if (P->Operation!=CrUcsiPpmIdle) P->Cancelled=TRUE;}
CR_STATUS CrUcsiPpmTake(CR_UCSI_PPM *P,UINT8 Block[48]) {
  CR_STATUS Status;
  if (P==NULL || P->Operation==CrUcsiPpmIdle || !P->Done) return CR_NOT_READY;
  Status=P->Cancelled?CR_ABORT:P->Status;
  if (Block!=NULL && !CR_ERROR(Status) && P->Operation==CrUcsiPpmReading) cr_memcpy(Block,P->Block,48);
  P->Operation=CrUcsiPpmIdle;P->Done=P->Cancelled=FALSE;return Status;
}
BOOLEAN CrUcsiPpmNotification(CR_UCSI_PPM *P,UINT32 *Cci) {
  if (P==NULL || Cci==NULL || P->NotificationCount==0) return FALSE;
  *Cci=P->Notifications[P->NotificationHead];P->NotificationHead=(P->NotificationHead+1)%CR_UCSI_NOTIFY_CAPACITY;
  --P->NotificationCount;return TRUE;
}
