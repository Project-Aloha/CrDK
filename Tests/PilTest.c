/** Independent ELF/PAS/SMP2P/QMP fixtures. SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <Uefi.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <Library/ArmSmcLib.h>
#include <Library/PILCrUefiLib.h>

static UINT32 Out[85], In[85], Qmp[256];
static UINT32 Votes[3], FailVote, VoteCalls, FailPas, PasCalls[8], Reads;
static BOOLEAN NoReady, NoAck, BadRemote, NoQmp, NoMsgAck, Busy, Interrupt, Smc32;
static BOOLEAN NoConvention, AlwaysBusy, AlwaysInterrupted;
static UINT32 CapabilityCalls, SupportCalls, Probe64Calls, Probe32Calls, FailTransportCommand;
static INT32 CapabilityTransport, SupportTransport, FailTransport;
static UINTN CapabilityResult, SupportResult;
static UINTN ResumeMetadata;
static BOOLEAN Crypto;
static EFI_STATUS FileStatus;
static UINT64 Delay;
static UINTN CallerEl = 8;
static UINT8 Header[1024], Memory[0x4000];
static CR_PIL_IMAGE Image;
static CR_PIL_CONTEXT Pil;
static VOID *Metadata;
static CONST CR_PIL_TARGET Target = {
  .PasId=1, .MemoryBase=0x85e00000, .MemorySize=sizeof(Memory),
  .SmemHost=2, .SmemIn=443, .SmemOut=429,
  .IpccSend=0xed1800c, .IpccClient=3, .QmpBase=0xc300000, .QmpSize=0x400,
  .ImagePath=L"\\image\\adsp", .InterconnectProvider="qcom,sm8450-pil",
  .CryptoSource=0x7000, .CryptoDestination=0x7001,
};
static EFI_SMEM_CR_PROTOCOL Smem;
static EFI_CMD_DB_PROTOCOL Db;
static EFI_RPMH_CR_PROTOCOL Rpmh;
static EFI_INTERCONNECT_CR_PROTOCOL Icb;

VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memmove(D,S,N); }
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) { return memset(D,V,N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D,0,N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A,B,N); }
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S) { return strlen(S); }
UINTN ArmReadCurrentEL(VOID) {return CallerEl;}
VOID EFIAPI ArmDataSynchronizationBarrier(VOID) {}
VOID EFIAPI MemoryFence(VOID) {}
UINTN EFIAPI MicroSecondDelay(UINTN N) { Delay+=N; return N; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
UINT32 EFIAPI MmioRead32(UINTN A)
{ assert(A>=0xc300000 && A<0xc300400 && (A&3)==0); return Qmp[(A-0xc300000)/4]; }
UINT32 EFIAPI MmioWrite32(UINTN A, UINT32 V)
{
  if(A==0xed1800c) {
    if(V==0) {
      if(!NoQmp) { Qmp[0x28/4]=Qmp[0x24/4]; Qmp[0x14/4]=0xffff; Qmp[0x30/4]=Qmp[0x2c/4]; }
      if(Qmp[0x80/4]==64 && !NoMsgAck) {
        assert(memcmp((UINT8 *)Qmp+0x84,"{class: image, res: load_state, name: adsp, val: ",46)==0);
        Qmp[0x80/4]=0;
      }
    } else {
      assert(V==((3U<<16)|2));
      if((Out[9]&1)!=0 && !NoAck) In[9]|=8;
    }
    return V;
  }
  assert(A>=0xc300000 && A<0xc300400 && (A&3)==0);
  Qmp[(A-0xc300000)/4]=V; return V;
}
static void Remote(UINT32 State)
{
  In[0]=0x504d5324; In[1]=1|0x100; In[2]=2; In[3]=16|(1U<<16);
  memcpy((UINT8 *)In+20,"slave-kernel",12); In[9]=State;
}
VOID ArmCallSmc(ARM_SMC_ARGS *A)
{
  assert(A->Arg8==0 && A->Arg9==0 && A->Arg10==0 && A->Arg11==0 &&
         A->Arg12==0 && A->Arg13==0 && A->Arg14==0 && A->Arg15==0 && A->Arg16==0 && A->Arg17==0);
  UINT32 Function=(UINT32)A->Arg0 & 0xffff;
  if(A->Arg0==1) {
    assert(Interrupt && A->Arg6==0x1234 && A->Arg1==0x82 && A->Arg2==1 && A->Arg3==ResumeMetadata);
    if(AlwaysInterrupted) { A->Arg0=1; A->Arg6=0x1234; return; }
    A->Arg0=A->Arg1=0; Interrupt=FALSE; return;
  }
  assert(A->Arg5==0 && A->Arg6==0 && A->Arg7==0);
  if(Function==0x601) {
    assert(A->Arg1==1 && A->Arg3==0 && A->Arg4==0);
    /* Literal kernel-derived wire IDs, independent of PIL's definitions:
       convention probes are FAST; availability checks are standard and
       their argument omits both the FAST and SMC64 flags. */
    if(A->Arg0==0xc2000601 || A->Arg0==0x82000601) {
      assert(A->Arg2==0x02000601);
      if(A->Arg0==0xc2000601) ++Probe64Calls; else ++Probe32Calls;
      if(NoConvention || (Smc32 && A->Arg0==0xc2000601)) { A->Arg0=(UINTN)-4; return; }
      assert(A->Arg0==(Smc32?0x82000601:0xc2000601));
      A->Arg0=0; A->Arg1=1; return;
    }
    assert(A->Arg0==(Smc32?0x02000601:0x42000601) && A->Arg2==0x02000207);
    ++CapabilityCalls;
    A->Arg0=(UINTN)(INTN)CapabilityTransport; A->Arg1=CapabilityResult; return;
  }
  assert((A->Arg0 & 0xffff0000U)==(Smc32?0x02000000:0x42000000));
  assert(A->Arg2==1);
  if(Function==0x207) {
    assert(A->Arg1==1 && A->Arg3==0 && A->Arg4==0 && CapabilityCalls==1);
    ++SupportCalls; A->Arg0=(UINTN)(INTN)SupportTransport; A->Arg1=SupportResult; return;
  }
  assert(Crypto);
  UINT32 Command=Function&255U;
  assert(Command<8); ++PasCalls[Command];
  if(Command==1) {
    assert(A->Arg1==0x82 && A->Arg3==(UINTN)Metadata && (A->Arg3&4095)==0);
    assert(Votes[0]==3 && Votes[1]==3 && Votes[2]==3);
    if(Interrupt) { ResumeMetadata=A->Arg3; A->Arg0=1; A->Arg6=0x1234; A->Arg1=99; A->Arg3=0; return; }
  } else if(Command==2) {
    assert(A->Arg1==3 && A->Arg3==0x85e00000 && A->Arg4==0x3000);
  } else {
    assert(A->Arg1==1 && A->Arg3==0 && A->Arg4==0);
  }
  if(Command==5 && (Busy || AlwaysBusy)) { Busy=FALSE; A->Arg0=(UINTN)-12; return; }
  if(Command==FailTransportCommand) { A->Arg0=(UINTN)(INTN)FailTransport; A->Arg1=1; return; }
  A->Arg0=0; A->Arg1=FailPas==Command?(UINTN)-22:0;
  if(Command==5 && FailPas!=5 && !NoReady) Remote(BadRemote?3:2);
}
static EFI_STATUS EFIAPI Item(EFI_SMEM_CR_PROTOCOL *This, UINT32 Host, UINT32 Id, UINT32 *Size, VOID **Address)
{
  assert(This==&Smem && Host==2); *Size=sizeof(In);
  if(Id==443) { if(In[0]==0) return EFI_NOT_FOUND; *Address=In; }
  else { assert(Id==429); *Address=Out; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Allocate(EFI_SMEM_CR_PROTOCOL *This, UINT32 Host, UINT32 Id, UINT32 Size, VOID **Address)
{ assert(This==&Smem && Host==2 && Id==429 && Size==340); *Address=Out; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Address(EFI_CMD_DB_PROTOCOL *This, CONST CHAR8 *Name, UINT32 *Value)
{
  assert(This==&Db);
  *Value=!strcmp(Name,"lmx.lvl")?0x100:!strcmp(Name,"lcx.lvl")?0x104:0x108;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Aux(EFI_CMD_DB_PROTOCOL *This, CONST CHAR8 *Name, UINT8 *Data, UINT32 *Length)
{
  assert(This==&Db && (!strcmp(Name,"lcx.lvl") || !strcmp(Name,"lmx.lvl")));
  assert(*Length>=10); static const UINT8 Map[]={0,0,16,0,128,0,128,1,0,0};
  memcpy(Data,Map,sizeof(Map)); *Length=sizeof(Map); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_RPMH_CR_PROTOCOL *This, VOID *Owner, UINT32 Address, UINT32 Value)
{
  assert(This==&Rpmh && Owner==&Pil);
  assert(Address>=0x100 && Address<=0x108); UINT32 Index=(Address-0x100)/4;
  Votes[Index]=Value;
  return ++VoteCalls==FailVote?EFI_TIMEOUT:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Acquire(EFI_INTERCONNECT_CR_PROTOCOL *This, CONST CHAR8 *Provider,
                                 UINT32 Source, UINT32 Destination, INTERCONNECT_PATH_HANDLE *Path)
{ assert(This==&Icb && !strcmp(Provider,"qcom,sm8450-pil") && Source==0x7000 && Destination==0x7001); Crypto=TRUE; *Path=1; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Bandwidth(EFI_INTERCONNECT_CR_PROTOCOL *This, INTERCONNECT_PATH_HANDLE Path, UINT64 Avg, UINT64 Peak)
{ assert(This==&Icb && Path==1 && Avg==0 && Peak==MAX_UINT32); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Release(EFI_INTERCONNECT_CR_PROTOCOL *This, INTERCONNECT_PATH_HANDLE Path)
{ assert(This==&Icb && Path==1); Crypto=FALSE; return EFI_SUCCESS; }
static EFI_STATUS Segments(VOID *Context, UINT32 Index, VOID *Buffer, UINT32 Size)
{
  assert(Context==&Reads && (Index==1 || Index==2 || Index==3)); ++Reads;
  if(EFI_ERROR(FileStatus)) return FileStatus;
  memset(Buffer,Index==1?0x88:Index==2?0xaa:0xbb,Size); return EFI_SUCCESS;
}
static void Put16(UINT8 *P, UINT16 V) { P[0]=(UINT8)V; P[1]=(UINT8)(V>>8); }
static void Put32(UINT8 *P, UINT32 V) { for(UINTN N=0;N<4;++N) P[N]=(UINT8)(V>>(8*N)); }
static void Put64(UINT8 *P, UINT64 V) { for(UINTN N=0;N<8;++N) P[N]=(UINT8)(V>>(8*N)); }
static UINTN Firmware(BOOLEAN Elf64)
{
  memset(Header,0,sizeof(Header));
  memcpy(Header,"\177ELF",4); Header[4]=Elf64?2:1; Header[5]=Header[6]=1;
  Put32(Header+20,1);
  UINT32 H=Elf64?64:52, Ph=Elf64?56:32, End=H+4*Ph;
  if(Elf64) Put64(Header+32,H); else Put32(Header+28,H);
  Put16(Header+(Elf64?52:40),(UINT16)H); Put16(Header+(Elf64?54:42),(UINT16)Ph);
  Put16(Header+(Elf64?56:44),4);
  for(UINT32 Index=0;Index<4;++Index) {
    UINT8 *P=Header+H+Index*Ph;
    UINT32 F=Index==1?(2U<<24):Index>=2?((1U<<27)|5U):0;
    UINT64 A=Index>=2?0x100000+(Index-2)*0x2000:0;
    UINT64 O=Index==0?0:Index==1?0x1000:Index==2?0x2000:0x3000;
    UINT32 S=Index==0?End:Index==1?16:4;
    Put32(P,Index>=2?1:0);
    if(Elf64) { Put32(P+4,F); Put64(P+8,O); Put64(P+24,A); Put64(P+32,S); Put64(P+40,Index>=2?8:0); }
    else { Put32(P+4,(UINT32)O); Put32(P+12,(UINT32)A); Put32(P+16,S); Put32(P+20,Index>=2?8:0); Put32(P+24,F); }
  }
  memset(Header+End,0x88,16);
  return End+16;
}
static void Fixture(void)
{
  memset(Out,0,sizeof(Out)); memset(In,0,sizeof(In)); memset(Qmp,0,sizeof(Qmp));
  memset(Votes,0,sizeof(Votes)); memset(PasCalls,0,sizeof(PasCalls)); memset(Memory,0x55,sizeof(Memory));
  Qmp[0]=0x4d41494c; Qmp[1]=1; Qmp[0xc/4]=0xffff; Qmp[0x34/4]=256; Qmp[0x38/4]=0x80;
  FailVote=VoteCalls=FailPas=Reads=0; NoReady=NoAck=BadRemote=NoQmp=NoMsgAck=Busy=Interrupt=Smc32=FALSE;
  NoConvention=AlwaysBusy=AlwaysInterrupted=FALSE;
  CapabilityCalls=SupportCalls=Probe64Calls=Probe32Calls=FailTransportCommand=0;
  CapabilityTransport=SupportTransport=FailTransport=0; CapabilityResult=SupportResult=1;
  Crypto=FALSE; FileStatus=EFI_SUCCESS; Delay=0;
  Smem.GetItem=Item; Smem.AllocateItem=Allocate; Db.GetEntryAddressByName=Address; Db.GetAuxDataByName=Aux;
  Rpmh.Revision=EFI_RPMH_CR_PROTOCOL_REVISION; Rpmh.VoteArc=Write;
  Icb.AcquirePath=Acquire; Icb.SetBandwidth=Bandwidth; Icb.ReleasePath=Release;
  assert(CrPilInitialize(&Pil,&Target,&Smem,&Db,&Rpmh,&Icb)==EFI_SUCCESS);
}
static EFI_STATUS Begin(BOOLEAN Elf64)
{
  UINTN Size=Firmware(Elf64);
  assert(CrPilInspect(Header,Size,&Target,&Image)==EFI_SUCCESS);
  assert(CrPilBuildMetadata(Header,Size,&Image,Segments,&Reads,Metadata,Image.HeaderSize+Image.HashSize)==EFI_SUCCESS);
  return CrPilStart(&Pil,Header,Size,&Image,Metadata,Memory,Segments,&Reads);
}
int main(void)
{
  Metadata=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  assert(Metadata!=MAP_FAILED && (UINTN)Metadata<MAX_UINT32);
  for(UINTN Is64=0;Is64<2;++Is64) {
    Fixture(); assert(Begin((BOOLEAN)Is64)==EFI_SUCCESS);
    assert(Memory[0]==0xaa && Memory[3]==0xaa && Memory[4]==0 && Memory[7]==0 && Memory[8]==0x55);
    assert(Memory[0x2000]==0xbb && Memory[0x2004]==0 && Pil.Running && Pil.PasOwned);
    assert(Out[0]==0x504d5324 && Out[1]==0x101 && Out[2]==0x20000 && Out[3]==0x10010);
    assert(!memcmp((UINT8 *)Out+20,"master-kernel",13));
    assert(CrPilStop(&Pil)==EFI_SUCCESS && PasCalls[6]==1 && !Pil.PasOwned && !Crypto);
    assert(Votes[0]==0 && Votes[1]==0 && Votes[2]==0);
    assert(CrPilStop(&Pil)==EFI_SUCCESS && PasCalls[6]==1);
  }
  Fixture(); CallerEl=4;assert(Begin(FALSE)==EFI_SUCCESS);assert(CrPilStop(&Pil)==EFI_SUCCESS);assert(Pil.NativeShutdownEl==1 && Pil.NativeShutdownGeneration==Pil.StartGeneration);
  Fixture(); CallerEl=12;assert(Begin(FALSE)==EFI_SUCCESS);assert(CrPilStop(&Pil)==EFI_SUCCESS);assert(Pil.NativeShutdownEl==0 && Pil.NativeShutdownGeneration==0);
  CallerEl=8;
  Fixture(); Interrupt=Busy=TRUE; assert(Begin(FALSE)==EFI_SUCCESS && PasCalls[5]==2 && Delay>=30000);
  assert(CrPilStop(&Pil)==EFI_SUCCESS);
  Fixture(); Smc32=TRUE; assert(Begin(FALSE)==EFI_SUCCESS); assert(CrPilStop(&Pil)==EFI_SUCCESS);
  assert(Probe64Calls==1 && Probe32Calls==1 && CapabilityCalls==1 && SupportCalls==1);
  /* qcom_q6v5_pas's signed ADSP startup works without the legacy support
     query. Each response must reach init/auth, and actual auth failure must
     still prevent readiness and keep shutdown/rollback ownership. */
  for(UINTN Is32=0;Is32<2;++Is32) {
    Fixture(); Smc32=(BOOLEAN)Is32; CapabilityResult=0;
    assert(Begin(FALSE)==EFI_SUCCESS && SupportCalls==0 && PasCalls[1]==1 && PasCalls[5]==1);
    assert(CrPilStop(&Pil)==EFI_SUCCESS);
    Fixture(); Smc32=(BOOLEAN)Is32; CapabilityTransport=-4;
    assert(Begin(FALSE)==EFI_SUCCESS && SupportCalls==0 && PasCalls[5]==1);
    assert(CrPilStop(&Pil)==EFI_SUCCESS);
    Fixture(); Smc32=(BOOLEAN)Is32; CapabilityResult=SupportResult=2;
    assert(Begin(FALSE)==EFI_SUCCESS && SupportCalls==1 && PasCalls[5]==1);
    assert(CrPilStop(&Pil)==EFI_SUCCESS);
    Fixture(); Smc32=(BOOLEAN)Is32; SupportResult=0;
    assert(Begin(FALSE)==EFI_SUCCESS && SupportCalls==1 && PasCalls[5]==1);
    assert(CrPilStop(&Pil)==EFI_SUCCESS);
    for(INT32 Code=-1;Code>=-4;Code-=3) {
      Fixture(); Smc32=(BOOLEAN)Is32; SupportTransport=Code;
      assert(Begin(FALSE)==EFI_SUCCESS && SupportCalls==1 && PasCalls[5]==1);
      assert(CrPilStop(&Pil)==EFI_SUCCESS);
    }
    Fixture(); Smc32=(BOOLEAN)Is32; SupportResult=0; FailPas=5;
    assert(Begin(FALSE)==EFI_SECURITY_VIOLATION && !Pil.Running && Pil.PasOwned);
    assert(CrPilStop(&Pil)==EFI_SUCCESS && PasCalls[6]==1);
  }
  Fixture(); NoConvention=TRUE;
  assert(Begin(FALSE)==EFI_UNSUPPORTED && Probe64Calls==1 && Probe32Calls==1 && !Crypto && !Pil.PasOwned);
  assert(CrPilStop(&Pil)==EFI_SUCCESS && PasCalls[6]==0);
  CONST EFI_STATUS TransportStatus[]={EFI_DEVICE_ERROR,EFI_INVALID_PARAMETER,
    EFI_INVALID_PARAMETER,EFI_UNSUPPORTED,EFI_OUT_OF_RESOURCES};
  for(UINTN Index=0;Index<ARRAY_SIZE(TransportStatus);++Index) {
    Fixture(); FailTransportCommand=1; FailTransport=-(INT32)(Index+1);
    assert(Begin(FALSE)==TransportStatus[Index] && !Pil.Running && Pil.PasOwned && PasCalls[5]==0);
    FailTransportCommand=0;
    assert(CrPilStop(&Pil)==EFI_SUCCESS && PasCalls[6]==1 && !Crypto);
  }
  Fixture(); AlwaysBusy=TRUE;
  assert(Begin(FALSE)==EFI_TIMEOUT && !Pil.Running && Pil.PasOwned && PasCalls[5]==21 && Delay==630000);
  AlwaysBusy=FALSE; assert(CrPilStop(&Pil)==EFI_SUCCESS);
  Fixture(); Interrupt=AlwaysInterrupted=TRUE;
  assert(Begin(FALSE)==EFI_TIMEOUT && !Pil.Running && Pil.PasOwned && PasCalls[5]==0);
  assert(Pil.SmcInterrupted && Pil.SmcCookie==0x1234 && Pil.Votes==7 && Crypto);
  assert(CrPilStop(&Pil)==EFI_TIMEOUT && Pil.SmcInterrupted && Pil.PasOwned &&
         Pil.Votes==7 && Crypto && PasCalls[6]==0);
  AlwaysInterrupted=FALSE;
  assert(CrPilStop(&Pil)==EFI_SUCCESS && !Pil.SmcInterrupted && !Pil.PasOwned && !Crypto && PasCalls[6]==1);
  Fixture(); assert(Begin(FALSE)==EFI_SUCCESS); In[4]=1;
  assert(CrPilStop(&Pil)==EFI_SUCCESS && (Out[4]&2)!=0); // SSR restart-done ACK
  Fixture(); assert(Begin(FALSE)==EFI_SUCCESS); In[1]=2; // v2 peer without SSR support
  assert(CrPilStop(&Pil)==EFI_SUCCESS && Out[1]==2);
  Fixture(); Remote(2); assert(Begin(FALSE)==EFI_ALREADY_STARTED && PasCalls[1]==0 && Votes[0]==0 && !Crypto);
  assert(CrPilStop(&Pil)==EFI_SUCCESS && PasCalls[6]==0);
  Fixture(); Remote(3); assert(Begin(FALSE)==EFI_DEVICE_ERROR && !Pil.PasOwned && !Crypto);
  assert(CrPilStop(&Pil)==EFI_SUCCESS && PasCalls[6]==0); // fatal peer cannot be borrowed as ready
  for(UINT32 Fail=1;Fail<=3;++Fail) {
    Fixture(); FailVote=Fail; assert(Begin(FALSE)==EFI_TIMEOUT && PasCalls[1]==0);
    FailVote=0; assert(CrPilStop(&Pil)==EFI_SUCCESS && Votes[0]==0 && Votes[1]==0 && Votes[2]==0 && !Crypto);
  }
  for(UINT32 Command=1;Command<=5;Command+=(Command==1?1:3)) {
    Fixture(); FailPas=Command; assert(EFI_ERROR(Begin(FALSE)) && Pil.PasOwned);
    assert(CrPilStop(&Pil)==EFI_SUCCESS && !Pil.PasOwned && PasCalls[6]==1);
  }
  Fixture(); assert(Begin(FALSE)==EFI_SUCCESS); FailPas=6;
  assert(CrPilStop(&Pil)==EFI_DEVICE_ERROR && Pil.PasOwned && Pil.Running && Crypto && Pil.Votes==7);
  FailPas=0; assert(CrPilStop(&Pil)==EFI_SUCCESS && !Pil.PasOwned && !Crypto);
  assert(Pil.NativeShutdownEl==2 && Pil.NativeShutdownGeneration==Pil.StartGeneration && Pil.Shutdown.Complete);
  Fixture(); NoReady=TRUE; assert(Begin(FALSE)==EFI_TIMEOUT && Delay>=5000000 && Pil.PasOwned);
  assert(CrPilStop(&Pil)==EFI_SUCCESS);
  Fixture(); BadRemote=TRUE; assert(Begin(FALSE)==EFI_DEVICE_ERROR); assert(CrPilStop(&Pil)==EFI_SUCCESS);
  Fixture(); NoQmp=TRUE; assert(Begin(FALSE)==EFI_TIMEOUT && !Pil.PasOwned);
  assert(CrPilStop(&Pil)==EFI_TIMEOUT && Pil.QmpOpen && Pil.QmpClosing && Crypto);
  NoQmp=FALSE; assert(CrPilStop(&Pil)==EFI_SUCCESS && !Pil.QmpOpen && !Crypto);
  Fixture(); NoMsgAck=TRUE; assert(Begin(FALSE)==EFI_TIMEOUT && Pil.LoadState && !Pil.PasOwned);
  NoMsgAck=FALSE; Qmp[0x80/4]=0; assert(CrPilStop(&Pil)==EFI_SUCCESS && !Pil.LoadState);
  Fixture(); FileStatus=EFI_NOT_FOUND; assert(Begin(FALSE)==EFI_NOT_FOUND && Pil.PasOwned);
  assert(CrPilStop(&Pil)==EFI_SUCCESS);
  Fixture(); UINTN Size=Firmware(FALSE);
  for(UINTN N=0;N<180;++N) assert(EFI_ERROR(CrPilInspect(Header,N,&Target,&Image)));
  Put32(Header+52+2*32+20,2); assert(CrPilInspect(Header,Size,&Target,&Image)==EFI_COMPROMISED_DATA);
  Size=Firmware(FALSE); Put32(Header+52+3*32+12,0x100004);
  assert(CrPilInspect(Header,Size,&Target,&Image)==EFI_COMPROMISED_DATA);
  Size=Firmware(TRUE); Put64(Header+64+2*56+40,MAX_UINT64);
  assert(CrPilInspect(Header,Size,&Target,&Image)==EFI_COMPROMISED_DATA);
  Size=Firmware(TRUE); Put64(Header+32,MAX_UINT64-8);
  assert(CrPilInspect(Header,Size,&Target,&Image)==EFI_COMPROMISED_DATA);
  Size=Firmware(FALSE); assert(CrPilInspect(Header,Size,&Target,&Image)==EFI_SUCCESS);
  assert(CrPilBuildMetadata(Header,Image.HeaderSize,&Image,Segments,&Reads,Metadata,Image.HeaderSize+16)==EFI_SUCCESS && Reads==1);
  assert(((UINT8 *)Metadata)[Image.HeaderSize]==0x88);
  /* A monolithic, nonrelocatable image copies embedded payload and must not
     issue PAS mem_setup or read any split file. */
  Fixture(); Size=Firmware(FALSE);
  Put32(Header+52+32+4,180);
  for(UINT32 Index=2;Index<4;++Index) {
    UINT8 *Ph=Header+52+Index*32;
    Put32(Ph+4,(UINT32)Size+(Index-2)*4);
    Put32(Ph+12,Target.MemoryBase+(Index-2)*0x2000); Put32(Ph+24,5);
    memset(Header+Size+(Index-2)*4,Index==2?0xaa:0xbb,4);
  }
  Size+=8;
  assert(CrPilInspect(Header,Size,&Target,&Image)==EFI_SUCCESS && !Image.Split && !Image.Relocatable);
  assert(CrPilBuildMetadata(Header,Size,&Image,NULL,NULL,Metadata,196)==EFI_SUCCESS);
  assert(CrPilStart(&Pil,Header,Size,&Image,Metadata,Memory,NULL,NULL)==EFI_SUCCESS);
  assert(PasCalls[2]==0 && Reads==0 && Memory[0]==0xaa && Memory[4]==0 && Memory[0x2000]==0xbb);
  assert(CrPilStop(&Pil)==EFI_SUCCESS);
  Fixture(); Size=Firmware(FALSE); Put32(Header+52+32+16,0x2000);
  assert(CrPilInspect(Header,Size,&Target,&Image)==EFI_SUCCESS);
  assert(CrPilStart(&Pil,Header,Size,&Image,(VOID *)(UINTN)0xfffff000,Memory,Segments,&Reads)==EFI_INVALID_PARAMETER);
  assert(PasCalls[1]==0 && !Crypto); // metadata extent must also fit below 4 GiB
  munmap(Metadata,4096);
  puts("PIL: ELF32/64 bounds, packed/split metadata, PAS wire arguments, SMP2P/QMP and owned rollback passed");
  return 0;
}
