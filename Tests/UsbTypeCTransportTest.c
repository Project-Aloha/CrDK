/** Independent GLink/PMIC wire-format regressions. SPDX-License-Identifier: MIT */
#include <oskal/cr_types.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#include <Library/DebugLib.h>
#endif
#include <ucsi/Glink.h>
#include <ucsi/PmicGlink.h>

static CR_GLINK Link;
static UINT32 Descriptor[8];
static UINT8 Tx[16384], Rx[16384], Received[1024], Sent[1024];
static UINTN ReceivedLength, SentLength, Kicks;
static UINT32 BadItem, ExpectedNotifyTail;
static UINT16 UcsiVersion=0x110;
static UINT32 ReplyType=1;
static CR_STATUS PmicSendStatus=CR_SUCCESS;
static UINTN V2Writes;
static UINTN PmicSends;
static UINTN QrtrCalls, QrtrReceivedLength, QrtrTransitions;
static CR_STATUS QrtrStatus;
static BOOLEAN QrtrOnline;
static UINT8 LogReceived[CR_GLINK_LOG_SIZE];
static UINTN LogCalls, LogReceivedLength, LogTransitions;
static BOOLEAN LogOnline;
static CR_STATUS LogStatus;
static CR_GLINK_SMEM Smem;
#ifndef _KERNEL_MODE
VOID *EFIAPI CopyMem(VOID *Dst, CONST VOID *Src, UINTN Size) { return memcpy(Dst,Src,Size); }
VOID *EFIAPI ZeroMem(VOID *Dst, UINTN Size) { return memset(Dst,0,Size); }
VOID *EFIAPI SetMem(VOID *Dst, UINTN Size, UINT8 Value) { return memset(Dst,Value,Size); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN Size) { return memcmp(A,B,Size); }
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
#endif
static CR_STATUS Get(VOID *This, UINT32 Host, UINT32 Item,
                             UINT32 *Size, VOID **Ptr)
{
  assert(This==&Smem && Host==2);
  if(Item==478) { *Size=32; *Ptr=Descriptor; }
  else if(Item==479) { *Size=sizeof(Tx); *Ptr=Tx; }
  else if(Item==480) { *Size=sizeof(Rx); *Ptr=Rx; }
  else return CR_NOT_FOUND;
  if(Item==BadItem) { if(Item==478) *Ptr=NULL; else *Size=1024; }
  return CR_SUCCESS;
}
static CR_STATUS Allocate(VOID *This, UINT32 Host, UINT32 Item,
                                  UINT32 Size, VOID **Ptr)
{
  assert(This==&Smem && Host==2 && (Item==478 || Item==479));
  assert(Size==(Item==478?32:sizeof(Tx))); *Ptr=NULL;
  return CR_ALREADY_STARTED;
}
static void Receive(VOID *Context, CONST UINT8 *Data, UINTN Length)
{ assert(Context==&Link); memcpy(Received,Data,Length); ReceivedLength=Length; }
static CR_STATUS ReceiveQrtr(VOID *Context, CONST UINT8 *Data, UINTN Length)
{
  assert(Context==&Link); ++QrtrCalls;
  if(CR_ERROR(QrtrStatus)) return QrtrStatus;
  memcpy(Received,Data,Length); QrtrReceivedLength=Length;
  return CR_SUCCESS;
}
static void QrtrState(VOID *Context, BOOLEAN Online)
{ assert(Context==&Link); QrtrOnline=Online; ++QrtrTransitions; }
static CR_STATUS ReceiveLog(VOID *Context, CONST UINT8 *Data, UINTN Length)
{
  assert(Context==&Link && Length<=sizeof(LogReceived)); ++LogCalls;
  if(CR_ERROR(LogStatus)) return LogStatus;
  memcpy(LogReceived,Data,Length); LogReceivedLength=Length;
  return CR_SUCCESS;
}
static void LogState(VOID *Context, BOOLEAN Online)
{ assert(Context==&Link); LogOnline=Online; ++LogTransitions; }
static void Kick(VOID *Context) {
  assert(Context==&Link);
  if(ExpectedNotifyTail!=0) assert(Descriptor[2]==ExpectedNotifyTail);
  ++Kicks;
}
static void Put32(UINT8 *Dst, UINT32 Value) { memcpy(Dst,&Value,4); }
static void Packet(UINT16 Command, UINT16 Param1, UINT32 Param2,
                   CONST VOID *Data, UINTN Length)
{
  UINT8 PacketData[CR_GLINK_LOG_SIZE+32]={0};
  UINTN Size=(8+Length+7)&~(UINTN)7;
  assert(Size<=sizeof(PacketData));
  memcpy(PacketData,&Command,2); memcpy(PacketData+2,&Param1,2);
  Put32(PacketData+4,Param2); if(Length) memcpy(PacketData+8,Data,Length);
  for(UINTN I=0;I<Size;++I) Rx[(Descriptor[3]+I)%sizeof(Rx)]=PacketData[I];
  Descriptor[3]=(UINT32)((Descriptor[3]+Size)%sizeof(Rx));
}
static void Fixture(void)
{
  memset(&Link,0,sizeof(Link)); memset(Descriptor,0,sizeof(Descriptor));
  memset(Tx,0,sizeof(Tx)); memset(Rx,0,sizeof(Rx));
  Link.Receive=Receive; Link.Kick=Kick; Link.Context=&Link;
  Smem.Get=Get; Smem.Allocate=Allocate; Smem.Context=&Smem; ReceivedLength=Kicks=0;
  assert(CrGlinkBind(&Link,&Smem)==CR_SUCCESS);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  /* Independently encoded v1/INTENT_REUSE version frame. */
  static CONST UINT8 Version[]={0,0,1,0,1,0,0,0};
  assert(memcmp(Tx,Version,8)==0 && Kicks==1);
  Descriptor[0]=Descriptor[1];
  Packet(0,1,1,NULL,0); Packet(1,1,1,NULL,0);
  Packet(2,77,sizeof("PMIC_RTR_ADSP_APPS"),"PMIC_RTR_ADSP_APPS",sizeof("PMIC_RTR_ADSP_APPS"));
  Packet(4,1,0,NULL,0);
  UINT32 Intent[2]={1024,9}; Packet(5,77,1,Intent,sizeof(Intent));
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && Link.Ready && Link.Channels[0].RemoteId==77);
  Descriptor[0]=Descriptor[1];
}
static CR_STATUS PmicSend(VOID *Context, CONST VOID *Data, UINTN Length)
{
  (void)Context;
  if(CR_ERROR(PmicSendStatus)) return PmicSendStatus;
  ++PmicSends;
  memcpy(Sent,Data,Length); SentLength=Length;
  if(((CONST UINT8 *)Data)[8]==0x12) {
    assert(Length==(UcsiVersion>=0x200?544U:64U));
    if(Length==544) ++V2Writes;
  }
  return CR_SUCCESS;
}
static void PmicHeader(UINT8 *Data, UINT32 Owner, UINT32 Type, UINT32 Opcode)
{ Put32(Data,Owner); Put32(Data+4,Type); Put32(Data+8,Opcode); }
static void PmicRead(CR_PMIC_GLINK *Pmic, UINT32 Cci, UINT32 Status)
{
  UINT8 Reply[544]={0}; PmicHeader(Reply,32779,ReplyType,0x11);
  memcpy(Reply+12,&UcsiVersion,2); Put32(Reply+16,Cci);
  Put32(Reply+28,Status); Reply[32]=1; /* capability num_connectors */
  CrPmicReceive(Pmic,Reply,UcsiVersion>=0x200?544:64);
}
static void Advance(CR_PMIC_GLINK *Pmic, UINT32 Completion, UINT32 Connector)
{
  for(UINTN N=0;N<32;++N) {
    UINT64 TickUs = 1000+N > Pmic->NextPollUs ? 1000+N : Pmic->NextPollUs;
    assert(CrPmicTick(Pmic,TickUs)==CR_SUCCESS);
    if(Pmic->WritePending) {
      UINT8 Reply[16]={0}; PmicHeader(Reply,32779,ReplyType,0x12);
      CrPmicReceive(Pmic,Reply,sizeof(Reply));
    }
    if(Pmic->ReadPending) PmicRead(Pmic, (Pmic->Command&(1ULL << 63))?(1U << 29):Completion, Connector);
    if(Pmic->Operation==0) return;
  }
  assert(0);
}
static void WriteReply(CR_PMIC_GLINK *Pmic)
{
  UINT8 Reply[16]={0}; PmicHeader(Reply,32779,ReplyType,0x12);
  assert(Pmic->WritePending);
  CrPmicReceive(Pmic,Reply,sizeof(Reply));
}
static void Tick(CR_PMIC_GLINK *Pmic, UINT64 *Now)
{
  assert(CrPmicTick(Pmic,*Now)==CR_SUCCESS);
  *Now+=20000; /* the board monitor's nonblocking timer interval */
}
static void AwaitPending(CR_PMIC_GLINK *Pmic, UINT64 *Now, BOOLEAN Write)
{
  for(UINTN I=0;I<12;++I) {
    Tick(Pmic,Now);
    if(Write?Pmic->WritePending:Pmic->ReadPending) return;
  }
  assert(0);
}
static void ExpectControl(UINT64 Command)
{
  UINT64 Actual;
  assert(SentLength==(UcsiVersion>=0x200?544U:64U));
  memcpy(&Actual,Sent+20,sizeof(Actual));
  assert(Actual==Command);
}
static void PumpCommand(CR_PMIC_GLINK *Pmic, UINT64 *Now,
                        UINT32 Completion, UINT32 Connector)
{
  for(UINTN I=0;I<32;++I) {
    Tick(Pmic,Now);
    if(Pmic->WritePending) WriteReply(Pmic);
    if(Pmic->ReadPending)
      PmicRead(Pmic,(Pmic->Command&(1ULL << 63))?(1U << 29):Completion,Connector);
    if(Pmic->Operation==0) return;
  }
  assert(0);
}
static void NotificationDrivenStatus(void)
{
  CR_PMIC_GLINK Pmic;
  UINT8 Notify[24]={0}, Pan[32]={0};
  UINT64 Now=0;
  UcsiVersion=0x110;
  CrPmicInitialize(&Pmic,PmicSend,NULL);
  Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  PumpCommand(&Pmic,&Now,(1U << 27),0);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  PumpCommand(&Pmic,&Now,(1U << 31)|(16U<<8),0);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  PumpCommand(&Pmic,&Now,(1U << 31)|(9U<<8),(1U << 19)|(1U << 20)|(2U<<29));
  assert(Pmic.Stage==6 && Pmic.Operation==0 && !CrPmicNeedsService(&Pmic));
  UINTN Before=PmicSends;
  for(UINTN I=0;I<1000;++I) { Now+=1000000; assert(CrPmicTick(&Pmic,Now)==CR_SUCCESS); }
  assert(PmicSends==Before && Pmic.Operation==0 && !Pmic.StatusDirty);
  PmicHeader(Notify,32779,2,0x13); Put32(Notify+12,(1U << 31));
  CrPmicReceive(&Pmic,Notify,sizeof(Notify)); Tick(&Pmic,&Now);
  Put32(Notify+12,(1U << 29)); CrPmicReceive(&Pmic,Notify,sizeof(Notify)); Tick(&Pmic,&Now);
  assert(PmicSends==Before && !CrPmicNeedsService(&Pmic));
  Put32(Notify+12,2); CrPmicReceive(&Pmic,Notify,sizeof(Notify));
  assert(Pmic.StatusDirty && CrPmicNeedsService(&Pmic));
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(0x10012);
  assert(!Pmic.StatusDirty);
  CrPmicReceive(&Pmic,Notify,sizeof(Notify)); /* another detach during query */
  PumpCommand(&Pmic,&Now,(1U << 31)|(9U<<8),0);
  assert(Pmic.StatusDirty && !Pmic.State.Connected);
  PumpCommand(&Pmic,&Now,(1U << 31)|(9U<<8),(1U << 19)|(1U<<29));
  assert(!Pmic.StatusDirty && Pmic.State.Connected && Pmic.State.DataRole==UsbTypeCDevice);
  Before=PmicSends; Tick(&Pmic,&Now); assert(PmicSends==Before);
  assert(CrPmicRequestRole(&Pmic,FALSE,1)==CR_SUCCESS);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  assert(Pmic.StatusDirty);
  PumpCommand(&Pmic,&Now,(1U << 31)|(9U<<8),(1U << 19)|(2U<<29));
  assert(!Pmic.StatusDirty && Pmic.State.DataRole==UsbTypeCHost);

  PmicHeader(Pan,32780,2,0xff010016); Pan[13]=1; Pan[14]=3; Pan[20]=4|(1U << 6);
  CrPmicReceive(&Pmic,Pan,sizeof(Pan)); assert(Pmic.StatusDirty);
  Pmic.StatusDirty=FALSE;
  Pan[20]|=(1U << 7); CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(!Pmic.StatusDirty && Pmic.AltState.HpdIrqSequence==1);
  Pan[14]=0; Pan[13]=2; CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(Pmic.StatusDirty && !Pmic.State.DpActive && !Pmic.State.Usb3);
}
static void ResetAndPanRegressions(void)
{
  CR_PMIC_GLINK Pmic;
  UINT8 PanAck[12]={0}, Notify[24]={0};
  UINT64 Now=0, ResetDeadline;
  ReplyType=2; UcsiVersion=0x110;
  PmicHeader(PanAck,32780,2,0x15);
  CrPmicInitialize(&Pmic,PmicSend,NULL);
  Tick(&Pmic,&Now); CrPmicReceive(&Pmic,PanAck,sizeof(PanAck));
  AwaitPending(&Pmic,&Now,FALSE);
  ReplyType=3; PmicRead(&Pmic,(1U << 27),0);
  assert(!Pmic.ReadDone); /* an undefined envelope cannot complete a request */
  ReplyType=2; PmicRead(&Pmic,(1U << 27),0); Tick(&Pmic,&Now);
  assert(Pmic.ResetClearing && Pmic.Stage==1);
  /* Linux/Android clear stale Reset Complete using SET_NOTIFY_ENABLE(0),
     then issue PPM_RESET without an ordinary command ACK. */
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(5); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,(1U << 31),0); Tick(&Pmic,&Now);
  assert(!Pmic.ResetClearing && Pmic.Stage==1);
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(1);
  WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); ResetDeadline=Pmic.DeadlineUs;
  PmicRead(&Pmic,(1U << 31)|(1U << 30),0); Tick(&Pmic,&Now);
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(1);
  assert(Pmic.ResetWrites==2 && Pmic.DeadlineUs==ResetDeadline);
  WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,(1U << 27)|(1U << 31),0); Tick(&Pmic,&Now);
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(1);
  assert(Pmic.ResetWrites==3 && Pmic.DeadlineUs==ResetDeadline);
  WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,(1U << 27),0); Tick(&Pmic,&Now);
  assert(Pmic.Stage==2 && !Pmic.Faulted);

  /* A stale preliminary clear completion may time out, but a valid
     transport still permits the real RESET with its own bounded deadline. */
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL);
  Tick(&Pmic,&Now); CrPmicReceive(&Pmic,PanAck,sizeof(PanAck));
  PumpCommand(&Pmic,&Now,(1U << 27),0);
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(5); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,0,0);
  /* GLink delivers the valid read before this timer invocation, which
     occurs at the clear-loop deadline. It is not a missing read reply. */
  Now=Pmic.DeadlineUs; Tick(&Pmic,&Now);
  assert(!Pmic.ResetClearing && !Pmic.Faulted && Pmic.Stage==1);
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(1);
  WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); ResetDeadline=Pmic.DeadlineUs;
  PmicRead(&Pmic,(1U << 31),0); Tick(&Pmic,&Now);
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(1);
  assert(Pmic.DeadlineUs==ResetDeadline);
  assert(CrPmicTick(&Pmic,(ResetDeadline < Pmic.ReplyDeadlineUs ? ResetDeadline : Pmic.ReplyDeadlineUs)-1)==CR_SUCCESS && !Pmic.Faulted);
  assert(CrPmicTick(&Pmic,Pmic.ReplyDeadlineUs)==CR_TIMEOUT && Pmic.Faulted);

  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL);
  Tick(&Pmic,&Now); CrPmicReceive(&Pmic,PanAck,sizeof(PanAck));
  PumpCommand(&Pmic,&Now,(1U << 27),0);
  AwaitPending(&Pmic,&Now,TRUE); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE);
  assert(!Pmic.ReadDone && Pmic.ResetClearing);
  assert(CrPmicTick(&Pmic,Pmic.ReplyDeadlineUs)==CR_TIMEOUT && Pmic.Faulted);

  /* No PAN enable ACK: UCSI still initializes, reports a hub as host,
     processes detach and later reports a computer as the host partner. */
  Now=0; UcsiVersion=0x200;
  CrPmicInitialize(&Pmic,PmicSend,NULL);
  PumpCommand(&Pmic,&Now,0,0);
  PumpCommand(&Pmic,&Now,(1U << 27),0);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  PumpCommand(&Pmic,&Now,(1U << 31)|(16U<<8),0);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  PumpCommand(&Pmic,&Now,(1U << 31)|(9U<<8),(1U << 19)|(1U << 20)|(2U<<29));
  assert(Pmic.Stage==6 && Pmic.State.PdReady && Pmic.State.Connected &&
         Pmic.State.PowerSource && Pmic.State.DataRole==UsbTypeCHost);
  Now=Pmic.PanDeadlineUs; Tick(&Pmic,&Now);
  assert(Pmic.PanFaulted && Pmic.PanError==CR_TIMEOUT && !Pmic.Faulted &&
         Pmic.State.PdReady && Pmic.State.Connected &&
         Pmic.State.DataRole==UsbTypeCHost && Pmic.State.LastError==CR_SUCCESS);
  PmicHeader(Notify,32779,2,0x13); Put32(Notify+12,2);
  CrPmicReceive(&Pmic,Notify,sizeof(Notify));
  assert(Pmic.Notifications==1);
  PumpCommand(&Pmic,&Now,(1U << 31)|(9U<<8),0);
  assert(Pmic.State.PdReady && !Pmic.State.Connected &&
         !Pmic.State.PowerSource && Pmic.State.DataRole==UsbTypeCNone);
  CrPmicReceive(&Pmic,Notify,sizeof(Notify));
  PumpCommand(&Pmic,&Now,(1U << 31)|(9U<<8),(1U << 19)|(1U<<29));
  assert(Pmic.State.Connected && !Pmic.State.PowerSource &&
         Pmic.State.DataRole==UsbTypeCDevice && Pmic.PanFaulted && !Pmic.Faulted);
  ReplyType=1;
}
static void PanSurvivesUcsiFailure(void)
{
  CR_PMIC_GLINK Pmic;
  UINT8 Pan[32]={0}, Ack[12]={0}, Notify[24]={0};
  CrPmicInitialize(&Pmic,PmicSend,NULL);
  Pmic.PanEnabled=TRUE;
  CrPmicFail(&Pmic,CR_TIMEOUT);
  PmicHeader(Pan,32780,2,0xff010016);
  Pan[13]=1; Pan[14]=3; Pan[20]=4|(1U << 6);
  CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(Pmic.PanAcks==1 && Pmic.RxMessages==1);
  assert(!Pmic.State.PdReady && !Pmic.State.Connected && !Pmic.State.DpActive);
  assert(CrPmicTick(&Pmic,10)==CR_TIMEOUT);
  assert(Pmic.PanPending && SentLength==24 && Sent[12]==0x11);
  PmicHeader(Ack,32780,1,0x15);
  CrPmicReceive(&Pmic,Ack,sizeof(Ack));
  assert(CrPmicTick(&Pmic,20)==CR_TIMEOUT);
  assert(!Pmic.PanPending && Pmic.PanAcks==0 && !Pmic.PanFaulted);
  PmicHeader(Notify,32779,2,0x13); Put32(Notify+12,2);
  CrPmicReceive(&Pmic,Notify,sizeof(Notify));
  assert(Pmic.Notifications==1 && Pmic.Faulted);
  Pan[13]=2; Pan[14]=0; Pan[20]=0;
  CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(Pmic.PanAcks==1 && !Pmic.State.PdReady);
}
static void SlowPpmResetRegression(void)
{
  CR_PMIC_GLINK Pmic;
  UINT64 Now=0, Started;
  UcsiVersion=0x110; ReplyType=1;
  CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(1); WriteReply(&Pmic);
  Started=Now; Tick(&Pmic,&Now);
  assert(Pmic.DeadlineUs==Started+10000000);
  assert(Pmic.NextPollUs==Started+20000);
  assert(CrPmicTick(&Pmic,Started+19999)==CR_SUCCESS && !Pmic.ReadPending);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,0,0); Tick(&Pmic,&Now);
  /* Real ChargerPD capture: PPM reset completion arrives after five seconds
     while every GLink read/write replies normally. Do not restart RESET. */
  Now=Started+5500000;
  assert(CrPmicTick(&Pmic,Now)==CR_SUCCESS && !Pmic.Faulted);
  Now+=20000; AwaitPending(&Pmic,&Now,FALSE);
  PmicRead(&Pmic,(1U << 27),0); Tick(&Pmic,&Now);
  assert(Pmic.Stage==2 && Pmic.ResetWrites==1 && !Pmic.Faulted);
}

static void CompletionRetryRegressions(void)
{
  CR_PMIC_GLINK Pmic;
  UINT8 Pan[32]={0}, Ack[12]={0};
  UINT64 Now=0;
  UcsiVersion=0x110; ReplyType=1;
  CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0); /* VERSION probe */
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(1); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,0,0); Tick(&Pmic,&Now);
  assert(Pmic.Operation==3 && !Pmic.ReadPending && !Pmic.WritePending);
  Now=Pmic.DeadlineUs;
  assert(CrPmicTick(&Pmic,Now)==CR_TIMEOUT && Pmic.Faulted);
  assert(Pmic.RetryAtUs==Now+1000000 && Pmic.Retries==0);
  PmicHeader(Pan,32780,2,0xff010016); Pan[13]=1; Pan[14]=3; Pan[20]=4|(1U << 6)|(1U << 7);
  CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(Pmic.AltValid && Pmic.AltState.DpActive && !Pmic.State.DpActive);
  assert(CrPmicTick(&Pmic,Now+1)==CR_TIMEOUT && Pmic.PanPending);
  PmicHeader(Ack,32780,1,0x15); CrPmicReceive(&Pmic,Ack,sizeof(Ack));
  assert(CrPmicTick(&Pmic,Pmic.RetryAtUs-1)==CR_TIMEOUT && !Pmic.PanPending);
  Now=Pmic.RetryAtUs; Tick(&Pmic,&Now);
  assert(!Pmic.Faulted && Pmic.Retries==1 && Pmic.PanEnabled && Pmic.Stage==0);
  PumpCommand(&Pmic,&Now,0,0);
  PumpCommand(&Pmic,&Now,(1U << 27),0);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  PumpCommand(&Pmic,&Now,(1U << 31)|(16U<<8),0);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  PumpCommand(&Pmic,&Now,(1U << 31)|(9U<<8),(1U << 19)|(1U << 20)|(2U<<29));
  assert(Pmic.Stage==6 && Pmic.State.PdReady && Pmic.State.DataRole==UsbTypeCHost);
  assert(Pmic.State.DpActive && Pmic.State.Hpd && Pmic.State.Orientation==UsbTypeCReverse &&
         Pmic.State.HpdIrqSequence==1 && Pmic.PanAcks==0);

  /* Exactly two retries, with no request flood during the cooldown. */
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  for(UINTN Attempt=0;Attempt<3;++Attempt) {
    PumpCommand(&Pmic,&Now,0,0);
    AwaitPending(&Pmic,&Now,TRUE); WriteReply(&Pmic);
    AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,0,0); Tick(&Pmic,&Now);
    Now=Pmic.DeadlineUs;
    assert(CrPmicTick(&Pmic,Now)==CR_TIMEOUT && Pmic.Faulted && Pmic.Retries==Attempt);
    if(Attempt<2) {
      UINT32 Writes=Pmic.ResetWrites;
      for(UINTN I=0;I<10;++I) assert(CrPmicTick(&Pmic,Now+I)==CR_TIMEOUT);
      assert(Pmic.ResetWrites==Writes && Pmic.Faulted);
      Now=Pmic.RetryAtUs; Tick(&Pmic,&Now);
    }
  }
  assert(Pmic.RetryAtUs==0 && Pmic.Retries==2);
  assert(CrPmicTick(&Pmic,Now+100000000)==CR_TIMEOUT && Pmic.ResetWrites==3);

  /* Missing transport acknowledgements have no transaction ID to retry. */
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  AwaitPending(&Pmic,&Now,TRUE);
  assert(CrPmicTick(&Pmic,Pmic.ReplyDeadlineUs)==CR_TIMEOUT && Pmic.RetryAtUs==0);
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  AwaitPending(&Pmic,&Now,TRUE); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE);
  assert(CrPmicTick(&Pmic,Pmic.ReplyDeadlineUs)==CR_TIMEOUT && Pmic.RetryAtUs==0);
}
static void InFlightReadAtCompletionDeadline(void)
{
  CR_PMIC_GLINK Pmic;
  UINT64 Now=0, Deadline, ReplyDeadline;
  UcsiVersion=0x100; ReplyType=1;
  CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  AwaitPending(&Pmic,&Now,TRUE); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,0,0); Tick(&Pmic,&Now);
  Deadline=Pmic.DeadlineUs;
  Now=Deadline-20000; Tick(&Pmic,&Now);
  assert(Pmic.ReadPending && !Pmic.ReadDone);
  /* The command deadline is not the deadline for this new transport read.
     A response may be waiting in SMEM just after the monitor's RX poll. */
  assert(CrPmicTick(&Pmic,Deadline)==CR_SUCCESS && !Pmic.Faulted);
  PmicRead(&Pmic,(1U << 27),0);
  assert(CrPmicTick(&Pmic,Deadline+20000)==CR_SUCCESS && Pmic.Stage==2 && !Pmic.ReadPending);

  /* A reply already dispatched by the RX poll at the command deadline
     wins too; completed reads must not be classified as missing ACKs. */
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  AwaitPending(&Pmic,&Now,TRUE); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,(1U << 27),0);
  assert(CrPmicTick(&Pmic,Pmic.DeadlineUs)==CR_SUCCESS && Pmic.Stage==2);

  /* CCI still zero after the late transport reply: drain it first, then
     the existing completion-only retry can safely restart negotiation. */
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  AwaitPending(&Pmic,&Now,TRUE); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,0,0); Tick(&Pmic,&Now);
  Deadline=Pmic.DeadlineUs; Now=Deadline-20000; Tick(&Pmic,&Now);
  ReplyDeadline=Pmic.ReplyDeadlineUs;
  assert(ReplyDeadline==Deadline-20000+5000000);
  assert(CrPmicTick(&Pmic,Deadline)==CR_SUCCESS);
  PmicRead(&Pmic,0,0);
  assert(CrPmicTick(&Pmic,Deadline+20000)==CR_SUCCESS && !Pmic.ReadPending);
  assert(CrPmicTick(&Pmic,Deadline+40000)==CR_TIMEOUT && Pmic.Faulted);
  assert(Pmic.RetryAtUs==Deadline+40000+1000000);

  /* A genuinely missing transport reply stays terminal, but gets the
     complete transport interval independent of the command deadline. */
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  AwaitPending(&Pmic,&Now,TRUE); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,0,0); Tick(&Pmic,&Now);
  Deadline=Pmic.DeadlineUs; Now=Deadline-20000; Tick(&Pmic,&Now);
  ReplyDeadline=Pmic.ReplyDeadlineUs;
  assert(CrPmicTick(&Pmic,ReplyDeadline-1)==CR_SUCCESS && !Pmic.Faulted);
  assert(CrPmicTick(&Pmic,ReplyDeadline)==CR_TIMEOUT && Pmic.RetryAtUs==0);

  /* A delivered write ACK starts the ten-second PPM completion interval.
     Time spent acquiring/sending the write cannot consume that interval. */
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  AwaitPending(&Pmic,&Now,TRUE);
  Now=Pmic.ReplyDeadlineUs-1; WriteReply(&Pmic);
  assert(CrPmicTick(&Pmic,Now)==CR_SUCCESS && Pmic.CompletionStarted);
  assert(Pmic.DeadlineUs==Now+10000000 && !Pmic.WritePending);
  Now+=20000; AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,(1U << 27),0);
  Tick(&Pmic,&Now); assert(Pmic.Stage==2 && !Pmic.Faulted);

  /* Normal command completion at its deadline still gets a separate ACK
     write/read interval, preserving real connected status and USB roles. */
  Now=0; CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PumpCommand(&Pmic,&Now,0,0);
  PumpCommand(&Pmic,&Now,(1U << 27),0);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  PumpCommand(&Pmic,&Now,(1U << 31)|(16U<<8),0);
  PumpCommand(&Pmic,&Now,(1U << 31),0);
  AwaitPending(&Pmic,&Now,TRUE); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE);
  Now=Pmic.DeadlineUs;
  PmicRead(&Pmic,(1U << 31)|(9U<<8),(1U << 19)|(1U << 20)|(2U<<29));
  Tick(&Pmic,&Now);
  assert(Pmic.State.PdReady && Pmic.State.DataRole==UsbTypeCHost);
  AwaitPending(&Pmic,&Now,TRUE); ExpectControl(0x20004); WriteReply(&Pmic);
  AwaitPending(&Pmic,&Now,FALSE); PmicRead(&Pmic,(1U << 29),0);
  assert(CrPmicTick(&Pmic,Pmic.DeadlineUs)==CR_SUCCESS && Pmic.Stage==6 && !Pmic.Faulted);

  /* VERSION intent acquisition is not a timed-out PPM command. It must
     not consume completion retries when no write was acknowledged. */
  CrPmicInitialize(&Pmic,PmicSend,NULL); Pmic.PanEnabled=TRUE;
  PmicSendStatus=CR_NOT_READY;
  assert(CrPmicTick(&Pmic,0)==CR_SUCCESS);
  assert(CrPmicTick(&Pmic,20000)==CR_SUCCESS && !Pmic.ReadPending);
  assert(CrPmicTick(&Pmic,5000000)==CR_TIMEOUT && Pmic.Faulted && Pmic.RetryAtUs==0);
  PmicSendStatus=CR_SUCCESS;
}
static void LogChannelRegressions(void)
{
  static UINT8 Data[CR_GLINK_LOG_SIZE], Chunk[CR_GLINK_LOG_SIZE+8];
  UINT8 Request[16]={0};
  UINT32 RemoteIntent[2]={16,9};
  Fixture(); Link.LogsReceive=ReceiveLog; Link.LogsState=LogState;
  LogCalls=LogReceivedLength=LogTransitions=0; LogOnline=FALSE; LogStatus=CR_SUCCESS;
  UINT32 Start=Descriptor[1];
  Packet(2,88,sizeof("PMIC_LOGS_ADSP_APPS"),"PMIC_LOGS_ADSP_APPS",sizeof("PMIC_LOGS_ADSP_APPS"));
  Packet(4,2,0,NULL,0); Packet(5,88,1,RemoteIntent,sizeof(RemoteIntent));
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && LogOnline && LogTransitions==1 && Link.Ready);
  assert(Link.Channels[1].Logs && !Link.Channels[1].Pmic && !Link.Channels[1].Qrtr);
  UINT32 At=Start, Advertisements=0;
  while(At!=Descriptor[1]) {
    UINT16 Cmd, Local; UINT32 Param;
    memcpy(&Cmd,Tx+At,2); memcpy(&Local,Tx+At+2,2); memcpy(&Param,Tx+At+4,4);
    UINTN Length=Cmd==2?Param:Cmd==5?Param*8:0;
    if(Cmd==5) {
      UINT32 Size, Id; memcpy(&Size,Tx+At+8,4); memcpy(&Id,Tx+At+12,4);
      assert(Local==2 && Param==1 && Size==8204 && Id==1); ++Advertisements;
    }
    At=(UINT32)((At+((8+Length+7)&~(UINTN)7))%sizeof(Tx));
  }
  assert(Advertisements==1);
  assert(CrGlinkSendLogs(&Link,Request,sizeof(Request))==CR_SUCCESS);
  assert(Link.Channels[1].Remote[0].Busy && !Link.Channels[0].Remote[0].Busy);
  Packet(14,88,9,NULL,0); assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  for(UINTN I=0;I<sizeof(Data);++I) Data[I]=(UINT8)(I%251);
  Put32(Chunk,3500); Put32(Chunk+4,sizeof(Data)-3500); memcpy(Chunk+8,Data,3500);
  Packet(9,88,1,Chunk,3508); assert(CrGlinkPoll(&Link)==CR_SUCCESS && LogCalls==0);
  Put32(Chunk,sizeof(Data)-3500); Put32(Chunk+4,0);
  memcpy(Chunk+8,Data+3500,sizeof(Data)-3500);
  LogStatus=CR_NOT_READY; UINT32 Tail=Descriptor[2];
  Packet(12,88,1,Chunk,sizeof(Data)-3500+8);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && Descriptor[2]==Tail && LogCalls==1);
  assert(Link.Channels[1].LogOffset==3500);
  LogStatus=CR_SUCCESS;
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && LogCalls==2 && LogReceivedLength==sizeof(Data));
  assert(memcmp(LogReceived,Data,sizeof(Data))==0 && ReceivedLength==0);
  /* One full-size log frame wraps; PMIC/QRTR retain their 1024-byte limit. */
  Descriptor[2]=Descriptor[3]=sizeof(Rx)-24;
  Put32(Chunk,sizeof(Data)); Put32(Chunk+4,0); memcpy(Chunk+8,Data,sizeof(Data));
  Packet(9,88,1,Chunk,sizeof(Data)+8);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && LogCalls==3 &&
         memcmp(LogReceived,Data,sizeof(Data))==0);
  Packet(3,88,0,NULL,0); Packet(11,2,0,NULL,0);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && !LogOnline && LogTransitions==2 && Link.Ready);
  CrGlinkReset(&Link);
  assert(LogTransitions==3 && Link.LogsReceive==ReceiveLog && Link.LogsState==LogState);
  Fixture(); Put32(Chunk,1025); Put32(Chunk+4,0);
  Packet(9,77,1,Chunk,1033);
  assert(CrGlinkPoll(&Link)==CR_COMPROMISED_DATA);
}
static void QrtrChannelRegressions(void)
{
  UINT8 Payload[12]={1,2,3,4,5,6,7,8,9,10,11,12}, Chunk[20]={0};
  UINT32 Intents[128];
  Fixture(); Link.QrtrReceive=ReceiveQrtr; Link.QrtrState=QrtrState;
  QrtrCalls=QrtrReceivedLength=QrtrTransitions=0; QrtrStatus=CR_SUCCESS; QrtrOnline=FALSE;
  assert(CrGlinkSendQrtr(&Link,Payload,12)==CR_NOT_READY);
  UINT32 Start=Descriptor[1];
  Packet(2,88,7,"IPCRTR",7); Packet(4,2,0,NULL,0);
  for(UINT32 I=0;I<64;++I) { Intents[2*I]=1024; Intents[2*I+1]=9+I; }
  Packet(5,88,64,Intents,sizeof(Intents));
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && QrtrOnline && QrtrTransitions==1 && Link.Ready);
  assert(Link.Channels[1].Qrtr && !Link.Channels[1].Pmic && Link.Channels[1].Remote[63].Id==72);
  UINT32 At=Start, Advertisements=0;
  while(At!=Descriptor[1]) {
    UINT16 Cmd, Local; UINT32 Param;
    memcpy(&Cmd,Tx+At,2); memcpy(&Local,Tx+At+2,2); memcpy(&Param,Tx+At+4,4);
    UINTN Length=Cmd==2?Param:Cmd==5?Param*8:0;
    if(Cmd==5) {
      assert(Local==2 && Param==1);
      UINT32 Size, Id; memcpy(&Size,Tx+At+8,4); memcpy(&Id,Tx+At+12,4);
      assert(Size==1024 && Id==++Advertisements);
    }
    At=(UINT32)((At+((8+Length+7)&~(UINTN)7))%sizeof(Tx));
  }
  assert(Advertisements==4);
  assert(CrGlinkSendQrtr(&Link,Payload,12)==CR_SUCCESS);
  assert(Link.Channels[1].Remote[0].Busy && !Link.Channels[0].Remote[0].Busy);
  assert(CrGlinkSend(&Link,Payload,12)==CR_SUCCESS && Link.Channels[0].Remote[0].Busy);
  Packet(14,88,9,NULL,0); assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  assert(!Link.Channels[1].Remote[0].Busy && Link.Channels[0].Remote[0].Busy);
  Packet(14,77,9,NULL,0); assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  /* A blocked service retains the final fragment and its preceding bytes. */
  Put32(Chunk,4); Put32(Chunk+4,8); memcpy(Chunk+8,Payload,4);
  Packet(9,88,1,Chunk,12); assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  assert(Link.Channels[1].Local[0].Offset==4 && QrtrCalls==0);
  QrtrStatus=CR_NOT_READY; Put32(Chunk,8); Put32(Chunk+4,0); memcpy(Chunk+8,Payload+4,8);
  UINT32 Tail=Descriptor[2]; Packet(12,88,1,Chunk,16);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && Descriptor[2]==Tail && QrtrCalls==1);
  assert(Link.Channels[1].Local[0].Offset==4 && Link.Channels[1].Local[0].Remaining==8);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && Descriptor[2]==Tail && QrtrCalls==2);
  QrtrStatus=CR_SUCCESS;
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && Descriptor[2]==Descriptor[3] && QrtrCalls==3);
  assert(QrtrReceivedLength==12 && ReceivedLength==0 && memcmp(Received,Payload,12)==0);
  Put32(Chunk,12); Put32(Chunk+4,0); memcpy(Chunk+8,Payload,12);
  Packet(9,77,1,Chunk,20); assert(CrGlinkPoll(&Link)==CR_SUCCESS && ReceivedLength==12 && QrtrCalls==3);
  assert(CrGlinkSendQrtr(&Link,Payload,12)==CR_SUCCESS);
  Packet(3,88,0,NULL,0); Packet(14,88,9,NULL,0);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && !QrtrOnline && Link.Ready && Link.Generation==0);
  assert(CrGlinkSendQrtr(&Link,Payload,12)==CR_NOT_READY);
  Packet(11,2,0,NULL,0); Packet(2,89,7,"IPCRTR",7); Packet(4,2,0,NULL,0);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && QrtrOnline && QrtrTransitions==3 && Link.Ready);
  CrGlinkReset(&Link);
  assert(!QrtrOnline && QrtrTransitions==4 && Link.QrtrReceive==ReceiveQrtr && Link.QrtrState==QrtrState);
}
int main(void)
{
  UINT8 Chunk[16]={0}, Payload[12]={1,2,3,4,5,6,7,8,9,10,11,12};
  Fixture();
  ExpectedNotifyTail=(Descriptor[2]+8)%sizeof(Rx);
  Packet(13,0,0,NULL,0); assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  ExpectedNotifyTail=0;
  assert(CrGlinkSend(&Link,Payload,sizeof(Payload))==CR_SUCCESS);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && Link.Channels[0].Remote[0].Busy);
  Packet(14,77,9,NULL,0); assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  assert(!Link.Channels[0].Remote[0].Busy);
  assert(CrGlinkSend(&Link,Payload,sizeof(Payload))==CR_SUCCESS);
  assert(CrGlinkSend(&Link,Payload,sizeof(Payload))==CR_NOT_READY);
  assert(Link.Channels[0].IntentRequested);
  Packet(8,77,0,NULL,0); /* intent ACK carries the sender's remote channel ID */
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && !Link.Channels[0].IntentRequested);
  assert(CrGlinkSend(&Link,Payload,sizeof(Payload))==CR_NOT_READY);
  Packet(8,77,1,NULL,0); assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  assert(Link.Channels[0].IntentRequested);
  UINT32 NewIntent[2]={1024,10}; Packet(5,77,1,NewIntent,sizeof(NewIntent));
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && !Link.Channels[0].IntentRequested);
  assert(CrGlinkSend(&Link,Payload,sizeof(Payload))==CR_SUCCESS);
  Packet(14,77,9,NULL,0); Packet(14,77,10,NULL,0);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  /* RX payload and command can wrap independently at FIFO end. */
  Descriptor[2]=Descriptor[3]=sizeof(Rx)-8;
  Put32(Chunk,4); Put32(Chunk+4,8); memcpy(Chunk+8,Payload,4);
  Packet(9,77,1,Chunk,12); assert(CrGlinkPoll(&Link)==CR_SUCCESS && ReceivedLength==0);
  Put32(Chunk,8); Put32(Chunk+4,0); memcpy(Chunk+8,Payload+4,8);
  Packet(12,77,1,Chunk,16); assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  assert(ReceivedLength==12 && memcmp(Received,Payload,12)==0);
  /* Backpressure retains replies, never overwrites the remote's unread TX. */
  Descriptor[0]=(Descriptor[1]+16)%sizeof(Tx);
  assert(CrGlinkSend(&Link,Payload,12)==CR_SUCCESS);
  UINT32 Head=Descriptor[1]; assert(CrGlinkPoll(&Link)==CR_SUCCESS);
  assert(Descriptor[1]==(Head+8)%sizeof(Tx) && Link.QueueCount==1 && Link.ReadNotify);
  assert(Tx[Head]==13 && Tx[Head+1]==0); /* wake a blocked remote reader */
  Head=Descriptor[1]; assert(CrGlinkPoll(&Link)==CR_SUCCESS && Descriptor[1]==Head);
  Descriptor[0]=Head; assert(CrGlinkPoll(&Link)==CR_SUCCESS && Link.QueueCount==0);
  Packet(3,77,0,NULL,0); assert(CrGlinkPoll(&Link)==CR_SUCCESS && !Link.Ready);
  assert(Link.Generation==1 && Link.Channels[0].Closing);
  Packet(14,77,9,NULL,0); /* completion from the closing session is harmless */
  Packet(11,1,0,NULL,0);
  Packet(2,78,sizeof("PMIC_RTR_ADSP_APPS"),"PMIC_RTR_ADSP_APPS",sizeof("PMIC_RTR_ADSP_APPS"));
  Packet(4,1,0,NULL,0);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && Link.Ready && Link.Generation==1);
  Packet(0,1,0,NULL,0); Packet(1,1,0,NULL,0);
  assert(CrGlinkPoll(&Link)==CR_SUCCESS && !Link.Ready && Link.Generation==2);
  assert(Link.VersionAck && Link.Features==0 && Link.Channels[0].RemoteId==0);
  Fixture(); Descriptor[3]=sizeof(Rx); assert(CrGlinkPoll(&Link)==CR_COMPROMISED_DATA);
  Fixture(); Put32(Chunk,1024); Put32(Chunk+4,1);
  Packet(9,77,1,Chunk,8); /* incomplete frame is not consumed */
  UINT32 Tail=Descriptor[2]; assert(CrGlinkPoll(&Link)==CR_SUCCESS && Descriptor[2]==Tail);
  Fixture(); UINT8 Huge[1032]={0}; Put32(Huge,1024); Put32(Huge+4,1);
  Packet(9,77,1,Huge,sizeof(Huge)); assert(CrGlinkPoll(&Link)==CR_COMPROMISED_DATA);

  CR_PMIC_GLINK Pmic; UINT8 Pan[32]={0};
  CrPmicInitialize(&Pmic,PmicSend,NULL);
  assert(CrPmicTick(&Pmic,0)==CR_SUCCESS);
  assert(SentLength==24 && Sent[0]==0x0c && Sent[1]==0x80 && Sent[12]==0x10);
  UINT8 PanAck[12]={0}; PmicHeader(PanAck,32780,1,0x15);
  CrPmicReceive(&Pmic,PanAck,sizeof(PanAck));
  Advance(&Pmic,0,0); assert(Pmic.Version==0x110 && Pmic.Stage==1);
  Advance(&Pmic,(1U << 27),0); assert(Pmic.Stage==2);
  Advance(&Pmic,(1U << 31),0); assert(Pmic.Stage==3);
  Advance(&Pmic,(1U << 31)|(16U<<8),0); assert(Pmic.Stage==4);
  Advance(&Pmic,(1U << 31),0); assert(Pmic.Stage==5);
  Advance(&Pmic,(1U << 31)|(9U<<8),(1U << 19)|(1U << 20)|(2U<<29));
  assert(Pmic.State.PdReady && Pmic.State.Connected && Pmic.State.PowerSource);
  assert(Pmic.State.DataRole==UsbTypeCHost && Pmic.Stage==6);
  PmicHeader(Pan,32780,2,0xff010016); Pan[13]=1; Pan[14]=3; Pan[20]=4|(1U << 6);
  CrPmicReceive(&Pmic,Pan,31); assert(!Pmic.State.DpActive);
  CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(Pmic.State.DpActive && Pmic.State.Hpd && Pmic.State.DpLanes==2 &&
         Pmic.State.Orientation==UsbTypeCReverse && Pmic.State.Usb3);
  Pan[20]|=(1U << 7); UINT32 Seq=Pmic.State.Sequence;
  CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(Pmic.State.HpdIrqSequence==1 && Pmic.State.Sequence==Seq+1);
  assert(CrPmicRequestRole(&Pmic,FALSE,2)==CR_SUCCESS);
  assert(Pmic.State.DataRole==UsbTypeCHost); /* request is not a negotiated state */
  assert(CrPmicRequestRole(&Pmic,FALSE,1)==CR_ALREADY_STARTED);
  Pan[14]=0; Pan[13]=2; Pan[20]=0; CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(!Pmic.State.DpActive && !Pmic.State.Hpd && !Pmic.State.Usb3);
  assert(CrPmicTick(&Pmic,6000000)==CR_SUCCESS); /* sends pending PAN ack */
  assert(CrPmicTick(&Pmic,12000000)==CR_TIMEOUT);
  assert(!Pmic.State.PdReady && !Pmic.State.Connected && Pmic.Faulted);
  CrPmicInitialize(&Pmic,PmicSend,NULL);
  Pmic.State.PdReady=Pmic.State.Connected=TRUE;
  Pmic.State.LastError=CR_SUCCESS;
  Pan[13]=0; Pan[14]=3; Pan[20]=4|(1U << 6);
  for(UINTN I=0;I<17;++I) CrPmicReceive(&Pmic,Pan,sizeof(Pan));
  assert(Pmic.PanFaulted && !Pmic.Faulted && Pmic.State.Connected && !Pmic.State.DpActive);
  assert(Pmic.PanError==CR_OUT_OF_RESOURCES && CrPmicTick(&Pmic,0)==CR_SUCCESS);
  CrPmicInitialize(&Pmic,PmicSend,NULL);
  UcsiVersion=0x200;
  assert(CrPmicTick(&Pmic,0)==CR_SUCCESS);
  CrPmicReceive(&Pmic,PanAck,sizeof(PanAck));
  Advance(&Pmic,0,0);
  assert(Pmic.Version==0x200 && Pmic.BufferSize==528);
  Advance(&Pmic,(1U << 27),0);
  assert(V2Writes==1); /* v2 writes include the full message-in/out region */
  Advance(&Pmic,(1U << 31),0);
  Advance(&Pmic,(1U << 31)|(16U<<8),0);
  Advance(&Pmic,(1U << 31),0);
  Advance(&Pmic,(1U << 31)|(9U<<8),(1U << 19)|(1U<<29));
  assert(Pmic.State.PdReady && Pmic.State.DataRole==UsbTypeCDevice);
  ResetAndPanRegressions();
  PanSurvivesUcsiFailure();
  SlowPpmResetRegression();
  CompletionRetryRegressions();
  InFlightReadAtCompletionDeadline();
  NotificationDrivenStatus();
  QrtrChannelRegressions();
  LogChannelRegressions();
  for(BadItem=478;BadItem<=479;++BadItem) {
    CrGlinkReset(&Link);
    assert(CrGlinkBind(&Link,&Smem)==CR_COMPROMISED_DATA && !Link.Bound);
  }
  BadItem=0; Fixture(); Link.Rx=NULL; BadItem=480;
  assert(CrGlinkPoll(&Link)==CR_COMPROMISED_DATA);
  BadItem=0;
  puts("Type-C transport: wire handshake, bounds, stale-reset recovery, independent reply/completion deadlines, bounded retries, PAN isolation and USB hotplug passed");
  return 0;
}
