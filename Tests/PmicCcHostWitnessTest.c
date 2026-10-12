/* Actual PM8150B samples: CC/source is insufficient without VBUS/PD proof.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/PmicCcCrLib.h>
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI SetMem(VOID *A,UINTN N,UINT8 V){return memset(A,V,N);}
static UINT8 Raw[8];static UINT32 Reads,Wakes;static BOOLEAN Online=TRUE;
static CR_INTERRUPT_HANDLER Irq;static VOID *IrqContext;
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *A,UINT32 Compare,UINT32 New){return __sync_val_compare_and_swap(A,Compare,New);}
static CR_STATUS Check(VOID *X){(VOID)X;return Online?CR_SUCCESS:CR_NOT_READY;}
static CR_STATUS Read(VOID *X,UINT8 Sid,UINT16 Address,UINT8 *Data,UINTN Bytes){
 (VOID)X;assert(Sid==2);++Reads;
 if(Address==0x100){assert(Bytes==6);UINT8 Rev[]={0,0,0,2,0x51,0x20};memcpy(Data,Rev,6);}
 else if(Address==0x1506){assert(Bytes==6);memcpy(Data,Raw,6);}
 else if(Address==0x1740){assert(Bytes==1);*Data=Raw[6];}
 else {assert(Address==0x1746&&Bytes==1);*Data=Raw[7];}return CR_SUCCESS;
}
static CR_STATUS Subscribe(VOID *X,UINT8 S,UINT8 P,UINT8 B,CR_INTERRUPT_TRIGGER_TYPE T,CR_INTERRUPT_HANDLER H,VOID *C,CR_SPMI_IRQ *Token){
 (VOID)X;(VOID)T;assert(S==2&&P==0x15&&B<8);Token->Provider=(VOID *)1;Token->Generation=B+1;Irq=H;IrqContext=C;return CR_SUCCESS;
}
static CR_STATUS Enable(VOID *X,CR_SPMI_IRQ *T,BOOLEAN On){(VOID)X;assert(T->Provider&&On);return CR_SUCCESS;}
static CR_STATUS Unsubscribe(VOID *X,CR_SPMI_IRQ *T){(VOID)X;memset(T,0,sizeof *T);return CR_SUCCESS;}
static VOID Wake(VOID *X){(VOID)X;++Wakes;}
static CR_SPMI_BUS Bus={.Check=Check,.Read=Read,.Subscribe=Subscribe,.Enable=Enable,.Unsubscribe=Unsubscribe};
int main(void){
 CR_PMIC_CC Port={0};CR_PMIC_CC_TARGET Target={8245,TRUE};Raw[2]=8;Raw[3]=0x20;Raw[4]=0x20;Raw[5]=0x69;
 assert(CrPmicCcStart(&Port,&Target,&Bus,Wake,NULL)==CR_SUCCESS);CR_TYPEC_NON_PD_HOST_WITNESS W={0};
 UINT32 Before=Reads;assert(CrPmicCcHostWitness(&Port,&W)==CR_SUCCESS&&Reads==Before);
 assert(W.Board==8245&&(W.Flags&127)==127&&!Port.State.PdReady&&Port.State.DataRole==UsbTypeCHost);
 UINT32 Epoch=W.Epoch;Irq(IrqContext);assert(Wakes==1&&CrPmicCcHostWitness(&Port,&W)==CR_NOT_READY&&Reads==Before);
 assert(CrPmicCcService(&Port)==CR_SUCCESS&&CrPmicCcHostWitness(&Port,&W)==CR_SUCCESS&&W.Epoch!=Epoch);
 for(UINT32 Case=0;Case<5;Case++){
  Raw[2]=8;Raw[3]=0x20;Raw[4]=0x20;Raw[5]=0x69;Raw[7]=0;
  if(Case==0)Raw[4]=0;if(Case==1)Raw[5]&=~0x20;if(Case==2)Raw[7]=1;
  if(Case==3){Raw[2]=0;Raw[5]&=~0x40;Raw[0]=8;}if(Case==4){Raw[3]=0;Raw[5]=0;}
  Irq(IrqContext);assert(CrPmicCcService(&Port)==CR_SUCCESS);Before=Reads;
  assert(CrPmicCcHostWitness(&Port,&W)==CR_NOT_READY&&Reads==Before&&!Port.State.PdReady);
 }
 Online=FALSE;assert(CrPmicCcHostWitness(&Port,&W)==CR_NOT_READY);Online=TRUE;
 assert(CrPmicCcStop(&Port)==CR_SUCCESS&&CrPmicCcHostWitness(&Port,&W)==CR_NOT_READY);
 puts("PM8150B actual CC sample: cached atomic epochs, VBUSdetect+vSafe5V+nonPDsource, IRQ invalidation, no fabricated PD-ready PASS");return 0;
}
