/* Actual PM8150B core behind the native primary protocol boundary. */
#include <Library/PmicCcCrLib.h>
static CR_PMIC_CC Cc;
static UINT8 CcRaw[8];static UINT32 CcReads;
static CR_INTERRUPT_HANDLER CcIrq;static VOID *CcIrqContext;
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *A,UINT32 Compare,UINT32 New){return __sync_val_compare_and_swap(A,Compare,New);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI SetMem(VOID *A,UINTN N,UINT8 V){return memset(A,V,N);}
static CR_STATUS CcCheck(VOID *X){(VOID)X;return CR_SUCCESS;}
static CR_STATUS CcRead(VOID *X,UINT8 Sid,UINT16 A,UINT8 *D,UINTN N){
 (VOID)X;assert(Sid==2);++CcReads;
 if(A==0x100){assert(N==6);UINT8 V[]={0,0,0,2,0x51,0x20};memcpy(D,V,6);}
 else if(A==0x1506){assert(N==6);memcpy(D,CcRaw,6);}
 else if(A==0x1740){assert(N==1);*D=CcRaw[6];}else{assert(A==0x1746&&N==1);*D=CcRaw[7];}return CR_SUCCESS;
}
static CR_STATUS CcSubscribe(VOID *X,UINT8 S,UINT8 P,UINT8 B,CR_INTERRUPT_TRIGGER_TYPE T,CR_INTERRUPT_HANDLER H,VOID *Ctx,CR_SPMI_IRQ *Token){
 (VOID)X;(VOID)T;assert(S==2&&P==0x15&&B<8);Token->Provider=(VOID *)1;Token->Generation=B+1;CcIrq=H;CcIrqContext=Ctx;return CR_SUCCESS;
}
static CR_STATUS CcEnable(VOID *X,CR_SPMI_IRQ *T,BOOLEAN On){(VOID)X;assert(T->Provider&&On);return CR_SUCCESS;}
static CR_STATUS CcUnsubscribe(VOID *X,CR_SPMI_IRQ *T){(VOID)X;memset(T,0,sizeof *T);return CR_SUCCESS;}
static VOID CcWake(VOID *X){(VOID)X;}
static CR_SPMI_BUS CcBus={.Check=CcCheck,.Read=CcRead,.Subscribe=CcSubscribe,.Enable=CcEnable,.Unsubscribe=CcUnsubscribe};
static VOID CcFresh(VOID){
 memset(&Cc,0,sizeof Cc);memset(CcRaw,0,sizeof CcRaw);CcRaw[2]=8;CcRaw[3]=0x20;CcRaw[4]=0x20;CcRaw[5]=0x69;
 CR_PMIC_CC_TARGET T={8245,TRUE};assert(CrPmicCcStart(&Cc,&T,&CcBus,CcWake,NULL)==CR_SUCCESS);
}
static VOID CcSample(VOID){CcIrq(CcIrqContext);assert(CrPmicCcService(&Cc)==CR_SUCCESS);}
