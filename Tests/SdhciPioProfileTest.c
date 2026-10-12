/* Inherited v5 FIFO selection and fallible checked writes. MIT */
#include <Library/SdhciMsmLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
#endif
#define MASK ((1U<<3)|(1U<<10))
typedef struct {UINT32 Value,Step,FailAt,Reads,Writes,Barriers;CR_STATUS Failure;BOOLEAN Drop,ReadOnlyAlt;} MODEL;
static CR_STATUS Step(MODEL *M){return ++M->Step==M->FailAt?M->Failure:CR_SUCCESS;}
static CR_STATUS Ready(VOID *P){return Step(P);}
static CR_STATUS Read(VOID *P,UINT32 O,UINT8 W,UINT32 *V){MODEL *M=P;assert(O==0x250&&W==4);++M->Reads;*V=M->Value;return Step(M);}
static CR_STATUS Write(VOID *P,UINT32 O,UINT8 W,UINT32 V){MODEL *M=P;assert(O==0x250&&W==4);++M->Writes;
 if(!M->Drop)M->Value=M->ReadOnlyAlt?((V&~(1U<<10))|(M->Value&(1U<<10))):V;
 return Step(M);}
static CR_STATUS Stall(VOID *P,UINT32 U){(VOID)P;(VOID)U;assert(0);return CR_UNSUPPORTED;}
static CR_STATUS Barrier(VOID *P){MODEL *M=P;++M->Barriers;return Step(M);}
static void Init(MODEL *M,CR_SDHC *C){memset(M,0,sizeof(*M));M->Value=0xa5a5ffff;M->Failure=CR_DEVICE_ERROR;
 CR_SDHC_IO I={M,Ready,Read,Write,Stall,Barrier,0x1000,CR_SDHC_WINDOW_ALL};assert(CrSdhcInit(C,&I)==CR_SUCCESS);}
int main(int argc,char **argv){(VOID)argv;MODEL M;CR_SDHC C;Init(&M,&C);UINT32 Old=M.Value;
 assert(CrSdhcPrepareSdr(&C)==CR_SUCCESS);assert(!(M.Value&MASK));
 assert((M.Value&~MASK)==(Old&~MASK));
 if(argc>1){puts("Inherited FIFO_ALT_EN clear PASS");return 0;}
 UINT32 Steps=M.Step;assert(M.Writes==1&&M.Reads==2&&M.Barriers==1);
 for(UINT32 F=1;F<=Steps;++F)for(UINT32 S=0;S<2;++S){Init(&M,&C);M.FailAt=F;M.Failure=S?CR_SUCCESS+1:CR_DEVICE_ERROR;
   assert(CrSdhcPrepareSdr(&C)==M.Failure);M.FailAt=0;assert(CrSdhcPrepareSdr(&C)==CR_SUCCESS&&!(M.Value&MASK));}
 Init(&M,&C);M.Drop=TRUE;assert(CrSdhcPrepareSdr(&C)==CR_DEVICE_ERROR&&M.Value==Old);
 Init(&M,&C);M.ReadOnlyAlt=TRUE;assert(CrSdhcPrepareSdr(&C)==CR_DEVICE_ERROR&&(M.Value&(1U<<10)));
 Init(&M,&C);C.Io.Windows=CR_SDHC_WINDOW_HOST;assert(CrSdhcPrepareSdr(&C)==CR_UNSUPPORTED&&!M.Reads&&!M.Writes);
 printf("PIO FIFO profile PASS: %u exact/nonfinal IO faults, dropped/RO bit10, neighbour preservation\n",Steps*2);return 0;}
