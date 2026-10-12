/* Read-only HDK interface diagnostics against the actual core/host/targets.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#define mPcieContext mCorePcieContext
#include "../Library/PcieLib/pcie.c"
#undef mPcieContext
#include "../Library/PciHostBridgeLib/PciHostBridgeLib.c"

STATIC CHAR8 Output[65536];
STATIC UINTN OutputBytes, GpioReads, MmioReads;
STATIC UINTN AuxFrequencyReads;
STATIC UINT32 AuxFrequencyValue;
STATIC INTN FailedPin = -1;
STATIC CONST PcieTargetContext *Target;
STATIC PcieDeviceContext Context;
STATIC GpioConfigParams Pins[211];
/* Actual generated target branch/RCG/GDSC facts. The shared PLL descriptor
   models the same GPLL0 register layout; no clock enable routine is called. */
STATIC ClockController mGcc, mDisp;
STATIC CR_STATUS CheckDisplayPll(ClockNode *Node,UINT64 Rate) {(VOID)Node;(VOID)Rate;assert(0);return CR_UNSUPPORTED;}
STATIC ClockNode gpll0 = {.Name="gcc_gpll0",.Type=CLOCK_NODE_TYPE_FIXED_PLL,
  .ParentController=&mGcc,.FixedPll.Description={.VoteRegister=0x52018,.VoteMask=1,
  .ModeRegister=0,.UserRegister=0xc,.LRegister=4,.AlphaRegister=0x40}};
#if HDK_PORTS == 3
#include "../Target/Sm8250/CrClockGenerated.h"
#else
#include "../Target/Sm8350/CrStorageClockGenerated.h"
#endif
STATIC ClockDriverContext ClockContext;
STATIC BOOLEAN HaveClockContext;
STATIC UINTN ClockReads;
ClockDriverContext *CrDalGetClockContext(VOID) {return HaveClockContext?&ClockContext:NULL;}
CR_STATUS CrDalClockResolveResource(CONST CHAR8 *C,CONST CHAR8 *Id,
    CR_DAL_CLOCK_RESOURCE_KIND Kind,CONST CHAR8 **Native,UINT32 *Flags) {
  if(strcmp(C,"gcc") || !Id || Kind>=CrDalClockResourceMax)return CR_NOT_FOUND;
  *Native=Id;*Flags=0;return CR_SUCCESS;
}

/* Adapt EDK2's ASCII/status/64-bit conversions to the host printf sink.
 * Assertions consume emitted diagnostics, not source text or mock calls. */
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {
  CHAR8 HostFormat[2048]; UINTN I=0, J=0; va_list Args;
  (VOID)Level;
  while (Format[I] && J+4<sizeof(HostFormat)) {
    if (Format[I]=='%' && Format[I+1]=='a') {
      HostFormat[J++]='%'; HostFormat[J++]='s'; I+=2;
    } else if (Format[I]=='%' && Format[I+1]=='r') {
      HostFormat[J++]='%'; HostFormat[J++]='z'; HostFormat[J++]='x'; I+=2;
    } else if (Format[I]=='%' && Format[I+1]=='L') {
      HostFormat[J++]='%'; HostFormat[J++]='l'; HostFormat[J++]='l'; I+=2;
    } else HostFormat[J++]=Format[I++];
  }
  HostFormat[J]=0;
  va_start(Args,Format);
  int N=vsnprintf(Output+OutputBytes,sizeof(Output)-OutputBytes,HostFormat,Args);
  va_end(Args);
  assert(N>=0 && (UINTN)N<sizeof(Output)-OutputBytes); OutputBytes+=(UINTN)N;
}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) {(VOID)Level;return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID) {return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D) {(VOID)F;(VOID)L;(VOID)D;assert(0);}
VOID *EFIAPI ZeroMem(VOID *B,UINTN N) {return memset(B,0,N);}
VOID *EFIAPI SetMem(VOID *B,UINTN N,UINT8 V) {return memset(B,V,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) {return strcmp(A,B);}
UINT64 EFIAPI LShiftU64(UINT64 V,UINTN N) {assert(N<64);return V<<N;}

STATIC UINT32 Read32(VOID *Cookie,UINT64 Address) {
  BOOLEAN Allowed=FALSE; (VOID)Cookie; ++MmioReads;
  if(Address>=mGcc.Address && Address+4<=mGcc.Address+mGcc.Size) {
    assert(HaveClockContext && mGcc.MemMapped); ++ClockReads;
    if(Address==mGcc.Address+gcc_pcie_1_pipe_clk.EnableRegister)return 0x40000000;
    if(Address==mGcc.Address+gcc_pcie_1_pipe_clk.HaltRegister)return 0x80000000;
#if HDK_PORTS == 3
    if(Address==mGcc.Address+gcc_pcie_wigig_clkref_en.EnableRegister)return 1;
#else
    if(Address==mGcc.Address+gcc_pcie_1_clkref_en.EnableRegister)return 1;
    if(Address==mGcc.Address+gcc_pcie_1_pipe_clk_src.MuxRegister)return 2;
#endif
    if(Address==mGcc.Address+gcc_pcie_1_aux_clk_src.CmdRegister)return 2;
    if(Address==mGcc.Address+gcc_pcie_1_aux_clk_src.CmdRegister+4)return 1;
    if(Address==mGcc.Address+pcie_1_gdsc.GdscRegister)return 0x80000000;
    return 0;
  }
  for(UINTN I=0;I<Target->RegionCount;++I) {
    CONST PcieTargetRegion *R=&Target->Regions[I];
    if(Address>=R->Base && Address+4<=R->Base+R->Size) Allowed=TRUE;
  }
  for(UINTN I=0;I<Target->PhyCount;++I) {
    CONST PcieTargetPhy *P=&Target->Phys[I];
    if(Address>=P->Base && Address+4<=P->Base+P->Size) Allowed=TRUE;
  }
  assert(Allowed && (Address&3U)==0);
  for(UINTN I=0;I<Target->ControllerCount;++I) {
    UINT64 D=Context.Ports[I].DbiBase;
    if(Address==D+0xB40) {++AuxFrequencyReads;return AuxFrequencyValue;}
    if(Address==D+0x34) return 0x90; /* Capability is deliberately not 0x70. */
    if(Address==D+0x90) return 0x10;
    if(Address==D+0xA0) return 0x20220008; /* active, Gen2 x2 */
  }
  return 0;
}
STATIC UINT64 Now(VOID *Cookie) {(VOID)Cookie;return 900000;}
STATIC EFI_STATUS EFIAPI GetPin(EFI_GPIO_CR_PROTOCOL *This,GpioConfigParams *Config) {
  (VOID)This; assert(Config->PinNumber<ARRAY_SIZE(Pins)); ++GpioReads;
  if((INTN)Config->PinNumber==FailedPin) return EFI_DEVICE_ERROR;
  *Config=Pins[Config->PinNumber]; return EFI_SUCCESS;
}
STATIC EFI_GPIO_CR_PROTOCOL Gpio={.Revision=EFI_GPIO_CR_PROTOCOL_REVISION,.GetGpioConfig=GetPin};
STATIC VOID Write32(VOID *Cookie,UINT64 Address,UINT32 Value) {
  (VOID)Cookie;(VOID)Address;(VOID)Value;assert(0);
}
STATIC PcieIoOps Io={.Read32=Read32,.Write32=Write32,.GetTimeUs=Now};

STATIC VOID Fixture(VOID) {
  memset(&Context,0,sizeof(Context)); memset(&mPcieResources,0,sizeof(mPcieResources));
  memset(Pins,0,sizeof(Pins));
  Target=CrTargetGetPcieContext(); mPcieTarget=Target; mPcieContext=&Context;
  Context.Target=Target; Context.Initialized=TRUE; mPcieResources.Gpio=&Gpio;
  for(UINTN I=0;I<Target->ControllerCount;++I) {
    CONST PcieTargetController *C=&Target->Controllers[I];
    Context.Ports[I].Target=C; Context.Ports[I].Phy=&Target->Phys[C->PhyIndex];
    Context.Ports[I].ParfBase=FindRegion(Target,C,"parf")->Base;
    Context.Ports[I].DbiBase=FindRegion(Target,C,"dbi")->Base;
    Context.Ports[I].DbiSize=FindRegion(Target,C,"dbi")->Size;
    Context.Ports[I].State=PCIE_PORT_READY_NO_LINK;
    Context.Ports[I].TimingValidMask=7;
    Context.Ports[I].PerstAssertTimeUs=100000;
    Context.Ports[I].PerstReleaseTimeUs=300000;
    Context.Ports[I].TrainingStartTimeUs=301500;
    mPcieResources.Endpoints[C->Domain].Ready=TRUE;
    mPcieResources.Endpoints[C->Domain].StepMask=(1ULL<<C->PowerSequence->Count)-1;
  }
  for(UINTN I=0;I<ARRAY_SIZE(Pins);++I) Pins[I]=(GpioConfigParams){
    .PinNumber=(UINT16)I,.FunctionSel=GPIO_FUNC_1,.OutputEnable=FALSE,
    .Pull=GPIO_PULL_UP,.DriveStrength=GPIO_DRIVE_STRENGTH_4MA,.OutputValue=GPIO_VALUE_HIGH};
  mPcieResources.SupplyRefs[0]=(CRANE_PCIE_RESOURCE_REF){.Id="smpa5",.Count=2,.Ready=TRUE,.Owned=TRUE,.VoltageMv=1904,.Mode=7};
  mPcieResources.SupplyRefs[1]=(CRANE_PCIE_RESOURCE_REF){.Id="ldoa15",.Count=1,.Ready=TRUE,.Owned=TRUE,.VoltageMv=1800,.Mode=7};
  (VOID)mDisp;(VOID)mDisplayClocks;(VOID)mDisplayGdscs;
  mGcc=(ClockController){.Address=0x100000,.Size=0x1f0000,.MemMapped=TRUE,
    .Clks=mClocks,.ClkCount=ARRAY_SIZE(mClocks),.Gdscs=mGdscs,.GdscCount=ARRAY_SIZE(mGdscs)};
  gpll0.FixedPll.Initialized=TRUE;
  gpll0.FixedPll.Description.Base=mGcc.Address;
  gpll0.FixedPll.Description.Size=mGcc.Size;
  ClockContext=(ClockDriverContext){.ClockControllers=&mGcc,.ClockControllerCount=1,.LockInitialized=TRUE};
  HaveClockContext=TRUE;
  Output[0]=0;OutputBytes=GpioReads=MmioReads=ClockReads=0;FailedPin=-1;
  AuxFrequencyReads=0;AuxFrequencyValue=0x14;
}
STATIC VOID Snapshot(UINTN Port) {
  CRANE_PCIE_RESOURCE_CONTEXT Before=mPcieResources;
  ClockController ClockBefore=mGcc;
  ClockDriverContext ContextBefore=ClockContext;
  PcieDeviceContext CoreBefore=Context;
  GpioConfigParams PinsBefore[ARRAY_SIZE(Pins)]; memcpy(PinsBefore,Pins,sizeof(Pins));
  CranePcieDiagnoseLink(&Io,&Target->Controllers[Port]);
  assert(memcmp(&Before,&mPcieResources,sizeof(Before))==0);
  assert(memcmp(&ClockBefore,&mGcc,sizeof(ClockBefore))==0);
  assert(memcmp(&ContextBefore,&ClockContext,sizeof(ContextBefore))==0);
  assert(memcmp(&CoreBefore,&Context,sizeof(Context))==0);
  assert(memcmp(PinsBefore,Pins,sizeof(Pins))==0);
}

int main(VOID) {
  Fixture(); Snapshot(1);
  assert(strstr(Output,"clock-hw gcc_pcie_1_pipe_clk") && strstr(Output,"gate=0x40000000") &&
         strstr(Output,"halt=0x80000000") && strstr(Output,"halt-policy=3"));
  assert(strstr(Output,"clock-hw gcc_pcie_1_aux_clk_src") && strstr(Output,"cmd=0x2 cfg=0x1"));
  assert(strstr(Output,"clock-hw gcc_gpll0"));
#if HDK_PORTS == 3
  assert(strstr(Output,"clock-hw gcc_pcie_wigig_clkref_en") &&
         strstr(Output,"clock-hw gcc_pcie_phy_refgen_clk_src"));
  /* SM8250 provider describes a pipe branch without a mux register. */
  assert(!strstr(Output,"gcc_pcie_1_pipe_clk_src"));
#else
  assert(strstr(Output,"clock-hw gcc_pcie_1_clkref_en") &&
         strstr(Output,"clock-hw gcc_pcie_1_phy_rchng_clk_src") &&
         strstr(Output,"clock-hw gcc_pcie_1_pipe_clk_src") && strstr(Output,"mux=0x2 mask=0x3"));
#endif
  assert(strstr(Output,"gdsc-hw pcie_1_gdsc") && strstr(Output,"control=0x80000000"));
  assert(ClockReads>10);
  if(HDK_PORTS==3) {
    assert(AuxFrequencyReads==1);
    assert(strstr(Output,"AUX_CLK_FREQ +0xb40 raw=0x14 target-aux-hz=19200000"));
  } else assert(AuxFrequencyReads==0);
  if(HDK_PORTS==3) {
    /* Regression: the old function reports only controller L5A/L9A. */
    assert(strstr(Output,"id=smpa5") && strstr(Output,"id=ldoa15") && strstr(Output,"id=rfclka1"));
    assert(strstr(Output,"step-mask=0xf") && strstr(Output,"endpoint-ready=1"));
    assert(strstr(Output,"GPIO 14") && strstr(Output,"GPIO 82") && strstr(Output,"GPIO 83") && strstr(Output,"GPIO 84"));
    assert(GpioReads==4);
  } else {
    assert(strstr(Output,"id=smpc1") && strstr(Output,"id=rfclka5"));
    assert(strstr(Output,"GPIO 47") && strstr(Output,"GPIO 6") && strstr(Output,"GPIO 67"));
    assert(GpioReads==6);
  }
  assert(strstr(Output,"point=timeout") && strstr(Output,"negotiated-speed=2") && strstr(Output,"negotiated-lanes=2"));
  assert(strstr(Output,"PHY base=") && strstr(Output,"PARF base=") && strstr(Output,"gcc_pcie_1_pipe_clk"));
  Fixture(); Context.Ports[1].State=PCIE_PORT_INITIALIZING;
  Context.Ports[1].TimingValidMask=1; Snapshot(1);
  assert(strstr(Output,"point=pre-perst-release") && !strstr(Output,"point=timeout"));
  assert(AuxFrequencyReads==(HDK_PORTS==3?1:0));
  Fixture();AuxFrequencyValue=0xffffffffU;Snapshot(1);
  if(HDK_PORTS==3) assert(strstr(Output,"AUX_CLK_FREQ +0xb40 raw=0xffffffff"));
  /* Public offset admission is independent of capability traversal.  Invalid
   * optional apertures and absent read operations must perform no MMIO. */
  if(HDK_PORTS==3) {
    CONST PcieTargetController *C=&Target->Controllers[1];
    CONST PcieTargetRegion *Dbi=CranePcieDiagnosticRegion(C,"dbi");
    PcieTargetRegion Bad=*Dbi;
    Fixture();Bad.Size=0xB43;
    CranePcieDiagnoseAuxClockFrequency(&Io,C,&Bad);
    assert(MmioReads==0 && strstr(Output,"AUX_CLK_FREQ unavailable"));
    Fixture();Bad=*Dbi;Bad.Base++;
    CranePcieDiagnoseAuxClockFrequency(&Io,C,&Bad);assert(MmioReads==0);
    Fixture();Bad=*Dbi;Bad.Base=MAX_UINT64-3;
    CranePcieDiagnoseAuxClockFrequency(&Io,C,&Bad);assert(MmioReads==0);
    Fixture();CranePcieDiagnoseAuxClockFrequency(&Io,C,NULL);assert(MmioReads==0);
    Fixture();PcieIoOps NoRead=Io;NoRead.Read32=NULL;
    CranePcieDiagnoseAuxClockFrequency(&NoRead,C,Dbi);assert(MmioReads==0);
    Fixture();PcieTargetController Other=*C;Other.Compatible="qcom,unknown-pcie";
    CranePcieDiagnoseAuxClockFrequency(&Io,&Other,Dbi);assert(MmioReads==0);
    Fixture();Bad=*Dbi;Bad.Size=0xB44;
    CranePcieDiagnoseAuxClockFrequency(&Io,C,&Bad);assert(AuxFrequencyReads==1);
  }
  Fixture(); FailedPin=HDK_PORTS==3?14:67; Snapshot(1);
  assert(strstr(Output,"readback failed") && strstr(Output,"negotiated-speed=2"));
  /* A malformed optional sequence must not permit an out-of-range read. */
  Fixture(); PcieTargetController Invalid=Target->Controllers[1];
  PcieTargetPowerStep Step={PCIE_POWER_SUPPLY,Target->SupplyCount,0};
  PcieTargetPowerSequence Sequence={&Step,1};Invalid.PowerSequence=&Sequence;
  CranePcieDiagnoseLink(&Io,&Invalid);assert(strstr(Output,"invalid resource"));
  Sequence.Count=65;Output[0]=0;OutputBytes=0;CranePcieDiagnoseLink(&Io,&Invalid);
  assert(strstr(Output,"invalid board sequence"));
  /* Missing/unmapped clock supplier grants no diagnostic MMIO. */
  Fixture();HaveClockContext=FALSE;Snapshot(1);assert(ClockReads==0);
  Fixture();mGcc.MemMapped=FALSE;Snapshot(1);assert(ClockReads==0);
  Fixture();mGcc.Size=3;Snapshot(1);assert(ClockReads==0);
  /* Bounds also apply to individual descriptor offsets and parent cycles. */
  Fixture();UINT32 Saved= gcc_pcie_1_pipe_clk.EnableRegister;
  gcc_pcie_1_pipe_clk.EnableRegister=mGcc.Size;Snapshot(1);
  assert(strstr(Output,"clock-hw gcc_pcie_1_pipe_clk invalid register"));
  gcc_pcie_1_pipe_clk.EnableRegister=Saved;
  Fixture();ClockNode *Cycle[]={&gcc_pcie_1_aux_clk};
  ClockNode **ParentsBefore=gcc_pcie_1_aux_clk.Parents;
  gcc_pcie_1_aux_clk.Parents=Cycle;Snapshot(1);
  assert(ClockReads<150);gcc_pcie_1_aux_clk.Parents=ParentsBefore;
  /* Each exposed root includes its own board interface, without any writes. */
  for(UINTN P=0;P<HDK_PORTS;++P) {
    Fixture();Snapshot(P);assert(GpioReads>=4);
    assert(AuxFrequencyReads==(HDK_PORTS==3?1:0));
  }
  printf("HDK %u RCs: endpoint supplies/GPIO/state, capability-relative speed/lanes, pre-PERST/timeout, read failures and read-only bounds passed\n",HDK_PORTS);
  return 0;
}
