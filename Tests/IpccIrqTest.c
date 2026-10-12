/** Real OSKAL/IPCC interrupt and failure ownership tests. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../Library/OskInterruptLib/oskinterrupt.c"
#include <ucsi/Ipcc.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SynchronizationLib.h>

EFI_GUID gHardwareInterrupt2ProtocolGuid;
EFI_BOOT_SERVICES *gBS;
static HARDWARE_INTERRUPT_HANDLER Handler;
static EFI_HARDWARE_INTERRUPT2_PROTOCOL Hardware;
static EFI_BOOT_SERVICES Services;
static CR_IPCC Ipcc;
static CR_INTERRUPT_CONFIG Config = { .InterruptNumber=261,
  .TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH };
static UINT32 Control, Ids[256];
static UINTN Head, Tail, Reads, Clears, Wakes, Eois, Enables, Disables;
static BOOLEAN IrqEnabled, SignalEnabled, FailRegister, FailUnregister;
static BOOLEAN FailEnable, FailDisable, FailTrigger, FailWake, DropConfig;
static BOOLEAN RegisterIrq, ReenableIrq;
static UINTN InterruptDepth;
static EFI_TPL CurrentTpl;
static struct { UINTN Before; BOOLEAN IsDispatch; } TplScopes[8];
static UINTN TplDepth;

static EFI_TPL EFIAPI Raise(EFI_TPL Tpl) {
  EFI_TPL Old=CurrentTpl;
  assert(Tpl==TPL_HIGH_LEVEL && Tpl>=Old);
  assert(TplDepth<8);
  TplScopes[TplDepth].Before=Eois;
  TplScopes[TplDepth++].IsDispatch=InterruptDepth!=0;
  CurrentTpl=Tpl;
  return Old;
}
static VOID EFIAPI Restore(EFI_TPL Tpl) {
  assert(CurrentTpl==TPL_HIGH_LEVEL && TplDepth!=0);
  --TplDepth;
  /* Setup/removal guards mask dispatch without generating an IRQ. A mock
     Register callback can still force a nested interrupt, whose own scope
     must finish precisely one EOI before restoring TPL. */
  if(TplScopes[TplDepth].IsDispatch) assert(Eois==TplScopes[TplDepth].Before+1);
  else assert(Eois>=TplScopes[TplDepth].Before);
  CurrentTpl=Tpl;
}

VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (void)Level; (void)Format; }
VOID *EFIAPI SetMem(VOID *Dst, UINTN Size, UINT8 Value) { return memset(Dst,Value,Size); }
VOID *EFIAPI CopyMem(VOID *Dst, CONST VOID *Src, UINTN Size) { return memcpy(Dst,Src,Size); }
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *Value, UINT32 Compare, UINT32 Exchange)
{ __atomic_compare_exchange_n(Value,&Compare,Exchange,FALSE,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST); return Compare; }

static void Dispatch(void)
{
  EFI_SYSTEM_CONTEXT Context={0};
  assert(Handler!=NULL && IrqEnabled);
  ++InterruptDepth; Handler(261,Context); --InterruptDepth;
}
static void Push(UINT32 Id) { assert(Tail<256); Ids[Tail++]=Id; }
UINT32 EFIAPI MmioRead32(UINTN Address)
{
  assert(Address==CR_IPCC_BASE+8 || Address==CR_IPCC_BASE+0x10);
  if(Address==CR_IPCC_BASE+8) return Control;
  ++Reads;
  return Head==Tail?0xffffffffU:Ids[Head];
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  assert(Address>=CR_IPCC_BASE && Address<=CR_IPCC_BASE+0x1c);
  switch(Address-CR_IPCC_BASE) {
  case 8: if(!DropConfig) Control=Value; break;
  case 0xc: assert(Value==CR_IPCC_LPASS_GLINK); break;
  case 0x14:
    assert(Value==CR_IPCC_LPASS_GLINK); SignalEnabled=TRUE; ++Enables;
    if(ReenableIrq) { ReenableIrq=FALSE; Push(CR_IPCC_LPASS_GLINK); Dispatch(); }
    break;
  case 0x18: assert(Value==CR_IPCC_LPASS_GLINK); SignalEnabled=FALSE; ++Disables; break;
  case 0x1c: assert(Head<Tail && Value==Ids[Head]); ++Head; ++Clears; break;
  default: assert(0);
  }
  return Value;
}
static EFI_STATUS EFIAPI Enable(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This, HARDWARE_INTERRUPT_SOURCE Source)
{ assert(This==&Hardware && Source==261); if(FailEnable) return EFI_DEVICE_ERROR; IrqEnabled=TRUE; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Disable(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This, HARDWARE_INTERRUPT_SOURCE Source)
{ assert(This==&Hardware && Source==261); if(FailDisable) return EFI_DEVICE_ERROR; IrqEnabled=FALSE; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Register(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,
                                  HARDWARE_INTERRUPT_SOURCE Source, HARDWARE_INTERRUPT_HANDLER NewHandler)
{
  assert(This==&Hardware && Source==261);
  if(NewHandler==NULL) {
    if(FailUnregister) return EFI_DEVICE_ERROR;
    Handler=NULL; return EFI_SUCCESS;
  }
  if(Handler!=NULL) return EFI_ALREADY_STARTED;
  Handler=NewHandler;
  if(RegisterIrq) { IrqEnabled=TRUE; Dispatch(); }
  if(FailRegister) return EFI_DEVICE_ERROR;
  return Enable(This,Source);
}
static EFI_STATUS EFIAPI Trigger(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,
                                 HARDWARE_INTERRUPT_SOURCE Source, EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE Type)
{ assert(This==&Hardware && Source==261 && Type==EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_HIGH); return FailTrigger?EFI_DEVICE_ERROR:EFI_SUCCESS; }
static EFI_STATUS EFIAPI Eoi(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This, HARDWARE_INTERRUPT_SOURCE Source)
{ assert(This==&Hardware && Source==261 && InterruptDepth==1); ++Eois; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Registration, VOID **Interface)
{ assert(Guid==&gHardwareInterrupt2ProtocolGuid && Registration==NULL); *Interface=&Hardware; return EFI_SUCCESS; }
static CR_STATUS Wake(VOID *Context)
{ assert(Context==&Ipcc && InterruptDepth==1 && CurrentTpl==TPL_HIGH_LEVEL); ++Wakes; return FailWake?CR_DEVICE_ERROR:CR_SUCCESS; }
static VOID EFIAPI Foreign(HARDWARE_INTERRUPT_SOURCE Source, EFI_SYSTEM_CONTEXT Context)
{ (void)Source; (void)Context; }
static void Fixture(void)
{
  assert(!Ipcc.Registered);
  memset(&Ipcc,0,sizeof(Ipcc)); memset(mCrIntTable,0,sizeof(mCrIntTable)); mHwInterrupt2=NULL;
  Handler=NULL; Control=0x21; Head=Tail=Reads=Clears=Wakes=Eois=Enables=Disables=0;
  IrqEnabled=SignalEnabled=FailRegister=FailUnregister=FailEnable=FailDisable=FALSE;
  FailTrigger=FailWake=DropConfig=RegisterIrq=ReenableIrq=FALSE;
  Hardware.RegisterInterruptSource=Register; Hardware.EnableInterruptSource=Enable;
  Hardware.DisableInterruptSource=Disable; Hardware.SetTriggerType=Trigger; Hardware.EndOfInterrupt=Eoi;
  CurrentTpl=TPL_APPLICATION; TplDepth=0;
  Services.LocateProtocol=Locate; Services.RaiseTPL=Raise; Services.RestoreTPL=Restore; gBS=&Services;
}
static CR_STATUS Start(void) { return CrIpccStart(&Ipcc,CR_IPCC_BASE,&Config,Wake,&Ipcc); }
static UINT32 SplitRead(VOID *Context,UINT32 Offset) {
  assert(Context==&Ipcc&&(Offset==8||Offset==0x10));
  return MmioRead32(CR_IPCC_BASE+Offset);
}
static VOID SplitWrite(VOID *Context,UINT32 Offset,UINT32 Value) {
  assert(Context==&Ipcc&&(Offset==8||Offset==0x14||Offset==0x18||Offset==0x1c));
  (VOID)MmioWrite32(CR_IPCC_BASE+Offset,Value);
}
int main(void)
{
  BOOLEAN Received;
  Fixture();CR_IPCC_IO Io={&Ipcc,SplitRead,SplitWrite};
  assert(CrIpccStartWithIo(&Ipcc,&Io,&Config,Wake,&Ipcc)==CR_SUCCESS&&Ipcc.Base==0);
  Push(CR_IPCC_LPASS_GLINK);Dispatch();
  assert(CrIpccService(&Ipcc,&Received)==CR_SUCCESS&&Received);
  FailUnregister=TRUE;
  assert(CrIpccStop(&Ipcc)==CR_DEVICE_ERROR&&Ipcc.Registered&&Ipcc.Io.Context==&Ipcc);
  FailUnregister=FALSE;assert(CrIpccStop(&Ipcc)==CR_SUCCESS&&Control==0x21);
  Fixture();
  assert(CrIpccStart(NULL,CR_IPCC_BASE,&Config,Wake,&Ipcc)==CR_INVALID_PARAMETER);
  assert(Start()==CR_SUCCESS && Ipcc.Registered && SignalEnabled && Control==0x20);
  assert(Start()==CR_ALREADY_STARTED);
  for(UINTN I=0;I<1000;++I) assert(CrIpccService(&Ipcc,&Received)==CR_SUCCESS && !Received);
  assert(Reads==0 && Wakes==0); /* no steady RX register polling */
  Push((5U<<16)|2U); Push(CR_IPCC_LPASS_GLINK); Dispatch();
  assert(Wakes==1 && Eois==1 && Clears==2 && !SignalEnabled);
  assert(Ipcc.Received==1 && Ipcc.OtherSignals==1 && Ipcc.Irqs==1);
  UINTN Before=Reads;
  assert(CrIpccService(&Ipcc,&Received)==CR_SUCCESS && Received && SignalEnabled);
  assert(Reads==Before); /* hardware ACK in ISR; parsing is deferred */
  Push(CR_IPCC_LPASS_GLINK); Dispatch(); ReenableIrq=TRUE;
  assert(CrIpccService(&Ipcc,&Received)==CR_NOT_READY && Received);
  assert(Ipcc.Pending!=0 && Wakes==3 && !SignalEnabled);
  assert(CrIpccService(&Ipcc,&Received)==CR_SUCCESS && Received && SignalEnabled);
  Control|=0x100;
  assert(CrIpccStop(&Ipcc)==CR_SUCCESS && Handler==NULL && !IrqEnabled && !SignalEnabled && Control==0x121);
  assert(CrIpccService(&Ipcc,&Received)==CR_NOT_READY);

  Fixture(); RegisterIrq=TRUE; Push(CR_IPCC_LPASS_GLINK);
  assert(Start()==CR_SUCCESS && Wakes==0 && Eois==1 && Ipcc.Pending!=0);
  assert(CrIpccService(&Ipcc,&Received)==CR_SUCCESS && Received);
  assert(CrIpccStop(&Ipcc)==CR_SUCCESS);

  Fixture(); Handler=Foreign;
  assert(Start()==EFI_ALREADY_STARTED && !Ipcc.Registered && Handler==Foreign);
  assert(Control==0x21 && Enables==0 && Disables==0 && Clears==0);

  Fixture(); FailRegister=FailUnregister=TRUE;
  assert(Start()==CR_DEVICE_ERROR && Ipcc.Registered && Handler!=NULL && Ipcc.Stopping);
  FailUnregister=FALSE; assert(CrIpccStop(&Ipcc)==CR_SUCCESS && Handler==NULL);
  Fixture(); FailEnable=TRUE;
  assert(Start()==CR_DEVICE_ERROR && !Ipcc.Registered && Handler==NULL && Control==0x21);
  Fixture(); FailTrigger=TRUE;
  assert(Start()==CR_DEVICE_ERROR && !Ipcc.Registered && Handler==NULL);
  Fixture(); DropConfig=TRUE;
  assert(Start()==CR_DEVICE_ERROR && !Ipcc.Registered && Handler==NULL && Control==0x21);

  Fixture(); assert(Start()==CR_SUCCESS); FailDisable=TRUE;
  assert(CrIpccStop(&Ipcc)==CR_DEVICE_ERROR && Ipcc.Registered && Handler!=NULL && !SignalEnabled);
  Push(CR_IPCC_LPASS_GLINK); Dispatch(); assert(Wakes==0 && Ipcc.Stopping);
  FailDisable=FALSE; FailUnregister=TRUE;
  assert(CrIpccStop(&Ipcc)==CR_DEVICE_ERROR && Ipcc.Registered && Control==0x20);
  FailUnregister=FALSE; assert(CrIpccStop(&Ipcc)==CR_SUCCESS && Control==0x21);

  Fixture(); assert(Start()==CR_SUCCESS); FailWake=TRUE;
  Push(CR_IPCC_LPASS_GLINK); Dispatch();
  assert(CrIpccService(&Ipcc,&Received)==CR_DEVICE_ERROR && !SignalEnabled && Ipcc.Pending!=0);
  assert(CrIpccStop(&Ipcc)==CR_SUCCESS);

  Fixture(); assert(Start()==CR_SUCCESS);
  for(UINTN I=0;I<150;++I) Push((5U<<16)|(I+1));
  Dispatch(); assert(Reads==64 && Head==64 && Eois==1 && Wakes==1);
  assert(CrIpccService(&Ipcc,&Received)==CR_NOT_READY && !Received && Reads==128);
  assert(CrIpccService(&Ipcc,&Received)==CR_SUCCESS && !Received && Head==150 && Reads==151);
  assert(Ipcc.OtherSignals==150 && SignalEnabled);
  assert(CrIpccStop(&Ipcc)==CR_SUCCESS);
  puts("IPCC/OSKAL: IRQ ACK, deferred receive, idle, burst budget, re-enable race and retained rollback/removal passed");
  return 0;
}
