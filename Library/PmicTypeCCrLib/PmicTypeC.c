/* Wire policy derived from Linux pmic_glink_altmode.c and Qualcomm Android
 * altmode-glink.c/ucsi_glink.c. OS retains every UCSI policy command.
 * Copyright (c) 2020-2021 The Linux Foundation.
 * Copyright (c) 2022-2023, Linaro Ltd.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/PmicTypeCCrLib.h>
#include <oskal/cr_string.h>
#define USBC 32779U
#define PAN 32780U
#define TIMEOUT 5000000ULL
STATIC UINT32 Le32(CONST UINT8 *P){return (UINT32)P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC UINT64 Le64(CONST UINT8 *P){return Le32(P)|((UINT64)Le32(P+4)<<32);}
STATIC VOID Put32(UINT8 *P,UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
STATIC BOOLEAN RouteEqual(CONST CR_USB_TYPEC_STATE *A,CONST CR_USB_TYPEC_STATE *B){
  return A->PdReady==B->PdReady&&A->Connected==B->Connected&&A->PowerSource==B->PowerSource&&
    A->DataRole==B->DataRole&&A->Orientation==B->Orientation&&A->DpActive==B->DpActive&&
    A->Usb3==B->Usb3&&A->PinAssignment==B->PinAssignment&&A->DpLanes==B->DpLanes&&A->LastError==B->LastError;
}
STATIC VOID Update(CR_PMIC_TYPEC *P){
  CR_USB_TYPEC_STATE S={0};S.LastError=CR_NOT_READY;
  if(!P->Closed&&P->Online&&P->StatusValid&&!P->UsbcFaulted){
    S=P->Status;S.LastError=CR_SUCCESS;S.PdReady=TRUE;
    if(S.Connected&&P->AltValid&&P->PanEnabled&&!P->PanFaulted){
      S.Orientation=P->Alt.Orientation;S.DpActive=P->Alt.DpActive;S.DpLanes=P->Alt.DpLanes;
      S.PinAssignment=P->Alt.PinAssignment;S.Usb3=P->Alt.Usb3;S.Hpd=P->Alt.Hpd;S.HpdIrqSequence=P->Alt.HpdIrqSequence;
    }
  }
  S.Sequence=P->State.Sequence;
  if(!RouteEqual(&P->State,&S)){
    if(P->Generation==CR_MAX_UINT64){P->Closed=TRUE;S.LastError=CR_OUT_OF_RESOURCES;S.PdReady=FALSE;}
    else ++P->Generation;
  }
  if(!RouteEqual(&P->State,&S)||P->State.Hpd!=S.Hpd||P->State.HpdIrqSequence!=S.HpdIrqSequence)++S.Sequence;
  P->State=S;
}
VOID CrPmicTypeCInit(CR_PMIC_TYPEC *P,CR_PMIC_TYPEC_SEND Send,VOID *Context){
  cr_memset(P,0,sizeof(*P));P->Send=Send;P->Context=Context;P->State.LastError=CR_NOT_READY;P->Generation=1;
}
VOID CrPmicTypeCPeer(CR_PMIC_TYPEC *P,BOOLEAN Online,UINT32 Peer,UINT32 Adsp){
  if(P->Closed)return;
  Online=Online&&Peer&&Adsp;
  if(P->PeerGeneration!=Peer||P->AdspGeneration!=Adsp||P->Online!=Online){
    BOOLEAN NewAdsp=Adsp&&Adsp!=P->AdspGeneration;
    /* A pending old PAN reply cannot be reused in the same remote boot. */
    if((P->PanPending||P->PanUncertain)&&!NewAdsp)P->PanFaulted=P->PanUncertain=TRUE;
    if((P->ReadPending||P->WritePending)&&!NewAdsp)P->UsbcFaulted=TRUE;
    if(NewAdsp){P->PanFaulted=P->UsbcFaulted=P->PanUncertain=FALSE;}
    P->StatusValid=P->AltValid=P->WriteAcked=P->WritePending=P->ReadPending=FALSE;
    P->PanPending=P->PanEnabled=FALSE;P->Acks=0;P->RetryUs=0;P->Control=0;
    P->PeerGeneration=Peer;if(Adsp)P->AdspGeneration=Adsp;P->Online=Online;Update(P);
  }
}
VOID CrPmicTypeCInvalidateUsbc(CR_PMIC_TYPEC *P){P->StatusValid=P->WriteAcked=P->WritePending=P->ReadPending=FALSE;P->Control=0;Update(P);}
VOID CrPmicTypeCObserveSend(CR_PMIC_TYPEC *P,CONST UINT8 *D,UINTN N,CR_STATUS S,UINT64 Now){
  if(!P||!D||N<12||P->Closed||!P->Online||Le32(D)!=USBC||Le32(D+4)!=1)return;
  CrPmicTypeCExpire(P,Now);if(P->UsbcFaulted)return;
  if(S!=CR_SUCCESS){if(S!=CR_NOT_READY){P->UsbcFaulted=TRUE;CrPmicTypeCInvalidateUsbc(P);}return;}
  UINT32 Op=Le32(D+8);
  if((Op==0x11&&N==12)||(Op==0x12&&N==64)){
    if(P->ReadPending||P->WritePending||Now>CR_MAX_UINT64-TIMEOUT){P->UsbcFaulted=TRUE;CrPmicTypeCInvalidateUsbc(P);return;}
    P->UsbcDeadlineUs=Now+TIMEOUT;
  }
  if(Op==0x11&&N==12){P->ReadPending=TRUE;return;}
  if(Le32(D+8)!=0x12||N!=64)return;
  P->ReadPending=FALSE;P->Control=Le64(D+20);P->WritePending=TRUE;P->WriteAcked=FALSE;
  if((P->Control&255)==1){P->StatusValid=P->AltValid=FALSE;Update(P);}
}
STATIC VOID Alt(CR_PMIC_TYPEC *P,CONST UINT8 *D,UINTN N){
  if(N!=32||D[12]!=0||D[13]>2)return;
  /* SM8450/SC8280 format: fixed port/orientation/mux payload; SVID belongs
     to opcode[31:16], not the optional payload VID/SVID pair. */
  CR_USB_TYPEC_STATE A={0};UINT8 Mux=D[14],Pin=D[20]&63;UINT32 Svid=Le32(D+8)>>16;
  A.Orientation=D[13]==0?UsbTypeCNormal:D[13]==1?UsbTypeCReverse:UsbTypeCUnoriented;
  A.Usb3=Mux==1;A.HpdIrqSequence=P->Alt.HpdIrqSequence;
  if(Svid==0xff01&&(Mux==2||Mux==3)&&A.Orientation!=UsbTypeCUnoriented&&
     ((Mux==2&&(Pin==3||Pin==5))||(Mux==3&&(Pin==4||Pin==6)))){
    A.DpActive=TRUE;A.DpLanes=Mux==2?4:2;A.PinAssignment=Pin;A.Usb3=Mux==3;A.Hpd=(D[20]&64)!=0;
    if(D[20]&128)++A.HpdIrqSequence;
  }else if(Mux!=1){A.Orientation=UsbTypeCUnoriented;}
  /* Unknown mux/SVID is observed safe, never promoted to an active route. */
  BOOLEAN Changed=!P->AltValid||!RouteEqual(&P->Alt,&A);
  P->Alt=A;P->AltValid=TRUE;
  if(Changed)P->StatusValid=FALSE; /* wait for OS GET_CONNECTOR_STATUS */
  if(P->Acks==32){P->PanFaulted=TRUE;P->AltValid=FALSE;}else ++P->Acks;
  Update(P);
}
VOID CrPmicTypeCReceive(CR_PMIC_TYPEC *P,CONST UINT8 *D,UINTN N,UINT64 Now){
  if(!P||!D||N<12||P->Closed||!P->Online||(Le32(D+4)!=1&&Le32(D+4)!=2))return;
  CrPmicTypeCExpire(P,Now);UINT32 Owner=Le32(D),Op=Le32(D+8);
  if(Owner==PAN){
    if((Op&255)==0x16){if(!P->PanFaulted)Alt(P,D,N);return;}
    /* Captured HDK replies are16bytes, mainline/Android callbacks use only
       the header completion. No invented status interpretation of its tail. */
    if(Op==0x15&&(N==12||N==16)&&(P->PanPending||P->PanUncertain)){
      if(!P->PanFaulted){if(P->PanCommand==0x10)P->PanEnabled=TRUE;else if(P->Acks)--P->Acks;}
      P->PanPending=P->PanUncertain=FALSE;Update(P);
    }
    return;
  }
  if(Owner!=USBC||P->UsbcFaulted)return;
  if(Op==0x13&&N==24&&Le32(D+16)==0){
    if(((Le32(D+12)>>1)&127)==1){P->StatusValid=FALSE;Update(P);}return;
  }
  if(Op==0x12&&N==16&&P->WritePending){P->WritePending=FALSE;P->WriteAcked=Le32(D+12)==0;
    if(!P->WriteAcked){P->StatusValid=FALSE;Update(P);}return;}
  if(Op!=0x11||N!=64||!P->ReadPending)return;
  P->ReadPending=FALSE;
  if(!P->WriteAcked||P->Control!=0x10012)return;
  UINT16 Version=(UINT16)(D[12]|((UINT16)D[13]<<8));UINT32 Cci=Le32(D+16),Len=(Cci>>8)&255;
  if(Version<0x100||Version>=0x200||Le32(D+60)||!(Cci&(1U<<31))||
     (Cci&((1U<<25)|(1U<<26)|(1U<<27)|(1U<<28)|(1U<<29)|(1U<<30)))||Len<9||Len>16)return;
  UINT32 Word=Le32(D+28),Partner=Word>>29;CR_USB_TYPEC_STATE S={0};
  S.Connected=(Word&(1U<<19))!=0;S.PowerSource=S.Connected&&(Word&(1U<<20))!=0;
  S.DataRole=!S.Connected?UsbTypeCNone:Partner==1?UsbTypeCDevice:(Partner==2||Partner==4)?UsbTypeCHost:UsbTypeCNone;
  if(!S.Connected)P->AltValid=FALSE;P->WriteAcked=FALSE;P->Status=S;P->StatusValid=TRUE;Update(P);
}
VOID CrPmicTypeCExpire(CR_PMIC_TYPEC *P,UINT64 Now){
  if(!P||P->Closed)return;
  if((P->ReadPending||P->WritePending)&&Now>=P->UsbcDeadlineUs){P->UsbcFaulted=TRUE;CrPmicTypeCInvalidateUsbc(P);}
  if(P->PanPending&&Now>=P->DeadlineUs){P->PanPending=FALSE;P->PanUncertain=P->PanFaulted=TRUE;P->AltValid=FALSE;Update(P);}
}
CR_STATUS CrPmicTypeCTick(CR_PMIC_TYPEC *P,UINT64 Now){
  if(!P||!P->Send)return CR_INVALID_PARAMETER;
  if(P->Closed||!P->Online)return CR_NOT_READY;
  BOOLEAN Pending=P->PanPending;CrPmicTypeCExpire(P,Now);
  if(P->PanFaulted)return Pending?CR_TIMEOUT:CR_NOT_READY;
  if(P->PanPending)return CR_SUCCESS;
  if((P->PanEnabled&&!P->Acks)||Now<P->RetryUs)return CR_SUCCESS;
  if(Now>CR_MAX_UINT64-TIMEOUT)return CR_INVALID_PARAMETER;
  UINT8 D[24]={0};Put32(D,PAN);Put32(D+4,1);Put32(D+8,0x15);UINT32 Command=P->PanEnabled?0x11:0x10;Put32(D+12,Command);
  P->PanCommand=Command;CR_STATUS S=P->Send(P->Context,D,sizeof(D));
  if(S==CR_SUCCESS){P->PanCommand=Command;P->PanPending=TRUE;P->DeadlineUs=Now+TIMEOUT;}
  else if(S==CR_NOT_READY)P->RetryUs=Now+20000; /* no packet committed */
  else{P->PanUncertain=P->PanFaulted=TRUE;P->AltValid=FALSE;Update(P);if(!CR_ERROR(S))S=CR_NOT_READY;}
  return S;
}
BOOLEAN CrPmicTypeCDrained(CONST CR_PMIC_TYPEC *P){return P&&!P->PanPending&&!P->PanUncertain&&(P->PanFaulted||!P->Acks);}
VOID CrPmicTypeCClose(CR_PMIC_TYPEC *P){P->Closed=TRUE;P->StatusValid=P->AltValid=FALSE;Update(P);}
CR_STATUS CrPmicTypeCGet(CONST CR_PMIC_TYPEC *P,CR_USB_TYPEC_STATE *Out,UINT64 *Generation){
  if(!P||!Out||!Generation)return CR_INVALID_PARAMETER;
  if(P->Closed||!P->Online||!P->StatusValid||P->UsbcFaulted||P->State.LastError!=CR_SUCCESS)return CR_NOT_READY;
  *Out=P->State;*Generation=P->Generation;return CR_SUCCESS;
}
