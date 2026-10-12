/* Actual MSM command engine against CRC cleanup timing and failed IO. MIT */
#include <Library/SdhciMsmLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
#endif
typedef struct {
  UINT8 Registers[CR_SDHC_POWER_END];
  UINT32 Now, CleanAt, Commands, Stalls, Clears, InjectAfterStatus, InjectOnClear;
  BOOLEAN Cleanup, FailCommand, LoseAccess;
  CR_STATUS ReadyStatus, StallStatus;
} MODEL;
static UINT32 Get(MODEL *M,UINT32 O,UINT8 W){UINT32 V=0;memcpy(&V,M->Registers+O,W);return V;}
static void Put(MODEL *M,UINT32 O,UINT8 W,UINT32 V){memcpy(M->Registers+O,&V,W);}
static CR_STATUS Ready(VOID *P){return ((MODEL *)P)->ReadyStatus;}
static CR_STATUS Read(VOID *P,UINT32 O,UINT8 W,UINT32 *V){
  MODEL *M=P;*V=Get(M,O,W);
  if(O==0x30&&M->InjectAfterStatus){Put(M,0x30,4,Get(M,0x30,4)|M->InjectAfterStatus);M->InjectAfterStatus=0;}
  return CR_SUCCESS;
}
static CR_STATUS Write(VOID *P,UINT32 O,UINT8 W,UINT32 V){
  MODEL *M=P;
  if(O==0x34||O==0x38)V&=0x7fffU; /* Bit15 is fixed-zero. */
  if(O==0x30||O==0x32){
    ++M->Clears;Put(M,0x30,4,Get(M,0x30,4)|M->InjectOnClear);M->InjectOnClear=0;
    Put(M,O,W,Get(M,O,W)&~V);
    Put(M,0x30,2,(Get(M,0x30,2)&0x7fffU)|((Get(M,0x32,2)&0x1ffU)?0x8000U:0));return CR_SUCCESS;
  }
  if(O==0x2f){
    if(V&3)M->Cleanup=FALSE;
    Put(M,O,W,0);return CR_SUCCESS;
  }
  Put(M,O,W,V);
  if(O==0xe){
    ++M->Commands;
    /* Three 400kHz MCLKs after a no-CRC response. An early CRC command
     * observes the previous CRC-engine error instead of clean completion. */
    BOOLEAN Early=M->Cleanup&&M->Now<M->CleanAt&&(V&8);
    Put(M,0x30,2,Early?0x8000:1);Put(M,0x32,2,Early?2:0);
    M->Cleanup=(V&3)&&!(V&8);M->CleanAt=M->Now+8;
    if(M->FailCommand)return CR_DEVICE_ERROR;
  }
  return CR_SUCCESS;
}
static CR_STATUS Stall(VOID *P,UINT32 Us){
  MODEL *M=P;++M->Stalls;
  if(M->StallStatus!=CR_SUCCESS)return M->StallStatus;
  M->Now+=Us;if(M->LoseAccess)M->ReadyStatus=CR_NOT_READY;return CR_SUCCESS;
}
static CR_STATUS Barrier(VOID *P){(VOID)P;return CR_SUCCESS;}
static void Init(CR_SDHC *C,MODEL *M){
  memset(M,0,sizeof(*M));CR_SDHC_IO Io={M,Ready,Read,Write,Stall,Barrier,sizeof(M->Registers),CR_SDHC_WINDOW_ALL};
  assert(CrSdhcInit(C,&Io)==CR_SUCCESS);assert(CrSdhcEvents(C,0x8001,TRUE)==CR_SUCCESS);
}
static CR_STATUS Issue(CR_SDHC *C,UINT8 Index,CR_SDHC_RESPONSE Response){
  CR_SDHC_COMMAND D={0};D.Index=Index;D.Response=Response;UINT32 Required=0;
  return CrSdhcIssue(C,&D,&Required);
}
static void Complete(CR_SDHC *C){
  UINT32 Events=0,Errors=0;assert(CrSdhcInterrupt(C,&Events,&Errors)==CR_SUCCESS);
  assert(Events==CR_SDHC_EVENT_COMMAND&&Errors==0);
}
int main(void){
  CR_SDHC C;MODEL M;
  /* A cause arriving after the status snapshot must survive acknowledgement.
   * This fails a split normal/error read and a blanket pre-command W1C. */
  Init(&C,&M);Put(&M,0x30,4,0x00018000);M.InjectAfterStatus=0x00020000;
  UINT32 E,R;assert(CrSdhcInterrupt(&C,&E,&R)==CR_SUCCESS);
  assert(E==0x8000&&R==1&&Get(&M,0x30,4)==0x00028000);
  Init(&C,&M);Put(&M,0x30,4,1);M.InjectOnClear=0x20;
  assert(CrSdhcClearEvents(&C,0xffff)==CR_SUCCESS&&Get(&M,0x30,4)==0x20&&M.Clears==1);
  Init(&C,&M);assert(CrSdhcClearEvents(&C,0xffff)==CR_SUCCESS&&!M.Clears);
  /* Summary enable bit15 cannot be used to mask the hardware error.
   * An unrelated masked error must remain pending and not claim an IRQ. */
  Init(&C,&M);Put(&M,0x30,2,0x8000);Put(&M,0x32,2,3);Put(&M,0x3a,2,1);
  UINT32 Events,Errors;assert(CrSdhcInterrupt(&C,&Events,&Errors)==CR_SUCCESS);
  assert(Events==0x8000&&Errors==1&&C.NeedsReset&&Get(&M,0x32,2)==2);
  Put(&M,0x30,2,0x8000);C.NeedsReset=FALSE;
  assert(CrSdhcInterrupt(&C,&Events,&Errors)==CR_SUCCESS&&!Events&&!Errors&&!C.NeedsReset);
  Put(&M,0x30,2,0x8001);Put(&M,0x32,2,0x100);Put(&M,0x3a,2,0x100);Put(&M,0x3c,2,0x82);
  assert(CrSdhcInterrupt(&C,&Events,&Errors)==CR_SUCCESS&&Events==0x8001&&Errors==0x820100);
  /* OCR polling (R3) followed by APP_CMD (R1), or CID (R2). */
  for(UINT32 R=CrSdResponse48;R<=CrSdResponse136;++R){
    Init(&C,&M);assert(Issue(&C,41,CrSdResponse48NoCrc)==CR_SUCCESS);Complete(&C);
    assert(Issue(&C,R==CrSdResponse136?2:55,(CR_SDHC_RESPONSE)R)==CR_SUCCESS);Complete(&C);
    assert(M.Now>=8&&M.Stalls==1&&M.Commands==2);
  }
  Init(&C,&M);assert(Issue(&C,0,CrSdResponseNone)==CR_SUCCESS);Complete(&C);
  assert(Issue(&C,8,CrSdResponse48)==CR_SUCCESS);Complete(&C);
  assert(Issue(&C,55,CrSdResponse48)==CR_SUCCESS);Complete(&C);assert(!M.Stalls);
  assert(Issue(&C,41,CrSdResponse48NoCrc)==CR_SUCCESS);Complete(&C);
  assert(Issue(&C,41,CrSdResponse48NoCrc)==CR_SUCCESS);Complete(&C);assert(!M.Stalls);
  /* A failed/pending delay cannot launch the command or consume history. */
  CR_STATUS Failure[]={CR_DEVICE_ERROR,(CR_STATUS)1};
  for(UINT32 I=0;I<2;++I){
    Init(&C,&M);assert(Issue(&C,41,CrSdResponse48NoCrc)==CR_SUCCESS);Complete(&C);
    M.StallStatus=Failure[I];assert(Issue(&C,55,CrSdResponse48)==Failure[I]);
    assert(M.Commands==1&&!C.NeedsReset);
    M.StallStatus=CR_SUCCESS;assert(Issue(&C,55,CrSdResponse48)==CR_SUCCESS);Complete(&C);
    assert(M.Commands==2&&M.Now>=8);
  }
  Init(&C,&M);assert(Issue(&C,41,CrSdResponse48NoCrc)==CR_SUCCESS);Complete(&C);
  M.LoseAccess=TRUE;assert(Issue(&C,55,CrSdResponse48)==CR_NOT_READY&&M.Commands==1);
  /* Failed command writes are uncertain and must be reset before retry. */
  Init(&C,&M);M.FailCommand=TRUE;assert(Issue(&C,41,CrSdResponse48NoCrc)==CR_DEVICE_ERROR&&C.NeedsReset);
  M.FailCommand=FALSE;assert(Issue(&C,55,CrSdResponse48)==CR_NOT_READY&&M.Commands==1);
  assert(CrSdhcReset(&C,2)==CR_SUCCESS);assert(Issue(&C,55,CrSdResponse48)==CR_SUCCESS);Complete(&C);assert(!M.Stalls);
  /* Data-only reset does not reset the command CRC engine. */
  Init(&C,&M);assert(Issue(&C,41,CrSdResponse48NoCrc)==CR_SUCCESS);Complete(&C);
  assert(CrSdhcReset(&C,4)==CR_SUCCESS);assert(Issue(&C,55,CrSdResponse48)==CR_SUCCESS);Complete(&C);assert(M.Stalls==1);
  puts("SDHCI command timing: R3 CRC cleanup, response variants, failed delay/access/commit and reset boundaries PASS");
  return 0;
}
