/* SPDX-License-Identifier: MIT
 * Executes the production redirection parser and ShellLib open path. Only
 * firmware services/file protocol operations are injected at the boundary.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Shell.h"
#include "UefiShellLib.h"
#define STR_SHELL_REDUNDA_REDIR 1
#define STR_SHELL_INVALID_REDIR 2

SHELL_INFO ShellInfoObject;
EFI_SHELL_PROTOCOL *gEfiShellProtocol;
EFI_SHELL_ENVIRONMENT2 *mEfiShellEnvironment2;
EFI_UNICODE_COLLATION_PROTOCOL *mUnicodeCollationProtocol;
EFI_UNICODE_COLLATION_PROTOCOL *gUnicodeCollation;
FILE_HANDLE_FUNCTION_MAP FileFunctionMap;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gShellVariableGuid;
EFI_GUID gEfiUnicodeCollation2ProtocolGuid;
BOOLEAN AsciiRedirection;
CONST CHAR16 gUnicodeFileTag = 0xfeff;
STATIC EFI_STATUS mOpenStatus, mSetStatus, mWriteStatus, mLocateStatus;
STATIC BOOLEAN mInfoMissing, mNullSuccess;
STATIC BOOLEAN mLegacyDates;
STATIC UINTN mOpens, mClosed, mTags, mSizes, mInterfaces, mPrinted;
STATIC CHAR16 mOpenedName[64];
STATIC EFI_FILE_PROTOCOL mFile;
STATIC BOOLEAN mLive;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL mText;

LIST_ENTRY *EFIAPI InitializeListHead (LIST_ENTRY *L) { L->ForwardLink=L->BackLink=L;return L; }
BOOLEAN EFIAPI IsListEmpty (CONST LIST_ENTRY *L) { return L->ForwardLink==L; }
LIST_ENTRY *EFIAPI GetFirstNode (CONST LIST_ENTRY *L) { return L->ForwardLink; }
UINTN EFIAPI StrLen (CONST CHAR16 *S) { UINTN N=0; while(S[N]) N++; return N; }
UINTN EFIAPI StrSize (CONST CHAR16 *S) { return (StrLen(S)+1)*sizeof(*S); }
INTN EFIAPI StrCmp (CONST CHAR16 *A, CONST CHAR16 *B) { while(*A && *A==*B) {A++;B++;} return *A-*B; }
CHAR16 *EFIAPI StrStr (CONST CHAR16 *S, CONST CHAR16 *N) {
  for (;*S;S++) { UINTN I=0; while(N[I] && S[I]==N[I]) I++; if(!N[I]) return (CHAR16 *)S; }
  return *N ? NULL : (CHAR16 *)S;
}
RETURN_STATUS EFIAPI StrnCpyS (CHAR16 *D, UINTN N, CONST CHAR16 *S, UINTN C) {
  assert(N>0); UINTN I=0; while(I+1<N && I<C && S[I]) {D[I]=S[I];I++;} D[I]=0; return 0;
}
CHAR16 *EFIAPI StrnCatGrow (CHAR16 **D, UINTN *Size, CONST CHAR16 *S, UINTN C) {
  assert(*D==NULL && Size==NULL && C==0); *D=malloc(StrSize(S)); assert(*D); memcpy(*D,S,StrSize(S)); return *D;
}
VOID *EFIAPI SetMem16 (VOID *B, UINTN N, UINT16 V) { for(UINTN I=0;I<N/2;I++) ((UINT16*)B)[I]=V; return B; }
VOID *EFIAPI ZeroMem (VOID *B, UINTN N) { return memset(B,0,N); }
VOID *EFIAPI AllocateCopyPool (UINTN N, CONST VOID *S) { VOID *B=malloc(N); assert(B); return memcpy(B,S,N); }
VOID EFIAPI FreePool (VOID *P) { free(P); }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID EFIAPI DebugAssert (CONST CHAR8 *F, UINTN L, CONST CHAR8 *D) { fprintf(stderr,"%s:%zu %s\n",F,(size_t)L,D); abort(); }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (UINTN L) { (VOID)L; return TRUE; }
VOID EFIAPI DebugPrint (UINTN L, CONST CHAR8 *F, ...) { (VOID)L;(VOID)F; }
INTN EFIAPI StringNoCaseCompare (CONST VOID *A, CONST VOID *B) { return StrCmp(*(CHAR16*CONST*)A,*(CHAR16*CONST*)B); }
EFI_STATUS IsVolatileEnv (CONST CHAR16 *S, BOOLEAN *V) { (VOID)S; *V=TRUE; return EFI_SUCCESS; }
EFI_STATUS EFIAPI ShellFileExists (CONST CHAR16 *S) { (VOID)S; return EFI_NOT_FOUND; }
EFI_STATUS IsUnicodeFile (CONST CHAR16 *S) { (VOID)S; return EFI_SUCCESS; }
VOID CalculateEfiHdrCrc (EFI_TABLE_HEADER *H) { (VOID)H; }
EFI_STATUS EFIAPI ShellPrintHiiEx (INT32 Col, INT32 Row, CONST CHAR8 *Lang, EFI_STRING_ID Token, EFI_HII_HANDLE H, ...) { (VOID)Col;(VOID)Row;(VOID)Lang;(VOID)Token;(VOID)H;mPrinted++; return EFI_SUCCESS; }
CHAR16 *EFIAPI PathCleanUpDirectories (CHAR16 *S) { return S; }
BOOLEAN EFIAPI PathRemoveLastItem (CHAR16 *S) { return StrStr(S,L"\\")!=NULL; }
EFI_STATUS EFIAPI ShellCreateDirectory (CONST CHAR16 *S, SHELL_FILE_HANDLE *H) { (VOID)S;*H=NULL;return EFI_NOT_FOUND; }
EFI_STATUS EFIAPI ShellOpenFileByDevicePath (EFI_DEVICE_PATH_PROTOCOL **P, SHELL_FILE_HANDLE *H, UINT64 M, UINT64 A) { (VOID)P;(VOID)H;(VOID)M;(VOID)A;abort(); }
EFI_FILE_PROTOCOL *CreateFileInterfaceFile (CONST EFI_FILE_PROTOCOL *H, BOOLEAN U) { (VOID)U;assert(H==&mFile && mLive);mInterfaces++;return (EFI_FILE_PROTOCOL *)H; }
EFI_FILE_PROTOCOL *CreateFileInterfaceEnv (CONST CHAR16 *S) { (VOID)S;abort(); }
EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *CreateSimpleTextOutOnFile (SHELL_FILE_HANDLE H, EFI_HANDLE *D, EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Old) {
 (VOID)D;(VOID)Old; assert(H==&mFile && mLive);mInterfaces++;return &mText;
}
EFI_SIMPLE_TEXT_INPUT_PROTOCOL *CreateSimpleTextInOnFile (SHELL_FILE_HANDLE H, EFI_HANDLE *D) { (VOID)H;(VOID)D;abort(); }

STATIC INTN EFIAPI Collate (EFI_UNICODE_COLLATION_PROTOCOL *P, CHAR16 *A, CHAR16 *B) { (VOID)P; return StrCmp(A,B); }
STATIC BOOLEAN EFIAPI Match (EFI_UNICODE_COLLATION_PROTOCOL *P, CHAR16 *A, CHAR16 *B) { return Collate(P,A,B)==0; }
STATIC EFI_STATUS EFIAPI Open (CONST CHAR16 *S, SHELL_FILE_HANDLE *H, UINT64 M) {
  assert(M==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE));
  mOpens++;StrnCpyS(mOpenedName,64,S,63);*H=NULL;
  if(EFI_ERROR(mOpenStatus)) return mOpenStatus;
  if(!mNullSuccess) {*H=&mFile;mLive=TRUE;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close (SHELL_FILE_HANDLE H) { assert(H==&mFile && mLive);mClosed++;mLive=FALSE;return EFI_SUCCESS; }
STATIC EFI_FILE_INFO *EFIAPI Info (SHELL_FILE_HANDLE H) {
 assert(H==&mFile && mLive); if(mInfoMissing) return NULL;
 EFI_FILE_INFO *F=calloc(1,sizeof(*F)+8);assert(F);F->Size=sizeof(*F)+8;
 if(mLegacyDates) {F->CreateTime.Year=F->ModificationTime.Year=F->LastAccessTime.Year=2019;F->CreateTime.Month=F->ModificationTime.Month=F->LastAccessTime.Month=1;}
 return F;
}
STATIC EFI_STATUS EFIAPI SetInfo (SHELL_FILE_HANDLE H, CONST EFI_FILE_INFO *F) {
 EFI_TIME Z={0};assert(H==&mFile && mLive && F->Attribute==0);
 assert(!memcmp(&F->CreateTime,&Z,sizeof(Z)) && !memcmp(&F->LastAccessTime,&Z,sizeof(Z)) && !memcmp(&F->ModificationTime,&Z,sizeof(Z)));
 return mSetStatus;
}
STATIC EFI_STATUS EFIAPI Write (SHELL_FILE_HANDLE H, UINTN *N, VOID *B) { assert(H==&mFile && mLive && *N==2 && *(CHAR16*)B==gUnicodeFileTag);mTags++;return mWriteStatus; }
STATIC EFI_STATUS EFIAPI GetSize (SHELL_FILE_HANDLE H, UINT64 *N) { assert(H==&mFile && mLive);mSizes++;*N=0;return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI SetPosition (SHELL_FILE_HANDLE H, UINT64 N) { assert(H==&mFile && mLive);(VOID)N;return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Delete (CONST CHAR16 *S) { (VOID)S;return EFI_NOT_FOUND; }
STATIC EFI_STATUS EFIAPI SetEnv (CONST CHAR16 *S, CONST CHAR16 *V, BOOLEAN B) { (VOID)S;(VOID)V;(VOID)B;abort(); }
STATIC EFI_STATUS EFIAPI Locate (EFI_GUID *G, VOID *R, VOID **P) { (VOID)G;(VOID)R;if(!EFI_ERROR(mLocateStatus)) *P=gUnicodeCollation;return mLocateStatus; }

#include "ShellRedirectionActual.inc"

STATIC VOID Reset (VOID) {
  assert(!mLive);mOpenStatus=mSetStatus=mWriteStatus=mLocateStatus=EFI_SUCCESS;
  mInfoMissing=mNullSuccess=mLegacyDates=FALSE;mOpens=mClosed=mTags=mSizes=mInterfaces=mPrinted=0;
  STATIC EFI_UNICODE_COLLATION_PROTOCOL Collation={.StriColl=Collate,.MetaiMatch=Match};
  STATIC EFI_SHELL_PROTOCOL Shell={.OpenFileByName=Open,.CloseFile=Close,.WriteFile=Write,
   .GetFileSize=GetSize,.SetFilePosition=SetPosition,.DeleteFileByName=Delete,.SetEnv=SetEnv};
  STATIC EFI_SYSTEM_TABLE Table;
  STATIC EFI_BOOT_SERVICES Boot={.LocateProtocol=Locate};
  memset(&Table,0,sizeof(Table));Table.ConOut=&mText;Table.ConIn=(VOID*)&mText;Table.StdErr=&mText;
  memset(&ShellInfoObject,0,sizeof(ShellInfoObject));InitializeListHead(&ShellInfoObject.SplitList.Link);
  ShellInfoObject.NewEfiShellProtocol=&Shell;gEfiShellProtocol=&Shell;gUnicodeCollation=&Collation;mUnicodeCollationProtocol=&Collation;
  gST=&Table;gBS=&Boot;FileFunctionMap.GetFileInfo=Info;FileFunctionMap.SetFileInfo=SetInfo;FileFunctionMap.CloseFile=Close;
}
STATIC EFI_STATUS Redirect (CONST CHAR16 *S) {
 CHAR16 Line[96];StrnCpyS(Line,96,S,95);EFI_SHELL_PARAMETERS_PROTOCOL P={.StdIn=(VOID*)1,.StdOut=(VOID*)2,.StdErr=(VOID*)3};
 SHELL_FILE_HANDLE I,O,E;SYSTEM_TABLE_INFO T;
 EFI_STATUS St=UpdateStdInStdOutStdErr(&P,Line,&I,&O,&E,&T);
 if(EFI_ERROR(St)) assert(P.StdOut==(VOID*)2);
 return St;
}
int main (int Argc, char **Argv) {
 BOOLEAN Before=Argc==2 && strcmp(Argv[1],"--before")==0;
 EFI_STATUS Errors[]={EFI_WRITE_PROTECTED,EFI_DEVICE_ERROR,EFI_VOLUME_FULL,EFI_ACCESS_DENIED,EFI_NO_MEDIA};
 for(UINTN I=0;I<sizeof(Errors)/sizeof(Errors[0]);I++) {
  Reset();mOpenStatus=Errors[I];EFI_STATUS S=Redirect(L"echo 1 > a");
  if(Before) assert(S==EFI_INVALID_PARAMETER);else assert(S==Errors[I]);
  assert(mOpens==1 && mTags==0 && mInterfaces==0 && mPrinted==1 && !mLive);
 }
 if(Before) {puts("BEFORE reproduced: every filesystem open failure becomes Invalid Parameter");return 0;}
 Reset();assert(Redirect(L"echo 1 > a")==EFI_SUCCESS);assert(StrCmp(mOpenedName,L"a")==0 && mTags==1 && mInterfaces==1);Close(&mFile);
 Reset();mLegacyDates=TRUE;assert(Redirect(L"echo 1 >> a")==EFI_SUCCESS);assert(mSizes==1 && mTags==1);Close(&mFile);
 Reset();assert(Redirect(L"echo 1 >a a")==EFI_SUCCESS);assert(mTags==0 && mInterfaces==2);Close(&mFile);
 Reset();mWriteStatus=EFI_VOLUME_FULL;assert(Redirect(L"echo 1 > a")==EFI_VOLUME_FULL);assert(mTags==1 && mClosed==1 && !mLive);
 Reset();mWriteStatus=EFI_DEVICE_ERROR;assert(Redirect(L"echo 1 2> a")==EFI_DEVICE_ERROR);assert(mTags==1 && mClosed==1 && !mLive);
 Reset();mSetStatus=EFI_VOLUME_FULL;assert(Redirect(L"echo 1 > a")==EFI_VOLUME_FULL);assert(mClosed==1 && !mLive && mTags==0);
 Reset();mSetStatus=EFI_DEVICE_ERROR;assert(Redirect(L"echo 1 >> a")==EFI_DEVICE_ERROR);assert(mClosed==1 && mSizes==0 && !mLive);
 Reset();mInfoMissing=TRUE;assert(Redirect(L"echo 1 > a")==EFI_DEVICE_ERROR);assert(mClosed==1 && !mLive);
 Reset();mUnicodeCollationProtocol=NULL;mLocateStatus=EFI_NOT_FOUND;assert(Redirect(L"echo 1 > a")==EFI_NOT_FOUND);assert(mClosed==1 && !mLive);
 Reset();mNullSuccess=TRUE;assert(Redirect(L"echo 1 > a")==EFI_DEVICE_ERROR);assert(mTags==0 && !mLive);
 puts("Shell actual redirection/open failure status and handle lifetime PASS");return 0;
}
