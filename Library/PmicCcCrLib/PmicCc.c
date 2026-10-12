/* PM8150B observer derived from Qualcomm SMB5/PD register definitions.
 * Reference Android SM8150 1dd473ab, SM8250 cad09e06, and Linux a53261b6:
 * drivers/power/supply/qcom/smb5-{lib.c,reg.h}, include/linux/qpnp/qpnp-revid.h,
 * drivers/usb/pd/qpnp-pdphy.c; Linux drivers/usb/typec/tcpm/qcom/.
 * Original Qualcomm files Copyright (c) 2017-2020, The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <Library/PmicCcCrLib.h>
#include <oskal/cr_string.h>
#include <oskal/cr_atomic.h>
STATIC UINT32 EpochSet(volatile UINT32 *Address,UINT32 Old,UINT32 New) {
#ifdef _KERNEL_MODE
 return (UINT32)InterlockedCompareExchange((volatile LONG *)Address,(LONG)New,(LONG)Old);
#else
 return InterlockedCompareExchange32(Address,Old,New);
#endif
}
#ifdef _KERNEL_MODE
#include "PmicCc.tmh"
#endif
VOID CrPmicCcInvalidate(CR_PMIC_CC *C,CR_STATUS Status) {
 if(!C)return;CrAtomicAnd32(&C->WitnessFlags,0);C->Valid=FALSE;C->LastStatus=Status;
}
VOID CrPmicCcClose(CR_PMIC_CC *C) {
 if(!C)return;CrPmicCcInvalidate(C,CR_NOT_READY);C->Stopping=TRUE;
 UINT32 Epoch=CrAtomicLoad32(&C->WitnessEpoch);if(Epoch!=~0U)EpochSet(&C->WitnessEpoch,Epoch,Epoch+1);
}
STATIC VOID Changed(VOID *X) {
 CR_PMIC_CC *C=X;if(!C->Stopping){CrAtomicAnd32(&C->WitnessFlags,0);
  UINT32 Epoch=CrAtomicLoad32(&C->WitnessEpoch);if(Epoch!=~0U)EpochSet(&C->WitnessEpoch,Epoch,Epoch+1);
  C->Dirty=TRUE;C->Valid=FALSE;C->LastStatus=CR_NOT_READY;C->Wake(C->WakeContext);}
}
STATIC CR_STATUS Sample(CR_PMIC_CC *C,UINT8 *R) {
 CR_STATUS S=C->Bus->Read(C->Bus,2,0x1506,R,6);
 if(C->Stopping)return CR_NOT_READY;
 if(S==CR_SUCCESS)S=C->Bus->Read(C->Bus,2,0x1740,R+6,1);
 if(C->Stopping)return CR_NOT_READY;
 if(S==CR_SUCCESS)S=C->Bus->Read(C->Bus,2,0x1746,R+7,1);
 if(C->Stopping)return CR_NOT_READY;
 return S;
}
CR_STATUS CrPmicCcService(CR_PMIC_CC *C) {
 UINT8 R[8],Again[8];CR_USB_TYPEC_STATE Next={0};CR_STATUS S;
 if(!C||!C->Initialized||C->Stopping)return CR_NOT_READY;
 S=C->Bus->Check(C->Bus);if(C->Stopping){S=CR_NOT_READY;goto Failure;}if(S!=CR_SUCCESS)goto Failure;
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
 UINT32 Epoch=CrAtomicLoad32(&C->WitnessEpoch);if(Epoch==~0U){S=CR_OUT_OF_RESOURCES;goto Failure;}
 CrAtomicAnd32(&C->WitnessFlags,0);EpochSet(&C->WitnessEpoch,Epoch,Epoch+1);
 UINT32 Flags=CR_TYPEC_WITNESS_VALID;
 if(Attached)Flags|=CR_TYPEC_WITNESS_ATTACHED;if(Next.PowerSource)Flags|=CR_TYPEC_WITNESS_SOURCE;
 /* Public PM8150B: TYPEC_SM_STATUS150a bit5 vSafe5V and MISC150b bit5
  * VBUS detect. Neither the OTG enable latch nor CC source alone proves VBUS. */
 if(R[4]&0x20)Flags|=CR_TYPEC_WITNESS_VSAFE5V;if(Misc&0x20)Flags|=CR_TYPEC_WITNESS_VBUS_DETECT;
 if(!(R[7]&1))Flags|=CR_TYPEC_WITNESS_PD_DISABLED;if(Next.DataRole==UsbTypeCHost)Flags|=CR_TYPEC_WITNESS_HOST;
 if(Next.Orientation==UsbTypeCReverse)Flags|=CR_TYPEC_WITNESS_REVERSE;
 C->State=Next;cr_memcpy(C->Raw,R,sizeof(R));C->Valid=TRUE;C->Dirty=FALSE;C->LastStatus=CR_SUCCESS;
 CrAtomicOr32(&C->WitnessFlags,Flags);
 return CR_SUCCESS;
Failure:
 CrPmicCcInvalidate(C,S);return S;
}
CR_STATUS CrPmicCcStart(CR_PMIC_CC *C,CONST CR_PMIC_CC_TARGET *T,
 CR_SPMI_BUS *Bus,VOID (*Wake)(VOID *),VOID *WakeContext) {
 if(!C||!T||!Bus||!Wake||!Bus->Check||!Bus->Read||!Bus->Subscribe||!Bus->Enable||!Bus->Unsubscribe)
  return CR_INVALID_PARAMETER;
 if(C->Stopping)return CR_NOT_READY;if(C->Initialized)return CR_ALREADY_STARTED;
 if((T->Board!=CR_PMIC_CC_HDK8150 && T->Board!=CR_PMIC_CC_HDK8245) ||
    T->Batteryless!=(T->Board==CR_PMIC_CC_HDK8245))return CR_UNSUPPORTED;
 UINT8 Rev[6];CR_STATUS S=Bus->Read(Bus,2,0x100,Rev,sizeof(Rev));if(C->Stopping)return CR_NOT_READY;if(S!=CR_SUCCESS)return S;
 /* Exact PM8150B type/subtype, source-documented v1/v2 revision tuples. */
 if(Rev[0]||Rev[1]||Rev[2]||(Rev[3]!=1&&Rev[3]!=2)||Rev[4]!=0x51||Rev[5]!=0x20)return CR_UNSUPPORTED;
 cr_memset(C,0,sizeof(*C));C->Bus=Bus;C->Target=*T;C->Wake=Wake;C->WakeContext=WakeContext;
 C->Initialized=TRUE;C->Dirty=TRUE;C->LastStatus=CR_NOT_READY;C->WitnessEpoch=1;cr_memcpy(C->Revision,Rev,sizeof(Rev));
 for(UINT8 I=0;I<8;I++) {
  CR_INTERRUPT_TRIGGER_TYPE Trigger=(I==1||I==3||I==6)?CR_INTERRUPT_TRIGGER_EDGE_BOTH:CR_INTERRUPT_TRIGGER_EDGE_RISING;
  S=Bus->Subscribe(Bus,2,0x15,I,Trigger,Changed,C,&C->Irqs[I]);if(C->Stopping)return CR_NOT_READY;if(S!=CR_SUCCESS)return S;
 }
 for(UINT8 I=0;I<8;I++){S=Bus->Enable(Bus,&C->Irqs[I],TRUE);if(C->Stopping)return CR_NOT_READY;if(S!=CR_SUCCESS)return S;}
 /* First snapshot follows enable; an edge during setup is captured here or
    remains pending in the actual arbiter. Unstable state is not published. */
 S=CrPmicCcService(C);if(C->Stopping)return CR_NOT_READY;if(S!=CR_SUCCESS)C->Wake(C->WakeContext);
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
 if(!C)return CR_INVALID_PARAMETER;CrPmicCcClose(C);if(!C->Initialized)return CR_SUCCESS;
 if(C->StopBusy)return CR_BUSY;
 UINT32 Epoch=CrAtomicLoad32(&C->WitnessEpoch);if(Epoch==~0U)return CR_NOT_READY;
 C->StopBusy=TRUE;CR_STATUS First=CR_SUCCESS;
 for(UINT8 I=0;I<8;I++)if(C->Irqs[I].Provider) {
  CR_STATUS S=C->Bus->Unsubscribe(C->Bus,&C->Irqs[I]);if(First==CR_SUCCESS&&S!=CR_SUCCESS)First=S;
  if(Epoch!=CrAtomicLoad32(&C->WitnessEpoch)){First=CR_NOT_READY;break;}
 }
 if(First==CR_SUCCESS)C->Initialized=FALSE;C->StopBusy=FALSE;return First;
}
CR_STATUS CrPmicCcHostWitness(CR_PMIC_CC *C,CR_TYPEC_NON_PD_HOST_WITNESS *Out) {
 if(!C||!Out)return CR_INVALID_PARAMETER;
 if(!C->Initialized||C->Stopping||!C->Bus||C->Bus->Check(C->Bus)!=CR_SUCCESS)return CR_NOT_READY;
 UINT32 Epoch=CrAtomicLoad32(&C->WitnessEpoch),Flags=CrAtomicLoad32(&C->WitnessFlags);
 CR_TYPEC_NON_PD_HOST_WITNESS W={CR_TYPEC_NON_PD_WITNESS_REVISION,C->Target.Board,Epoch,Flags};
 if(Epoch!=CrAtomicLoad32(&C->WitnessEpoch)||Flags!=CrAtomicLoad32(&C->WitnessFlags))return CR_NOT_READY;
 *Out=W;return Epoch!=~0U&&(Flags&CR_TYPEC_WITNESS_REQUIRED)==CR_TYPEC_WITNESS_REQUIRED?CR_SUCCESS:CR_NOT_READY;
}
