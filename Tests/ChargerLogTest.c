/** Charger firmware log wire/bounds regressions. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ucsi/ChargerLog.h>

static CR_CHARGER_LOG Log;
static UINT32 Sent[4];
static UINTN Requests, Printed, PrintBytes;
static EFI_STATUS SendStatus;
static char FirstLine[161];
VOID *EFIAPI CopyMem(VOID *Dst, CONST VOID *Src, UINTN Size) { return memcpy(Dst,Src,Size); }
VOID *EFIAPI ZeroMem(VOID *Dst, UINTN Size) { return memset(Dst,0,Size); }
VOID *EFIAPI SetMem(VOID *Dst, UINTN Size, UINT8 Value) { return memset(Dst,Value,Size); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (void)Level; return TRUE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...)
{
  (void)Level;
  if(strstr(Format,"ChargerPD: %a")==NULL) return;
  va_list Args; va_start(Args,Format);
  CONST CHAR8 *Line=va_arg(Args,CONST CHAR8 *); va_end(Args);
  UINTN Length=strlen(Line);
  assert(Length<=160);
  for(UINTN I=0;I<Length;++I) assert(Line[I]>=32 && Line[I]<=126);
  if(Printed==0) strcpy(FirstLine,Line);
  ++Printed; PrintBytes+=Length;
}
static EFI_STATUS Send(VOID *Context, CONST VOID *Data, UINTN Length)
{
  assert(Context==&Log && Length==16);
  memcpy(Sent,Data,Length);
  assert(Sent[0]==32778 && Sent[1]==1 && (Sent[2]==0x18 || Sent[2]==0x23) && Sent[3]==8192);
  if(!EFI_ERROR(SendStatus)) ++Requests;
  return SendStatus;
}
static void Fixture(void)
{
  Requests=Printed=PrintBytes=0; FirstLine[0]=0; SendStatus=EFI_SUCCESS;
  CrChargerLogInitialize(&Log,Send,&Log);
  CrChargerLogOnline(&Log,TRUE);
}
static void Put32(UINT8 *Data, UINT32 Value) { memcpy(Data,&Value,4); }
static void Reply(UINT8 *Data, UINT32 Opcode)
{
  memset(Data,0,8204); Put32(Data,32778); Put32(Data+4,1); Put32(Data+8,Opcode);
}
int main(void)
{
  static UINT8 Data[8204];
  Fixture(); CrChargerLogTick(&Log,0,FALSE); assert(Requests==0);
  SendStatus=EFI_NOT_READY;
  for(UINTN I=0;I<100;++I) CrChargerLogTick(&Log,I,TRUE);
  assert(Requests==0 && !Log.Pending && !Log.Done && Log.Triggered);
  SendStatus=EFI_SUCCESS;
  CrChargerLogTick(&Log,200,FALSE);
  assert(Requests==1 && Log.Pending && Log.Opcode==0x23 && Log.InitRequests==1);
  Reply(Data,0x23); Put32(Data,32779);
  assert(CrChargerLogReceive(&Log,Data,sizeof(Data))==EFI_SUCCESS && Log.Pending);
  Reply(Data,0x18);
  CrChargerLogReceive(&Log,Data,sizeof(Data));
  Reply(Data,0x23); Put32(Data+4,3);
  CrChargerLogReceive(&Log,Data,sizeof(Data));
  Reply(Data,0x23);
  CrChargerLogReceive(&Log,Data,sizeof(Data)-1);
  assert(Log.Malformed==4 && Log.Pending);
  strcpy((char *)Data+12,"%s %Lx: USB-PD init\033\r\nnext\n");
  assert(CrChargerLogReceive(&Log,Data,sizeof(Data))==EFI_SUCCESS && !Log.Pending);
  /* A duplicate while printing cannot overwrite the retained page. */
  CrChargerLogReceive(&Log,Data,sizeof(Data));
  CrChargerLogTick(&Log,300,FALSE);
  assert(strcmp(FirstLine,"%s %Lx: USB-PD init.")==0 && Printed==2);
  assert(Requests==2 && Log.Pending && Log.Opcode==0x23);
  Reply(Data,0x23); CrChargerLogReceive(&Log,Data,sizeof(Data));
  CrChargerLogTick(&Log,400,FALSE);
  assert(Requests==3 && Log.Runtime && Log.Opcode==0x18);
  Reply(Data,0x18); strcpy((char *)Data+12,"UCSI firmware status\n");
  CrChargerLogReceive(&Log,Data,sizeof(Data));
  CrChargerLogTick(&Log,500,FALSE);
  /* An 8 KiB runtime page can contain old boot records. Continue reading
     bounded pages so later UCSI/Type-C events are not silently omitted. */
  assert(!Log.Done && !Log.Pending && Printed==3);
  CrChargerLogTick(&Log,1000400,FALSE);
  assert(Requests==4 && Log.Pending && Log.Opcode==0x18);
  Reply(Data,0x18); CrChargerLogReceive(&Log,Data,sizeof(Data));
  CrChargerLogTick(&Log,60000400,FALSE);
  assert(Log.Done);
  for(UINTN I=0;I<100;++I) CrChargerLogTick(&Log,10000000+I,TRUE);
  assert(Requests==4);

  Fixture(); CrChargerLogTick(&Log,0,TRUE);
  /* Eight init pages maximum, including a full non-NUL-terminated page. */
  UINT64 Now=0;
  for(UINTN Page=0;Page<8;++Page) {
    Reply(Data,0x23);
    if(Page==0) memset(Data+12,'X',8192); else strcpy((char *)Data+12,"page\n");
    CrChargerLogReceive(&Log,Data,sizeof(Data));
    UINTN Before=Printed;
    CrChargerLogTick(&Log,++Now,TRUE);
    assert(Printed-Before<=4);
    while(Log.Length) {
      Before=Printed; CrChargerLogTick(&Log,++Now,TRUE);
      assert(Printed-Before<=4);
    }
    assert(Requests==Page+2);
  }
  assert(Log.Runtime && Log.Pending && Log.Opcode==0x18 && Log.InitRequests==8 && Requests==9);
  Reply(Data,0x18); CrChargerLogReceive(&Log,Data,sizeof(Data));
  CrChargerLogTick(&Log,++Now,TRUE);
  assert(!Log.Done && Log.RuntimeRequests==1 && PrintBytes==8192+7*4);
  /* Empty runtime pages do not end observation before a later cable event.
     No more than 32 requests, at least one second apart, even if the peer
     continuously supplies complete non-NUL-terminated pages. */
  for(UINTN Page=1;Page<32;++Page) {
    UINTN Count=Requests;
    CrChargerLogTick(&Log,Log.NextRequestUs-1,TRUE);
    assert(Requests==Count && !Log.Pending);
    Now=Log.NextRequestUs; CrChargerLogTick(&Log,Now,TRUE);
    assert(Requests==Count+1 && Log.Pending && Log.RuntimeRequests==Page+1);
    Reply(Data,0x18);
    if(Page==1) memset(Data+12,'R',8192);
    else strcpy((char *)Data+12,"later Type-C status\n");
    CrChargerLogReceive(&Log,Data,sizeof(Data));
    do {
      UINTN Before=Printed; CrChargerLogTick(&Log,++Now,TRUE);
      assert(Printed-Before<=4);
    } while(Log.Length);
  }
  assert(Log.Done && Requests==40 && Log.RuntimeRequests==32);
  CrChargerLogTick(&Log,Now+100000000,TRUE); assert(Requests==40);

  Fixture(); CrChargerLogTick(&Log,0,TRUE);
  CrChargerLogTick(&Log,999999,TRUE); assert(Requests==1);
  CrChargerLogTick(&Log,1000000,TRUE);
  assert(Requests==2 && Log.Runtime && Log.Pending && Log.Opcode==0x18);
  Reply(Data,0x23); CrChargerLogReceive(&Log,Data,sizeof(Data));
  assert(Log.Pending && Log.Malformed==1); /* late INIT cannot complete runtime */
  CrChargerLogTick(&Log,2000000,TRUE);
  assert(Log.Done && Log.LastError==EFI_TIMEOUT && Requests==2);
  CrChargerLogOnline(&Log,FALSE);
  assert(!Log.Pending && !Log.Online && Log.Send==Send && Log.Context==&Log);
  CrChargerLogTick(&Log,3000000,TRUE); assert(Requests==2);
  CrChargerLogOnline(&Log,TRUE); SendStatus=EFI_DEVICE_ERROR;
  CrChargerLogTick(&Log,3000000,TRUE);
  assert(Log.Done && Log.LastError==EFI_DEVICE_ERROR && Requests==2);
  Fixture(); SendStatus=EFI_NOT_READY;
  CrChargerLogTick(&Log,0,TRUE); assert(Log.Waiting && !Log.Pending);
  CrChargerLogTick(&Log,1000000,TRUE); assert(Log.Runtime && Log.Waiting && Requests==0);
  CrChargerLogTick(&Log,2000000,TRUE);
  assert(Log.Done && !Log.Pending && !Log.Waiting && Log.LastError==EFI_TIMEOUT);
  puts("ChargerPD logs: read-only wire, bounded pages/timeouts, malformed/stale replies and safe timer output passed");
  return 0;
}
