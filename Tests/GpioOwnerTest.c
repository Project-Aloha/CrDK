/* Actual portable absolute-owner bank and PEP GPIO journal failures. MIT */
#include <Library/PepGpioLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _KERNEL_MODE
KIRQL KeGetCurrentIrql(VOID){return PASSIVE_LEVEL;}
#else
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
static CR_GPIO_OWNER_BANK Bank;
static CR_GPIO_OWNER_RESOURCE Resource={.Key={CrGpioOwnerTlmm,0,105},.Baseline={0,0,0,0,1,0,0,0,0,1}};
static CR_GPIO_OWNER_ENTRY Entries[8];
static CR_GPIO_OWNER_CONFIG Hardware;
static BOOLEAN Claimed,ClassOwner,Offline;
static UINT32 Writes,Claims,FailClaim,FailUnclaim,FailWrite;
static int OwnerA,OwnerB;
static CR_STATUS Validate(VOID *C,CONST CR_GPIO_OWNER_KEY *K,CONST CR_GPIO_OWNER_CONFIG *V){(void)C;if(Offline)return CR_NOT_READY;if(K->Pin!=105||V->Output>1||V->Direction>1||V->Enabled!=1)return CR_INVALID_PARAMETER;return CR_SUCCESS;}
static CR_STATUS Claim(VOID *C,CONST CR_GPIO_OWNER_KEY *K){(void)C;(void)K;++Claims;if(ClassOwner)return CR_BUSY;Claimed=TRUE;if(FailClaim){--FailClaim;return CR_DEVICE_ERROR;}return CR_SUCCESS;}
static CR_STATUS Unclaim(VOID *C,CONST CR_GPIO_OWNER_KEY *K){(void)C;(void)K;if(Offline)return CR_NOT_READY;if(FailUnclaim){--FailUnclaim;return CR_DEVICE_ERROR;}Claimed=FALSE;return CR_SUCCESS;}
static CR_STATUS Write(VOID *C,CONST CR_GPIO_OWNER_KEY *K,CONST CR_GPIO_OWNER_CONFIG *V){(void)C;(void)K;assert(Claimed&&!ClassOwner);++Writes;Hardware=*V;if(FailWrite){--FailWrite;return CR_DEVICE_ERROR;}return CR_SUCCESS;}
static CR_STATUS Get(VOID *C,VOID *O,CONST CR_GPIO_OWNER_KEY *K,CR_GPIO_OWNER_VALUE *V,BOOLEAN *P){return CrGpioOwnerGet(C,O,K,V,P);}
static CR_STATUS Set(VOID *C,VOID *O,CONST CR_GPIO_OWNER_KEY *K,CONST CR_GPIO_OWNER_VALUE *V){return CrGpioOwnerSet(C,O,K,V);}
static CR_STATUS PValidate(VOID *C,CONST CR_GPIO_OWNER_KEY *K,CONST CR_GPIO_OWNER_CONFIG *V){return CrGpioOwnerValidate(C,K,V);}
int main(void){
  memset(Entries,0xa5,sizeof(Entries));for(UINT32 I=0;I<8;++I)Entries[I].Owner=NULL;
  CR_GPIO_OWNER_IO Io={NULL,Validate,Claim,Unclaim,Write};Hardware=Resource.Baseline;
  assert(CrGpioOwnerInit(&Bank,&Io,&Resource,1,Entries,8)==CR_SUCCESS);
  FailClaim=1;assert(CrGpioOwnerReserve(&Bank,&OwnerA,&Resource.Key)==CR_DEVICE_ERROR&&Claimed&&Entries[0].Owner==&OwnerA);
  assert(Entries[0].Value.Present==FALSE&&Entries[0].Closing==FALSE);
  assert(CrGpioOwnerReserve(&Bank,&OwnerA,&Resource.Key)==CR_SUCCESS&&Claims==2&&!Writes);
  assert(CrGpioOwnerReserve(&Bank,&OwnerB,&Resource.Key)==CR_SUCCESS&&Claims==2);
  CR_GPIO_OWNER_VALUE A={.Present=TRUE,.Config={0,1,3,2,0,0,0,0,0,1}},B=A,Value;BOOLEAN Pending;
  assert(CrGpioOwnerSet(&Bank,&OwnerA,&Resource.Key,&A)==CR_SUCCESS);
  B.Config.Output=1;UINT32 Before=Writes;
  assert(CrGpioOwnerSet(&Bank,&OwnerB,&Resource.Key,&B)==CR_BUSY&&Writes==Before);
  assert(CrGpioOwnerSet(&Bank,&OwnerB,&Resource.Key,&A)==CR_SUCCESS);
  assert(CrGpioOwnerRelease(&Bank,&OwnerA)==CR_SUCCESS&&Claimed&&!memcmp(&Hardware,&A.Config,sizeof(Hardware)));
  FailWrite=1;assert(CrGpioOwnerRelease(&Bank,&OwnerB)==CR_DEVICE_ERROR&&Claimed);
  assert(CrGpioOwnerGet(&Bank,&OwnerB,&Resource.Key,&Value,&Pending)==CR_SUCCESS&&Value.Present&&Pending);
  assert(CrGpioOwnerUninit(&Bank)==CR_BUSY);
  assert(CrGpioOwnerReserve(&Bank,&OwnerA,&Resource.Key)==CR_SUCCESS);
  Before=Writes;assert(CrGpioOwnerRelease(&Bank,&OwnerA)==CR_SUCCESS&&Writes==Before&&Pending);
  Offline=TRUE;assert(CrGpioOwnerRelease(&Bank,&OwnerB)==CR_NOT_READY&&Claimed);
  Offline=FALSE;FailUnclaim=1;assert(CrGpioOwnerRelease(&Bank,&OwnerB)==CR_DEVICE_ERROR&&Claimed);
  assert(CrGpioOwnerGet(&Bank,&OwnerB,&Resource.Key,&Value,&Pending)==CR_SUCCESS&&!Value.Present&&Pending);
  assert(CrGpioOwnerReserve(&Bank,&OwnerB,&Resource.Key)==CR_NOT_READY);
  assert(CrGpioOwnerRelease(&Bank,&OwnerB)==CR_SUCCESS&&!Claimed&&!memcmp(&Hardware,&Resource.Baseline,sizeof(Hardware)));
  ClassOwner=TRUE;assert(CrGpioOwnerReserve(&Bank,&OwnerA,&Resource.Key)==CR_BUSY&&!Claimed);assert(CrGpioOwnerRelease(&Bank,&OwnerA)==CR_SUCCESS&&ClassOwner);ClassOwner=FALSE;
  assert(CrGpioOwnerReserve(&Bank,&OwnerA,&Resource.Key)==CR_SUCCESS);
  CR_PEP_GPIO_PROFILE Profile={.Kind=CrPepTlmmGpio,.Arguments={105,1,0,1,3,3},.ArgumentCount=6,.Config=A.Config};
  CR_PEP_GPIO_BINDING Binding={Resource.Key,&Profile,1};CR_PEP_GPIO_IO Pio={&Bank,PValidate,Get,Set};CR_PEP_GPIO_PROVIDER Provider={0};
  assert(CrPepGpioProviderInit(&Provider,&Pio,&Binding,1)==CR_SUCCESS);
  CR_PEP_RESOURCE_OP Op={.Kind=CrPepTlmmGpio,.Arguments={105,1,0,1,3,3},.ArgumentCount=6};
  CR_PEP_JOURNAL_PROVIDER J;assert(CrPepGpioProviderGetJournal(&Provider,&J)==CR_SUCCESS);UINT64 R;
  assert(J.Resolve(J.Context,&OwnerA,&Op,&R)==CR_SUCCESS&&R==1);
  Op.Arguments[0]|=1ULL<<32;assert(J.Resolve(J.Context,&OwnerA,&Op,&R)==CR_UNSUPPORTED);Op.Arguments[0]=105;
  CR_PEP_JOURNAL_VALUE Saved;assert(J.Capture(J.Context,&OwnerA,R,&Saved)==CR_SUCCESS&&!Saved.Words[0]);
  assert(J.Apply(J.Context,&OwnerA,R,&Op)==CR_SUCCESS);
  Saved.Words[7]=1;assert(J.Restore(J.Context,&OwnerA,R,&Saved)==CR_INVALID_PARAMETER);Saved.Words[7]=0;
  assert(J.Restore(J.Context,&OwnerA,R,&Saved)==CR_SUCCESS);
  assert(CrPepGpioProviderUninit(&Provider)==CR_SUCCESS);
  assert(CrGpioOwnerDrain(&Bank)==CR_SUCCESS&&CrGpioOwnerUninit(&Bank)==CR_SUCCESS);
  puts("GPIO owner bank + actual PEP tuple journal: class exclusion, compatible foreign owners, retained claim/write/unclaim, full-width tuple and exact restore passed");return 0;
}
