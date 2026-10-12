/* SPDX-License-Identifier: MIT */
#ifndef _KERNEL_MODE
#include <Uefi.h>
#else
#include <ntddk.h>
#endif
#include <Library/cmddb.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../Library/CmdDBLib/cmddb_internal.h"
#ifndef _KERNEL_MODE
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {(VOID)Level;(VOID)Format;}
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A, CONST CHAR8 *B, UINTN N) { return strncmp(A,B,N); }
#endif
int main(void) {
  UINT8 Region[144+24+8]={0};
  CmdDbHeader *Header=(CmdDbHeader *)Region;
  Header->magic=CMD_DB_MAGIC;
  Header->rscHeaders[0]=(ResourceHeader){.slaveId=4,.headerOffset=0,.dataOffset=24,.count=1};
  EntryHeader *Entry=(EntryHeader *)Header->data;
  memcpy(Entry->id,"rfclka1",8);Entry->address=0x40000;Entry->length=8;
  memset(Region+144+24,0x37,8);
  assert(!ValidateCmdDBHeader(NULL));
  assert(!ValidateCmdDBRegion(NULL,sizeof(Region)));
  assert(ValidateCmdDBRegion(Header,sizeof(Region)));
  for (UINTN Size=0;Size<sizeof(Region);++Size) assert(!ValidateCmdDBRegion(Header,Size));
  UINT32 Address=0,Length=4;UINT8 Output[8];memset(Output,0xee,sizeof(Output));
  assert(GetCmdDBEntryAddressByName(Header,"rfclka1",&Address)==CR_SUCCESS && Address==0x40000);
  assert(GetCmdDBAuxDataByName(Header,"rfclka1",Output,&Length)==CR_BUFFER_TOO_SMALL && Length==8);
  for (UINTN I=0;I<8;++I) assert(Output[I]==0xee);
  assert(GetCmdDBAuxDataByAddress(Header,Address,Output,&Length)==CR_SUCCESS && Length==8);
  for (UINTN I=0;I<8;++I) assert(Output[I]==0x37);
  Header->rscHeaders[0].count=2;assert(!ValidateCmdDBRegion(Header,sizeof(Region)));
  Header->rscHeaders[0].count=1;
  Header->rscHeaders[0].headerOffset=0xffff;assert(!ValidateCmdDBRegion(Header,sizeof(Region)));
  Header->rscHeaders[0].headerOffset=0;
  Entry->offset=0xffff;assert(!ValidateCmdDBRegion(Header,sizeof(Region)));
  Entry->offset=0;Entry->length=9;assert(!ValidateCmdDBRegion(Header,sizeof(Region)));
  Entry->length=0;Header->rscHeaders[0].dataOffset=32;
  assert(ValidateCmdDBRegion(Header,sizeof(Region))); // Empty aux span at end is valid.
  Header->rscHeaders[0].dataOffset=33;assert(!ValidateCmdDBRegion(Header,sizeof(Region)));
  puts("CmdDB complete-region bounds, every truncation and auxiliary capacity checks passed");
  return 0;
}
