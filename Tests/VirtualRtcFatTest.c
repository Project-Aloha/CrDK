/* SPDX-License-Identifier: MIT
 * Actual virtual RTC/calendar and FAT metadata routines. Timer and filesystem
 * I/O are injected; production date conversion and SetFileInfo are unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#undef NULL
#include "Fat.h"
#include <Library/RealTimeClockLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/ArmGenericTimerCounterLib.h>

static UINTN Frequency=24000000,CounterReads,Flushes;
static UINT64 Counter;
static EFI_STATUS FlushStatus=EFI_SUCCESS;
static FAT_OFILE File;
EFI_RUNTIME_SERVICES *gRT;
UINT8 mMonthDays[]={31,28,31,30,31,30,31,31,30,31,30,31};

UINTN EFIAPI ArmGenericTimerGetTimerFreq(VOID) {return Frequency;}
UINT64 EFIAPI GetPerformanceCounter(VOID) {CounterReads++;return Counter;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID) {return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D) {fprintf(stderr,"%s:%zu %s\n",F,(size_t)L,D);abort();}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N) {return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) {return memcmp(A,B,N);}
BOOLEAN EFIAPI IsListEmpty(CONST LIST_ENTRY *L) {return L->ForwardLink==L;}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *L) {(VOID)L;abort();}
LIST_ENTRY *EFIAPI InsertHeadList(LIST_ENTRY *L,LIST_ENTRY *E) {(VOID)L;(VOID)E;abort();}
EFI_STATUS FatLocateOFile(FAT_OFILE **F,CHAR16 *N,UINT8 A,CHAR16 *Remaining) {
 (VOID)N;(VOID)A;*F=&File;*Remaining=0;return EFI_SUCCESS;
}
EFI_STATUS FatOFileFlush(FAT_OFILE *F) {assert(F==&File);Flushes++;return FlushStatus;}
EFI_STATUS FatRemoveDirEnt(FAT_OFILE *F,FAT_DIRENT *D) {(VOID)F;(VOID)D;abort();}
EFI_STATUS FatCreateDirEnt(FAT_OFILE *F,CHAR16 *N,UINT8 A,FAT_DIRENT **D) {(VOID)F;(VOID)N;(VOID)A;(VOID)D;abort();}
VOID FatCloneDirEnt(FAT_DIRENT *A,FAT_DIRENT *B) {(VOID)A;(VOID)B;abort();}
VOID FatFreeDirEnt(FAT_DIRENT *D) {(VOID)D;abort();}
VOID FatResetODirCursor(FAT_OFILE *F) {(VOID)F;abort();}
EFI_STATUS FatGetNextDirEnt(FAT_OFILE *F,FAT_DIRENT **D) {(VOID)F;(VOID)D;abort();}
BOOLEAN FatIsDotDirEnt(FAT_DIRENT *D) {(VOID)D;abort();}
EFI_STATUS FatStoreDirEnt(FAT_OFILE *F,FAT_DIRENT *D) {(VOID)F;(VOID)D;abort();}
EFI_STATUS FatExpandOFile(FAT_OFILE *F,UINT64 N) {(VOID)F;(VOID)N;abort();}
EFI_STATUS FatTruncateOFile(FAT_OFILE *F,UINTN N) {(VOID)F;(VOID)N;abort();}
VOID FatUpdateDirEntClusterSizeInfo(FAT_OFILE *F) {(VOID)F;abort();}

#include "FatTimeActual.inc"

static VOID CheckCalendar(UINT64 Seconds) {
 EFI_TIME T;
 EFI_TIME_CAPABILITIES C;
 time_t Epoch=(time_t)(1546300800ULL+Seconds);struct tm Expected;
 assert(gmtime_r(&Epoch,&Expected)!=NULL);
 Counter=Seconds*Frequency+Frequency-1;
 memset(&T,0xa5,sizeof(T));memset(&C,0xa5,sizeof(C));
 assert(LibGetTime(&T,&C)==EFI_SUCCESS && IsTimeValid(&T));
 assert(T.Year==Expected.tm_year+1900 && T.Month==Expected.tm_mon+1 && T.Day==Expected.tm_mday);
 assert(T.Hour==Expected.tm_hour && T.Minute==Expected.tm_min && T.Second==Expected.tm_sec);
 assert(T.Nanosecond==0 && T.TimeZone==0 && T.Daylight==0 && T.Pad1==0 && T.Pad2==0);
 assert(C.Resolution==Frequency && C.Accuracy==0 && !C.SetsToZero);
}

int main(int Argc,char **Argv) {
 BOOLEAN Before=Argc==2&&!strcmp(Argv[1],"--before");
 FAT_DIRENT Entry={0};FAT_OFILE Parent={0};FAT_IFILE Handle={0};FAT_VOLUME Volume={0};
 struct {EFI_FILE_INFO Info;CHAR16 Extra[16];} Request={0};EFI_FILE_INFO *Info=&Request.Info;
 EFI_TIME Now;FAT_DATE_TIME Created={0};
 static EFI_RUNTIME_SERVICES Runtime={.GetTime=LibGetTime};gRT=&Runtime;
 assert(LibRtcInitialize(NULL,NULL)==EFI_SUCCESS);
 Counter=3*60*Frequency;FatGetCurrentFatTime(&Created);FatFatTimeToEfiTime(&Created,&Now);
 File.Parent=&Parent;File.DirEnt=&Entry;Info->Size=sizeof(Request);
 Info->CreateTime=Info->ModificationTime=Now;
 Entry.Entry.FileCreateTime=Entry.Entry.FileModificationTime=Created;
 EFI_STATUS Status=FatSetFileInfo(&Volume,&Handle,&File,sizeof(Request),Info);
 if(Before) {
  assert(Now.Year==2019 && Now.Month==1 && Now.Day==0 && !FatIsValidTime(&Now));
  assert(Status==EFI_INVALID_PARAMETER && Flushes==0);
  puts("BEFORE reproduced actual RTC -> FAT January day0 -> SetFileInfo Invalid Parameter");return 0;
 }
 assert(Now.Year==2019 && Now.Month==1 && Now.Day==1 && FatIsValidTime(&Now));
 assert(Status==EFI_SUCCESS && Flushes==1);
 // Existing on-disk day0 entries: attribute-only preserve timestamps succeeds.
 Entry.Entry.FileCreateTime.Date.Day=Entry.Entry.FileModificationTime.Date.Day=0;
 FAT_DATE_TIME SavedCreate=Entry.Entry.FileCreateTime,SavedModify=Entry.Entry.FileModificationTime;
 memset(&Info->CreateTime,0,sizeof(EFI_TIME));memset(&Info->ModificationTime,0,sizeof(EFI_TIME));
 Info->Attribute=EFI_FILE_HIDDEN;
 assert(FatSetFileInfo(&Volume,&Handle,&File,sizeof(Request),Info)==EFI_SUCCESS && Entry.Entry.Attributes==EFI_FILE_HIDDEN);
 assert(!memcmp(&SavedCreate,&Entry.Entry.FileCreateTime,sizeof(SavedCreate)) && !memcmp(&SavedModify,&Entry.Entry.FileModificationTime,sizeof(SavedModify)));
 Info->CreateTime=Now;Info->CreateTime.Day=0;
 assert(FatSetFileInfo(&Volume,&Handle,&File,sizeof(Request),Info)==EFI_INVALID_PARAMETER);
 memset(&Info->CreateTime,0,sizeof(EFI_TIME));FlushStatus=EFI_DEVICE_ERROR;
 assert(FatSetFileInfo(&Volume,&Handle,&File,sizeof(Request),Info)==EFI_DEVICE_ERROR);
 // Every day through FAT's supported era includes leap2000/2020 and nonleap2100.
 for(UINT64 Day=0;Day<32500;Day++) CheckCalendar(Day*86400ULL);
 UINT64 Cases[]={0,59,60,3599,3600,86399,86400,2678400,31535999,31536000,
  36720000,253402300799ULL-1546300800ULL};
 Frequency=1;
 for(UINTN I=0;I<sizeof(Cases)/sizeof(Cases[0]);I++) CheckCalendar(Cases[I]);
 EFI_TIME Bad,Snapshot;EFI_TIME_CAPABILITIES C,CBefore;
 memset(&Bad,0xa5,sizeof(Bad));Snapshot=Bad;memset(&C,0xa5,sizeof(C));CBefore=C;
 Counter=253402300800ULL-1546300800ULL;
 assert(LibGetTime(&Bad,&C)==EFI_DEVICE_ERROR && !memcmp(&Bad,&Snapshot,sizeof(Bad)) && !memcmp(&C,&CBefore,sizeof(C)));
 Counter=MAX_UINT64;
 assert(LibGetTime(&Bad,&C)==EFI_DEVICE_ERROR && !memcmp(&Bad,&Snapshot,sizeof(Bad)));
 Frequency=0;CounterReads=0;
 assert(LibGetTime(NULL,&C)==EFI_INVALID_PARAMETER && CounterReads==0);
 assert(LibGetTime(&Bad,&C)==EFI_DEVICE_ERROR && CounterReads==0 && !memcmp(&Bad,&Snapshot,sizeof(Bad)) && !memcmp(&C,&CBefore,sizeof(C)));
 assert(LibSetTime(&Now)==EFI_UNSUPPORTED);
 puts("Actual virtual RTC Gregorian/calendar boundaries + FAT new-date validation/old-date preservation/error propagation PASS");return 0;
}
