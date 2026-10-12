/* Actual NPA decode/provider/journal against a fallible owner backend. MIT */
#include <Library/PepNpaLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "NpaFixture.h"
#ifdef _KERNEL_MODE
KIRQL KeGetCurrentIrql(VOID) { return PASSIVE_LEVEL; }
#else
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
VOID *EFIAPI SetMem(VOID *Buffer,UINTN Length,UINT8 Value) { return memset(Buffer,Value,Length); }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) { return strcmp(A,B); }
#endif
static int Owner, Foreign;
typedef struct {
  CR_PEP_NPA_VALUE Values[2][2];
  UINT64 LastLevel;
  UINT32 Resolves, Gets, Sets, FailSet;
  BOOLEAN Pending, Offline, Unreserved, XoAux, Corrupt;
} BACKEND;
static UINT32 Index(CONST CR_PEP_NPA_BINDING *Binding) {
  if (strcmp(Binding->HardwareName,"cx.lvl")==0) return 0;
  assert(strcmp(Binding->HardwareName,"xo.lvl")==0); return 1;
}
static CR_STATUS ResolveLevel(VOID *Context, CONST CR_PEP_NPA_BINDING *Binding,
    UINT64 Level, CR_PEP_NPA_VALUE *Value) {
  BACKEND *Backend=Context; ++Backend->Resolves; Backend->LastLevel=Level;
  if (Backend->Offline) return CR_NOT_READY;
  if (Level>0xffffffffULL) return CR_UNSUPPORTED;
  UINT32 I=Index(Binding);
  if (I==1 && Level!=0 && !Backend->XoAux) return CR_UNSUPPORTED;
  assert(Level==0 || Level==(I==0 ? 256 : 128));
  *Value=(CR_PEP_NPA_VALUE){.Present=TRUE,.State=Level==0 ? 0 : I==0 ? 4 : 3};
  return CR_SUCCESS;
}
static CR_STATUS Get(VOID *Context, VOID *Token, CONST CR_PEP_NPA_BINDING *Binding,
    CR_PEP_NPA_VALUE *Value, BOOLEAN *Pending) {
  BACKEND *Backend=Context; ++Backend->Gets;
  if (Backend->Offline) return CR_NOT_READY;
  if (Backend->Unreserved) return CR_NOT_FOUND;
  assert(Token==&Owner || Token==&Foreign);
  *Value=Backend->Values[Token==&Owner ? 0 : 1][Index(Binding)];
  if (Backend->Corrupt) *Value=(CR_PEP_NPA_VALUE){.State=1};
  *Pending=Backend->Pending; return CR_SUCCESS;
}
static CR_STATUS Set(VOID *Context, VOID *Token, CONST CR_PEP_NPA_BINDING *Binding,
    CONST CR_PEP_NPA_VALUE *Value) {
  BACKEND *Backend=Context; ++Backend->Sets;
  if (Backend->Offline) return CR_NOT_READY;
  assert(Token==&Owner || Token==&Foreign);
  Backend->Values[Token==&Owner ? 0 : 1][Index(Binding)]=*Value;
  if (Backend->FailSet) {--Backend->FailSet; return CR_TIMEOUT;}
  Backend->Pending=FALSE; return CR_SUCCESS;
}
static CR_PEP_RESOURCE_OP Operation(CONST CHAR8 *Name, UINT64 Level) {
  return (CR_PEP_RESOURCE_OP){.Kind=CrPepNpa,.Name={Name,strlen(Name)},
    .ArgumentCount=2,.Arguments={1,Level}};
}
int main(void) {
  BACKEND Backend={0}; CONST CR_PEP_NPA_BINDING *Bindings; UINT32 Count;
  CrPepNpaGetKodiakBindings(&Bindings,&Count); assert(Count==2);
  CR_PEP_NPA_IO Io={&Backend,ResolveLevel,Get,Set}; CR_PEP_NPA_PROVIDER Provider={0};
  assert(CrPepNpaProviderInit(&Provider,&Io,Bindings,Count)==CR_SUCCESS);
  assert(CrPepNpaProviderInit(&Provider,&Io,Bindings,Count)==CR_ALREADY_STARTED);
  CR_PEP_JOURNAL_PROVIDER Callbacks;
  assert(CrPepNpaProviderGetJournal(&Provider,&Callbacks)==CR_SUCCESS);
  CR_PEP_RESOURCE_OP Cx=Operation("/arc/client/rail_cx",256),Xo=Operation("/arc/client/rail_xo",128),Bad=Cx;
  CONST CR_PEP_NPA_BINDING *Binding; CONST CR_PEP_NPA_PROFILE *Profile; UINT64 Resource;
  assert(CrPepNpaProviderDescribe(&Provider,&Cx,&Binding,&Profile)==CR_SUCCESS);
  assert(Binding==&Bindings[0] && Profile->Arguments[1]==256 && Backend.Resolves==0);
  assert(Callbacks.Resolve(&Provider,&Owner,&Cx,&Resource)==CR_SUCCESS && Resource==1);
  assert(Callbacks.Resolve(&Provider,&Owner,&Xo,&Resource)==CR_UNSUPPORTED);
  assert(Backend.Sets==0);
  Backend.XoAux=TRUE;
  assert(Callbacks.Resolve(&Provider,&Owner,&Xo,&Resource)==CR_SUCCESS && Resource==2);
  UINT32 CxRecords=0,XoRecords=0;
  for (UINT32 I=0; I<sizeof(OriginalNpaRecords)/sizeof(OriginalNpaRecords[0]); ++I) {
    CR_PEP_RESOURCE_OP Decoded;
    assert(CrPepDecodeResource(&OriginalNpaRecords[I],&Decoded)==CR_SUCCESS);
    assert(CrPepNpaProviderDescribe(&Provider,&Decoded,&Binding,&Profile)==CR_SUCCESS);
    assert(Callbacks.Resolve(&Provider,&Owner,&Decoded,&Resource)==CR_SUCCESS);
    if (Resource==1) ++CxRecords; else {assert(Resource==2); ++XoRecords;}
  }
  assert(CxRecords==15 && XoRecords==6);
  UINT32 Resolves=Backend.Resolves,Sets=Backend.Sets;
  Bad.Arguments[1]=256ULL+(1ULL<<32);
  assert(CrPepNpaProviderDescribe(&Provider,&Bad,&Binding,&Profile)==CR_UNSUPPORTED);
  Bad=Cx; Bad.Arguments[0]=2;
  assert(Callbacks.Resolve(&Provider,&Owner,&Bad,&Resource)==CR_UNSUPPORTED);
  Bad=Cx; Bad.Name=(CR_PEP_TEXT){"/arc/client/unknown",19};
  assert(Callbacks.Resolve(&Provider,&Owner,&Bad,&Resource)==CR_NOT_FOUND);
  Bad=Cx; Bad.DriverName=(CR_PEP_TEXT){"ignored",7};
  assert(Callbacks.Resolve(&Provider,&Owner,&Bad,&Resource)==CR_UNSUPPORTED);
  assert(Backend.Resolves==Resolves && Backend.Sets==Sets);
  CR_PEP_NPA_PROFILE WideProfile={0};
  WideProfile.Arguments[0]=1; WideProfile.Arguments[1]=0x1234567800000100ULL;
  CR_PEP_NPA_BINDING WideBinding=Bindings[0]; WideBinding.Profiles=&WideProfile; WideBinding.ProfileCount=1;
  CR_PEP_NPA_PROVIDER Wide={0};
  assert(CrPepNpaProviderInit(&Wide,&Io,&WideBinding,1)==CR_SUCCESS);
  Bad=Cx; Bad.Arguments[1]=WideProfile.Arguments[1];
  assert(CrPepNpaProviderDescribe(&Wide,&Bad,&Binding,&Profile)==CR_SUCCESS);
  CR_PEP_JOURNAL_PROVIDER WideCallbacks;
  assert(CrPepNpaProviderGetJournal(&Wide,&WideCallbacks)==CR_SUCCESS);
  assert(WideCallbacks.Resolve(&Wide,&Owner,&Bad,&Resource)==CR_UNSUPPORTED);
  assert(Backend.LastLevel==WideProfile.Arguments[1]);
  assert(CrPepNpaProviderUninit(&Wide)==CR_SUCCESS);
  Backend.Unreserved=TRUE;
  assert(Callbacks.Resolve(&Provider,&Owner,&Cx,&Resource)==CR_NOT_FOUND);
  Backend.Unreserved=FALSE; Backend.Pending=TRUE;
  assert(Callbacks.Resolve(&Provider,&Owner,&Cx,&Resource)==CR_NOT_READY);
  Backend.Pending=FALSE; Backend.Corrupt=TRUE;
  assert(Callbacks.Resolve(&Provider,&Owner,&Cx,&Resource)==CR_COMPROMISED_DATA);
  Backend.Corrupt=FALSE;
  CR_PEP_JOURNAL Journal={0}; CR_PEP_JOURNAL_ENTRY Transaction[4],Owned[2];
  assert(CrPepJournalInit(&Journal,&Owner,&Callbacks,Transaction,4,Owned,2)==CR_SUCCESS);
  Backend.Values[1][0]=(CR_PEP_NPA_VALUE){.State=6,.Present=TRUE};
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS);
  assert(CrPepJournalApply(&Journal,&Cx)==CR_SUCCESS);
  assert(CrPepJournalCommit(&Journal)==CR_SUCCESS);
  assert(Backend.Values[0][0].State==4 && Backend.Values[1][0].State==6);
  Backend.Offline=TRUE;
  assert(CrPepJournalClose(&Journal)==CR_NOT_READY && Journal.Closing && Journal.OwnedCount==1);
  assert(CrPepJournalBegin(&Journal)==CR_BUSY);
  assert(CrPepJournalUninit(&Journal)==CR_BUSY);
  Backend.Offline=FALSE;
  assert(CrPepJournalClose(&Journal)==CR_SUCCESS && !Backend.Values[0][0].Present);
  assert(Backend.Values[1][0].State==6);
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS); Backend.FailSet=2;
  assert(CrPepJournalApply(&Journal,&Cx)==CR_TIMEOUT && Journal.TransactionCount==1);
  assert(CrPepJournalRollback(&Journal)==CR_TIMEOUT && Journal.Active && Journal.TransactionCount==1);
  assert(CrPepJournalBegin(&Journal)==CR_BUSY);
  assert(CrPepJournalRollback(&Journal)==CR_SUCCESS && !Journal.Active && Journal.OwnedCount==0);
  assert(!Backend.Values[0][0].Present && Backend.Values[1][0].State==6);
  /* Capture and restore preserve opaque backend state at full UINT64 width. */
  Backend.Values[0][0]=(CR_PEP_NPA_VALUE){.State=0xfedcba9876543210ULL,.Present=TRUE};
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS);
  assert(CrPepJournalApply(&Journal,&Cx)==CR_SUCCESS);
  assert(Transaction[0].Before.Words[1]==0xfedcba9876543210ULL);
  assert(CrPepJournalRollback(&Journal)==CR_SUCCESS);
  assert(Backend.Values[0][0].State==0xfedcba9876543210ULL);
  CR_PEP_JOURNAL_VALUE Invalid={0}; Invalid.Words[1]=1;
  assert(Callbacks.Restore(&Provider,&Owner,1,&Invalid)==CR_INVALID_PARAMETER);
  Invalid.Words[0]=1; Invalid.Words[2]=1;
  assert(Callbacks.Restore(&Provider,&Owner,1,&Invalid)==CR_INVALID_PARAMETER);
  assert(Callbacks.Apply(&Provider,&Owner,1,&Xo)==CR_UNSUPPORTED);
  assert(CrPepJournalUninit(&Journal)==CR_SUCCESS);
  assert(CrPepNpaProviderUninit(&Provider)==CR_SUCCESS);
  puts("Original Kodiak21 NPA tuples, full-width levels, retained rollback and foreign owners: PASS");
  return 0;
}
