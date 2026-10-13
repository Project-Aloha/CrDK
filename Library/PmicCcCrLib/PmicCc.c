/* PM8150B observer derived from Qualcomm SMB5/PD register definitions.
 * Reference Android SM8150 1dd473ab, SM8250 cad09e06, and Linux a53261b6:
 * drivers/power/supply/qcom/smb5-{lib.c,reg.h}, include/linux/qpnp/qpnp-revid.h,
 * drivers/usb/pd/qpnp-pdphy.c; Linux drivers/usb/typec/tcpm/qcom/.
 * Original Qualcomm files Copyright (c) 2017-2020, The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <Library/PmicCcCrLib.h>
#include <oskal/cr_string.h>
#ifdef _KERNEL_MODE
#include "PmicCc.tmh"
#endif
STATIC VOID Changed(VOID *X) {
 CR_PMIC_CC *C=X;if(!C->Stopping){C->Dirty=TRUE;C->Valid=FALSE;C->LastStatus=CR_NOT_READY;C->Wake(C->WakeContext);}
}
STATIC CR_STATUS Sample(CR_PMIC_CC *C,UINT8 *R) {
 CR_STATUS S=C->Bus->Read(C->Bus,2,0x1506,R,6);
 if(S==CR_SUCCESS)S=C->Bus->Read(C->Bus,2,0x1740,R+6,1);
 if(S==CR_SUCCESS)S=C->Bus->Read(C->Bus,2,0x1746,R+7,1);
 return S;
}
CR_STATUS CrPmicCcService(CR_PMIC_CC *C) {
 UINT8 R[8],Again[8];CR_USB_TYPEC_STATE Next={0};CR_STATUS S;
 if(!C||!C->Initialized||C->Stopping)return CR_NOT_READY;
 S=C->Bus->Check(C->Bus);if(S!=CR_SUCCESS)goto Failure;
 if(!C->Dirty)return C->LastStatus;
 S=Sample(C,R);if(S==CR_SUCCESS)S=Sample(C,Again);
 if(S!=CR_SUCCESS)goto Failure;
 if(cr_memcmp(R,Again,sizeof(R))!=0){S=CR_NOT_READY;goto Failure;}
 UINT8 Misc=R[5];BOOLEAN Attached=(Misc&1)!=0;
 /* A transition, water or VBUS error is not a validated attach/detach. */
 if(((R[3]&0x20)!=0)!=Attached || (Misc&0x90) || (Attached && !(Misc&8))) {
  S=CR_NOT_READY;goto Failure;
 }
 Next.Connected=Attached;Next.LastError=CR_SUCCESS;
 if(Attached) {
  Next.Orientation=(Misc&2)?UsbTypeCReverse:UsbTypeCNormal;
  Next.PowerSource=(Misc&0x40)!=0;
  /* Do not infer a negotiated data role from CC while an inherited PD PHY
     is enabled. Unit A does not own that policy engine or observe its swaps. */
  if(!(R[7]&1)) {
   if(Next.PowerSource && ((R[2]&0x1f)==8 || (R[2]&0x1f)==4))Next.DataRole=UsbTypeCHost;
   else if(!Next.PowerSource && ((R[0]&0x7f)==8 || (R[0]&0x7f)==4 || (R[0]&0x7f)==2))Next.DataRole=UsbTypeCDevice;
  }
 }
 if(!C->Valid || C->LastStatus!=CR_SUCCESS || cr_memcmp(R,C->Raw,sizeof(R))!=0) {
  if(C->State.Sequence==~(UINT32)0){S=CR_OUT_OF_RESOURCES;goto Failure;}
  Next.Sequence=C->State.Sequence+1;
 }else Next.Sequence=C->State.Sequence;
 C->State=Next;cr_memcpy(C->Raw,R,sizeof(R));C->Valid=TRUE;C->Dirty=FALSE;C->LastStatus=CR_SUCCESS;
 return CR_SUCCESS;
Failure:
 C->Valid=FALSE;C->LastStatus=S;return S;
}
CR_STATUS CrPmicCcStart(CR_PMIC_CC *C,CONST CR_PMIC_CC_TARGET *T,
 CR_SPMI_BUS *Bus,VOID (*Wake)(VOID *),VOID *WakeContext) {
 if(!C||!T||!Bus||!Wake||!Bus->Check||!Bus->Read||!Bus->Subscribe||!Bus->Enable||!Bus->Unsubscribe)
  return CR_INVALID_PARAMETER;
 if(C->Initialized)return CR_ALREADY_STARTED;
 if((T->Board!=CR_PMIC_CC_HDK8150 && T->Board!=CR_PMIC_CC_HDK8245) ||
    T->Batteryless!=(T->Board==CR_PMIC_CC_HDK8245))return CR_UNSUPPORTED;
 UINT8 Rev[6];CR_STATUS S=Bus->Read(Bus,2,0x100,Rev,sizeof(Rev));if(S!=CR_SUCCESS)return S;
 /* Exact PM8150B type/subtype, source-documented v1/v2 revision tuples. */
 if(Rev[0]||Rev[1]||Rev[2]||(Rev[3]!=1&&Rev[3]!=2)||Rev[4]!=0x51||Rev[5]!=0x20)return CR_UNSUPPORTED;
 cr_memset(C,0,sizeof(*C));C->Bus=Bus;C->Target=*T;C->Wake=Wake;C->WakeContext=WakeContext;
 C->Initialized=TRUE;C->Dirty=TRUE;C->LastStatus=CR_NOT_READY;cr_memcpy(C->Revision,Rev,sizeof(Rev));
 for(UINT8 I=0;I<8;I++) {
  CR_INTERRUPT_TRIGGER_TYPE Trigger=(I==1||I==3||I==6)?CR_INTERRUPT_TRIGGER_EDGE_BOTH:CR_INTERRUPT_TRIGGER_EDGE_RISING;
  S=Bus->Subscribe(Bus,2,0x15,I,Trigger,Changed,C,&C->Irqs[I]);if(S!=CR_SUCCESS)return S;
 }
 for(UINT8 I=0;I<8;I++){S=Bus->Enable(Bus,&C->Irqs[I],TRUE);if(S!=CR_SUCCESS)return S;}
 /* First snapshot follows enable; an edge during setup is captured here or
    remains pending in the actual arbiter. Unstable state is not published. */
 S=CrPmicCcService(C);if(S!=CR_SUCCESS)C->Wake(C->WakeContext);
 return CR_SUCCESS;
}
CR_STATUS CrPmicCcGet(CR_PMIC_CC *C,CR_USB_TYPEC_STATE *Out) {
 if(!C||!Out)return CR_INVALID_PARAMETER;
 if(!C->Initialized||C->Stopping)return CR_NOT_READY;
 CR_STATUS S=C->Bus->Check(C->Bus);if(S!=CR_SUCCESS)return S;
 if(!C->Valid)return C->LastStatus;
 *Out=C->State;return CR_SUCCESS;
}
CR_STATUS CrPmicCcStop(CR_PMIC_CC *C) {
 if(!C)return CR_INVALID_PARAMETER;if(!C->Initialized)return CR_SUCCESS;
 C->Stopping=TRUE;C->Valid=FALSE;CR_STATUS First=CR_SUCCESS;
 for(UINT8 I=0;I<8;I++)if(C->Irqs[I].Provider) {
  CR_STATUS S=C->Bus->Unsubscribe(C->Bus,&C->Irqs[I]);if(First==CR_SUCCESS&&S!=CR_SUCCESS)First=S;
 }
 if(First==CR_SUCCESS)C->Initialized=FALSE;return First;
}
