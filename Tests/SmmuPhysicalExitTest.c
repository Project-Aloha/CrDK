/* SPDX-License-Identifier: MIT */
/* Actual core; source-derived cache attributes and modeled physical routing. */
#define main OriginalSmmuRegressionSuite
#include "SmmuLibTest.c"
#undef main
#include "AndroidSctlrOracle.h"
static BOOLEAN Coherent;
static UINT32 IgnoredSctlrBits;
static CR_STATUS Exit(SmmuDomain *D) {
#ifdef EXPECT_OLD_COHERENCY
  return SmmuReleaseToPhysicalAtExit(D);
#else
  return Coherent ? SmmuReleaseToCoherentPhysicalAtExit(D) : SmmuReleaseToPhysicalAtExit(D);
#endif
}
static SmmuDevice Dev;
static SmmuDomain Usb, Other;
static UINT64 *Tab;
static unsigned TraceCount, FaultWrite, ChangeWrite;
static UINT64 TraceAddress[16];
static UINT32 TraceValue[16], ChangeOffset, ChangeValue;
static BOOLEAN Stuck;
static UINT32 ExitRead(VOID *C,UINT64 A) {
  if(Stuck && A==Usb.BankBase+0x7f4) return 1;
  return Read32(C,A);
}
static VOID ExitWrite(VOID *C,UINT64 A,UINT32 V) {
  assert(TraceCount<16); TraceAddress[TraceCount]=A;TraceValue[TraceCount++]=V;
  if(TraceCount==FaultWrite) {
    if(A==Usb.BankBase) return; /* SCTLR write rejected. */
    Stuck=TRUE; /* Maintenance cannot establish completion. */
  }
  if(A==Usb.BankBase) V &= ~IgnoredSctlrBits;
  Write32(C,A,V);
  if(TraceCount==ChangeWrite) Registers[ChangeOffset/4]=ChangeValue;
}
static VOID Seed(void) {
  Init(&Dev,&Usb,&Other);
  Registers[0x20/4]=0x4c017e71;Registers[0x24/4]=0x6000004b;
  Registers[0x28/4]=0x5111;Registers[0]=0x210406;
  /* A real unrelated enabled stream/CB which must remain byte exact. */
  Registers[SMR(0)/4]=0x800002f0; Registers[S2CR(0)/4]=0;
  Registers[CB(0)/4]=0x1027; Registers[(0x1000+0*4)/4]=0x1f300;
  assert(SmmuProbe(&Dev,BASE,sizeof(Registers),&Ops)==CR_SUCCESS);
  Tab=Tables();
  assert(SmmuAttach(&Dev,&Usb,Coherent?0xe0:0,Tab,0xa77173000ULL)==CR_SUCCESS);
  assert(SmmuSetMapping(&Usb,SMMU_IOVA_BASE,0xa7ff12000ULL,1,3)==CR_SUCCESS);
  assert(SmmuBlock(&Usb)==CR_SUCCESS);
  TraceCount=FaultWrite=ChangeWrite=0;IgnoredSctlrBits=0;Stuck=FALSE;
  Dev.Io.Read32=ExitRead;Dev.Io.Write32=ExitWrite;
}
/* Independent architectural routing model: Linux qcom_smmu_write_s2cr uses
 * a TRANS stream pointing at SCTLR0/S1_TRANS_S2_BYPASS to pass the full PA.
 * This is not real hardware validation. */
static UINT64 UsbBusAddress(UINT64 Address) {
  unsigned Matches=0;UINT32 Route=0;
  for(unsigned I=0;I<Dev.StreamCount;++I) {
    UINT32 Smr=Registers[SMR(I)/4],Mask=(Smr>>16)&0x7fff;
    if((Smr&0x80000000)&&(((Smr^Usb.Sid)&~Mask&0xffff)==0)) {
      ++Matches;Route=Registers[S2CR(I)/4];
    }
  }
  if(!Matches) return (Registers[0]&(1U<<10))?MAX_UINT64:Address;
  if(Matches!=1 || (Route&0x30000)) return MAX_UINT64;
  unsigned Bank=Route&0xff;
  if((Registers[(0x1000+Bank*4)/4]&0x30000)!=0x10000) return MAX_UINT64;
  if(!(Registers[CB(Bank)/4]&1)) return Address;
  assert(Bank==Usb.Bank);return Translate(&Usb,Address);
}
#ifndef EXPECT_OLD_COHERENCY
static VOID Closed(SmmuDomain *D) {
  unsigned W=Writes,T=TraceCount;SmmuMappingEvidence E;
  assert(SmmuSetMapping(D,SMMU_IOVA_BASE,0x81234000,1,3)!=CR_SUCCESS);
  assert(SmmuBlock(D)!=CR_SUCCESS);
  assert(SmmuDetach(D)!=CR_SUCCESS);
  assert(SmmuReleaseToPhysicalAtExit(D)!=CR_SUCCESS);
  assert(SmmuReleaseToCoherentPhysicalAtExit(D)!=CR_SUCCESS);
  assert(SmmuQueryRevokedMapping(D,SMMU_IOVA_BASE,SMMU_IOVA_SIZE/4096,&E)!=CR_SUCCESS);
  assert(SmmuQueryMappingEvidence(D,SMMU_IOVA_BASE,0x81234000,1,3,&E)!=CR_SUCCESS);
  assert(SmmuAttach(&Dev,D,0,Tab,0xa77173000ULL)!=CR_SUCCESS);
  assert(Writes==W && TraceCount==T);
}
static VOID Success(void) {
  Seed();SmmuDomain Copy=Usb;UINT32 Before[sizeof(Registers)/4];
  memcpy(Before,Registers,sizeof(Before));
  assert(UsbBusAddress(0x81234000)==MAX_UINT64);
  assert(UsbBusAddress(0xa7ff12000ULL)==MAX_UINT64);
  assert(Exit(&Usb)==CR_SUCCESS);
  assert(Usb.ExitState==SmmuDomainExitPhysical && Usb.Active && !Usb.Failed);
  assert(Dev.PhysicalExitBanks[Usb.Bank] && Dev.ReservedBanks[Usb.Bank] &&
    Dev.ReservedStreams[Usb.Stream] && Dev.ReservedAsids[Usb.Asid]);
  assert(TraceCount==5);
  UINT32 Off[5]={0x610,0x7f0,0,0x610,0x7f0};
  for(unsigned I=0;I<5;++I) assert(TraceAddress[I]==Usb.BankBase+Off[I]);
  assert(TraceValue[0]==Usb.Asid && TraceValue[1]==~0U &&
    TraceValue[2]==(Coherent?AndroidCoherentAttributes():0) && TraceValue[3]==Usb.Asid && TraceValue[4]==~0U);
  for(unsigned I=0;I<sizeof(Registers)/4;++I) {
    UINT64 Addr=BASE+I*4;
    if(Addr!=Usb.BankBase && Addr!=Usb.BankBase+0x610 && Addr!=Usb.BankBase+0x7f0)
      assert(Registers[I]==Before[I]);
  }
  assert(UsbBusAddress(0x81234000)==0x81234000);
  assert(UsbBusAddress(0xa7ff12000ULL)==0xa7ff12000ULL);
  assert(UsbBusAddress(0xa76e83000ULL)==0xa76e83000ULL);
  if(Coherent) assert(Registers[(Usb.BankBase-BASE)/4]==AndroidCoherentAttributes());
  Closed(&Usb);Closed(&Copy);
  /* A fresh domain cannot claim the handed-off SID or last/reserved bank. */
  SmmuDomain Fresh={0};assert(SmmuAttach(&Dev,&Fresh,Usb.Sid,Tab,0xa77173000ULL)==CR_BUSY);
  assert(TraceCount==5);free(Tab);
}
static VOID Rejects(void) {
  for(unsigned Case=0;Case<13;++Case) {
    Seed();unsigned W=Writes;
    switch(Case) {
    case 0:Tab[1024]=3;break; /* Live leaf. */
    case 1:Registers[SMR(Usb.Stream)/4]|=0x10000;break;
    case 2:Registers[SMR(12)/4]=(0x80010000U | (Usb.Sid^1));break; /* Wildcard SID0. */
    case 3:Registers[SMR(12)/4]=0x80000100;Registers[S2CR(12)/4]=Usb.Bank;break;
    case 4:Registers[(0x1000+Usb.Bank*4)/4]=0;break; /* Stage2. */
    case 5:Registers[(0x1800+Usb.Bank*4)/4]=0;break;
    case 6:Registers[0]|=8;break;
    case 7:Registers[0]|=1;break;
    case 8:Tab[1]=3;break;
    case 9:Tab[512]=3;break;
    case 10:Dev.ReservedBanks[Usb.Bank]=FALSE;break;
    case 11:Usb.Failed=TRUE;break;
    case 12:Dev.SyncFailed=TRUE;break;
    }
    assert(Exit(&Usb)!=CR_SUCCESS);
    assert(TraceCount==0 && Writes==W && Usb.ExitState==SmmuDomainExitNormal);
    free(Tab);
  }
}
static VOID WriteFailures(void) {
  for(unsigned I=1;I<=5;++I) {
    Seed();SmmuDomain Copy=Usb;FaultWrite=I;
    assert(Exit(&Usb)!=CR_SUCCESS);
    assert(Usb.ExitState==SmmuDomainExitPending && Usb.Failed && Usb.Active);
    assert(Dev.PhysicalExitBanks[Usb.Bank] && Dev.ReservedBanks[Usb.Bank] &&
      Dev.ReservedStreams[Usb.Stream] && Dev.ReservedAsids[Usb.Asid]);
    assert(TraceCount==(I<=2?2:I==3?3:5));
    Closed(&Usb);Closed(&Copy);free(Tab);
  }
}
static VOID ReadbackChanges(void) {
  /* Inject an actual route/context change before SCTLR or final admission. */
  for(unsigned Case=0;Case<8;++Case) {
    Seed();SmmuDomain Copy=Usb;ChangeWrite=Case==0?2:5;
    switch(Case) {
    case 0:ChangeOffset=SMR(12);ChangeValue=(0x80010000U | (Usb.Sid^1));break;
    case 1:ChangeOffset=0;ChangeValue=0x210407;break;
    case 2:ChangeOffset=SMR(Usb.Stream);ChangeValue=0x80000000U|(Usb.Sid^1);break;
    case 3:ChangeOffset=S2CR(Usb.Stream);ChangeValue=Usb.Bank+1;break;
    case 4:ChangeOffset=0x1000+Usb.Bank*4;ChangeValue=0;break;
    case 5:ChangeOffset=0x1800+Usb.Bank*4;ChangeValue=0;break;
    case 6:ChangeOffset=(UINT32)(Usb.BankBase-BASE);ChangeValue=1;break;
    case 7:ChangeOffset=SMR(12);ChangeValue=(0x80010000U | (Usb.Sid^1));break;
    }
    assert(Exit(&Usb)!=CR_SUCCESS);
    assert(Usb.ExitState==SmmuDomainExitPending && Usb.Failed);
    assert(TraceCount==(Case==0?2:5));
    Closed(&Usb);Closed(&Copy);free(Tab);
  }
}
static VOID CoherenceFailures(void) {
  const UINT32 Policy=AndroidCoherentAttributes();
  assert(Policy==0x0a5f0000U && !(Policy&1));
  /* Exact hardware ignores one demanded bit at a time, not only an entire
   * SCTLR write. Both at write readback and after final sync must fail. */
  for(unsigned Bit=0;Bit<32;++Bit) if(Policy&(1U<<Bit)) {
    Seed();SmmuDomain Copy=Usb;IgnoredSctlrBits=1U<<Bit;
    assert(Exit(&Usb)==CR_DEVICE_ERROR);
    assert(Usb.ExitState==SmmuDomainExitPending && Usb.Failed && TraceCount==3);
    Closed(&Usb);Closed(&Copy);free(Tab);
    Seed();Copy=Usb;ChangeWrite=5;ChangeOffset=(UINT32)(Usb.BankBase-BASE);
    ChangeValue=Policy&~(1U<<Bit);
    assert(Exit(&Usb)==CR_DEVICE_ERROR);
    assert(Usb.ExitState==SmmuDomainExitPending && Usb.Failed && TraceCount==5);
    Closed(&Usb);Closed(&Copy);free(Tab);
  }
  /* M accidentally retained would translate; unrequested bits are rejected
   * too, including both other values of the allocation/shareability fields. */
  for(unsigned Bit=0;Bit<32;++Bit) if(!(Policy&(1U<<Bit))) {
    Seed();ChangeWrite=3;ChangeOffset=(UINT32)(Usb.BankBase-BASE);
    ChangeValue=Policy|(1U<<Bit);
    assert(Exit(&Usb)==CR_DEVICE_ERROR && Usb.Failed && TraceCount==3);
    Closed(&Usb);free(Tab);
  }
}
static VOID CacheVisibilityModel(void) {
  /* Source-backed attribute/routing model only, not a CPU/interconnect test.
   * With _CCA=1 a dirty CPU command line may not be cleaned before DMA. The
   * source's cache override is the contract for coherent access to that line.
   * A successful unqualified physical route alone cannot prove visibility. */
  for(unsigned Kind=0;Kind<2;++Kind) {
    Coherent=(BOOLEAN)Kind;Seed();
    UINT32 DramLine=0,DirtyCpuLine=0x55545244;
    assert(Exit(&Usb)==CR_SUCCESS);
    assert(UsbBusAddress(0xa76e83000ULL)==0xa76e83000ULL);
    BOOLEAN Cacheable=(Registers[(Usb.BankBase-BASE)/4]&AndroidAttributeMask())==AndroidCoherentAttributes();
    UINT32 DmaRead=Cacheable?DirtyCpuLine:DramLine;
    assert(DmaRead==(Coherent?DirtyCpuLine:DramLine));
    free(Tab);
  }
}
#endif
int main(void) {
#ifdef EXPECT_OLD_COHERENCY
  Coherent=TRUE;Seed();assert(Exit(&Usb)==CR_SUCCESS);
  assert(UsbBusAddress(0xa76e83000ULL)==0xa76e83000ULL);
  assert(Registers[(Usb.BankBase-BASE)/4]==AndroidCoherentAttributes());
#else
  for(unsigned Kind=0;Kind<2;++Kind) {
    Coherent=(BOOLEAN)Kind;Success();Rejects();WriteFailures();ReadbackChanges();
  }
  Coherent=TRUE;CoherenceFailures();CacheVisibilityModel();
  puts("PASS: USB/UFS exact physical exit, Android SCTLR oracle, PA 0xa76e83000, per-bit cache readback, five-write failures, terminal ownership, aliases/global isolation.");
#endif
  return 0;
}
