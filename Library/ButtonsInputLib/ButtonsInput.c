/* Physical button press/release states; target facts remain outside OS glue.
   PMIC gen3 subtype9 status bits6/7 match the native HDK button source. MIT */
#include <Library/ButtonsInputLib.h>
#include <oskal/cr_string.h>
#ifndef _KERNEL_MODE
#include <Library/SynchronizationLib.h>
#endif
STATIC UINT32 Exchange(volatile UINT32 *P,UINT32 V) {
#ifdef _KERNEL_MODE
 return (UINT32)InterlockedExchange((volatile LONG *)P,(LONG)V);
#else
 UINT32 Old;do {Old=*P;}while(InterlockedCompareExchange32(P,Old,V)!=Old);return Old;
#endif
}
STATIC VOID Irq(VOID *Context) {
 CR_BUTTON_INPUT *B=Context;
 /* Registration owns B until successful unregister/drain. Wake decides
    whether OS work may be queued; Stop's caller closes that queue first. */
 (VOID)Exchange(&B->Dirty,1);B->Io.Wake(B->Io.Context);
}
STATIC CR_STATUS Pressed(CR_BUTTON_INPUT *B,UINT32 I,BOOLEAN *Value) {
 CONST ButtonTarget *T=&B->Target->Buttons[I];CR_STATUS S;
 if(T->Source==BUTTON_SOURCE_PON) {
  UINT8 V;S=B->Io.Read(B->Io.Context,T->Sid,(UINT16)((T->Peripheral<<8)+0x10),&V,1);
  if(!CR_ERROR(S))*Value=(V&(1U<<T->Irq))!=0;
 } else S=B->Io.ReadGpio(B->Io.Context,B,T->Pin,Value);
 if(!CR_ERROR(S)&&T->ActiveLow)*Value=!*Value;return S;
}
CR_STATUS CrButtonInputInit(CR_BUTTON_INPUT *B,CONST ButtonsTargetContext *T,CONST CR_BUTTON_INPUT_IO *Io) {
 if(!B||!T||!T->Buttons||T->ButtonCount!=BUTTON_CODE_MAX||!Io||!Io->Context||!Io->Read||!Io->ReadGpio||
    !Io->AcquireGpio||!Io->ReleaseGpio||!Io->Register||!Io->Enable||!Io->Unregister||!Io->Wake||!Io->Submit)return CR_INVALID_PARAMETER;
 if(B->Initialized||B->GpioAttempted)return CR_BUSY;
 UINT32 Codes=0,Gpios=0;
 for(UINT32 I=0;I<T->ButtonCount;++I) {
  CONST ButtonTarget *K=&T->Buttons[I];
  if((UINT32)K->Code>=BUTTON_CODE_MAX||K->Sid>15||K->Irq>=8||!K->DebounceUs||K->DebounceUs>1000000||
     K->Trigger!=CR_INTERRUPT_TRIGGER_EDGE_BOTH||(Codes&(1U<<K->Code)))return CR_INVALID_PARAMETER;
  Codes|=1U<<K->Code;
  if(K->Source==BUTTON_SOURCE_PON){if((K->Irq!=6&&K->Irq!=7)||K->ActiveLow||K->Pin)return CR_UNSUPPORTED;}
  else if(K->Source==BUTTON_SOURCE_PMIC_GPIO){if(!K->Pin||!K->ActiveLow||++Gpios>1)return CR_UNSUPPORTED;}
  else return CR_UNSUPPORTED;
 }
 if(Gpios!=1)return CR_UNSUPPORTED;cr_memset(B,0,sizeof(*B));B->Io=*Io;B->Target=T;return CR_SUCCESS;
}
CR_STATUS CrButtonInputStart(CR_BUTTON_INPUT *B,UINT64 Now) {
 if(!B||!B->Target)return CR_INVALID_PARAMETER;if(B->Initialized||B->GpioAttempted)return CR_BUSY;
 /* Do not steal live native/foreign PMIC IRQs. The provider owns its PIC
    state and unregister leaves our exact source masked. No raw PIC writes. */
 for(UINT32 I=0;I<B->Target->ButtonCount;++I) {
  CONST ButtonTarget *T=&B->Target->Buttons[I];UINT8 V;
  CR_STATUS S=B->Io.Read(B->Io.Context,T->Sid,(UINT16)((T->Peripheral<<8)+0x15),&V,1);if(CR_ERROR(S))return S;
  if(V&(1U<<T->Irq))return CR_NOT_READY;
  if(T->Source==BUTTON_SOURCE_PON) {
   S=B->Io.Read(B->Io.Context,T->Sid,(UINT16)((T->Peripheral<<8)+5),&V,1);if(CR_ERROR(S))return S;
   if(V!=9)return CR_UNSUPPORTED;
  }
 }
 B->Initialized=TRUE;B->Stopping=FALSE;B->Reported=0;B->ReportPending=TRUE;
 (VOID)Exchange(&B->Dirty,0);cr_memset(B->State,0,sizeof(B->State));
 CR_STATUS S=CR_SUCCESS;
 for(UINT32 I=0;I<B->Target->ButtonCount;++I) {
  CONST ButtonTarget *T=&B->Target->Buttons[I];
  if(T->Source==BUTTON_SOURCE_PMIC_GPIO){B->GpioAttempted=TRUE;S=B->Io.AcquireGpio(B->Io.Context,B,T->Pin);if(CR_ERROR(S))goto Failed;}
 }
 for(UINT32 I=0;I<B->Target->ButtonCount;++I) {
  BOOLEAN V;S=Pressed(B,I,&V);if(CR_ERROR(S))goto Failed;
  B->State[I].Stable=B->State[I].Candidate=V;B->State[I].Armed=!V;B->State[I].Since=Now;
  S=B->Io.Register(B->Io.Context,&B->Target->Buttons[I],Irq,B);if(CR_ERROR(S))goto Failed;B->State[I].Registered=TRUE;
 }
 for(UINT32 I=0;I<B->Target->ButtonCount;++I){S=B->Io.Enable(B->Io.Context,&B->Target->Buttons[I],TRUE);if(CR_ERROR(S))goto Failed;}
 (VOID)Exchange(&B->Dirty,1);return CR_SUCCESS;
 Failed:B->Stopping=TRUE;return S;
}
CR_STATUS CrButtonInputService(CR_BUTTON_INPUT *B,UINT64 Now) {
 if(!B||!B->Initialized||B->Stopping)return CR_NOT_READY;
 if(!CrButtonInputNextUs(B,Now))return CR_SUCCESS;
 (VOID)Exchange(&B->Dirty,0);
 UINT8 Mask=0;
 for(UINT32 I=0;I<B->Target->ButtonCount;++I) {
  CR_BUTTON_INPUT_STATE *S=&B->State[I];BOOLEAN V;CR_STATUS Status=Pressed(B,I,&V);
  if(CR_ERROR(Status)){(VOID)Exchange(&B->Dirty,1);return Status;}
  if(V!=S->Candidate){S->Candidate=V;S->Since=Now;}
  if(S->Candidate!=S->Stable&&Now>=S->Since&&Now-S->Since>=B->Target->Buttons[I].DebounceUs) {
   S->Stable=S->Candidate;if(!S->Stable)S->Armed=TRUE;
  }
  if(S->Stable&&S->Armed)Mask|=(UINT8)(1U<<B->Target->Buttons[I].Code);
 }
 if(B->ReportPending||Mask!=B->Reported) {
  B->ReportPending=TRUE;CR_STATUS S=B->Io.Submit(B->Io.Context,Mask);if(CR_ERROR(S))return S;
  B->Reported=Mask;B->ReportPending=FALSE;
 }
 return CR_SUCCESS;
}
UINT32 CrButtonInputNextUs(CONST CR_BUTTON_INPUT *B,UINT64 Now) {
 if(!B||!B->Initialized||B->Stopping)return 0;if(B->Dirty||B->ReportPending)return 1;
 UINT32 Next=0;
 for(UINT32 I=0;I<B->Target->ButtonCount;++I)if(B->State[I].Stable!=B->State[I].Candidate) {
  UINT64 Since=B->State[I].Since;UINT32 Debounce=B->Target->Buttons[I].DebounceUs;
  UINT32 Left=Now<Since?Debounce:Now-Since>=Debounce?1:Debounce-(UINT32)(Now-Since);
  if(!Next||Left<Next)Next=Left;
 }
 return Next;
}
CR_STATUS CrButtonInputStop(CR_BUTTON_INPUT *B) {
 if(!B||!B->Target)return CR_INVALID_PARAMETER;if(!B->Initialized&&!B->GpioAttempted)return CR_SUCCESS;
 B->Stopping=TRUE;CR_STATUS First=CR_SUCCESS;
 for(UINT32 I=0;I<B->Target->ButtonCount;++I)if(B->State[I].Registered) {
  CR_STATUS S=B->Io.Enable(B->Io.Context,&B->Target->Buttons[I],FALSE);
  if(!CR_ERROR(S))S=B->Io.Unregister(B->Io.Context,&B->Target->Buttons[I]);
  if(CR_ERROR(S)){if(!CR_ERROR(First))First=S;}else B->State[I].Registered=FALSE;
 }
 if(CR_ERROR(First))return First; /* no GPIO restoration while callbacks live */
 if(B->GpioAttempted)for(UINT32 I=0;I<B->Target->ButtonCount;++I)if(B->Target->Buttons[I].Source==BUTTON_SOURCE_PMIC_GPIO) {
  CR_STATUS S=B->Io.ReleaseGpio(B->Io.Context,B,B->Target->Buttons[I].Pin);if(CR_ERROR(S))return S;B->GpioAttempted=FALSE;
 }
 B->Initialized=FALSE;B->ReportPending=FALSE;(VOID)Exchange(&B->Dirty,0);return CR_SUCCESS;
}
