/* Actual SM8250/8350 all-RC cold bring-up and board power rollback.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#define mPcieContext mCorePcieContext
#include "../Library/PcieLib/pcie.c"
#undef mPcieContext
#include "../Library/PciHostBridgeLib/PciHostBridgeLib.c"
#include <Library/CrTargetClockLib.h>
#include <Library/CrTargetSmmuLib.h>

STATIC CONST PcieTargetContext *T;
STATIC struct {UINT64 A;UINT32 V;} Regs[8192];
STATIC UINTN Nr, Calls, FailAt, ReleaseCalls, FailRelease;
STATIC UINTN BoardGpioCalls, FailBoardGpio;
STATIC BOOLEAN Power[3], Fabric[3], Dma[3], Perst[3], Endpoint[3];
STATIC UINTN ClockVotes[128], SupplyVotes[64];
STATIC INTN FailedPort=-1;
STATIC BOOLEAN NoLink;
STATIC UINT64 Time;
STATIC UINTN PrePerstSnapshots[3], TimeoutSnapshots[3];
ClockDriverContext *CrDalGetClockContext(VOID) {return NULL;}
CR_STATUS CrDalClockResolveResource(CONST CHAR8 *C,CONST CHAR8 *Id,
    CR_DAL_CLOCK_RESOURCE_KIND Kind,CONST CHAR8 **Native,UINT32 *Flags) {
  (VOID)C;(VOID)Id;(VOID)Kind;(VOID)Native;(VOID)Flags;return CR_NOT_FOUND;
}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...) {(VOID)L;(VOID)F;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) {(VOID)L;return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID) {return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D) {(VOID)F;(VOID)L;(VOID)D;assert(0);}
VOID *EFIAPI SetMem(VOID *B,UINTN N,UINT8 V) {return memset(B,V,N);}
VOID *EFIAPI ZeroMem(VOID *B,UINTN N) {return memset(B,0,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) {return strcmp(A,B);}
UINT64 EFIAPI LShiftU64(UINT64 V,UINTN N) {return V<<N;}
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L) {*L=1;return L;}
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L) {assert(*L==1);*L=2;return L;}
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L) {assert(*L==2);*L=1;return L;}
VOID EFIAPI MemoryFence(VOID) {}
STATIC UINT64 Base(UINTN P,CONST CHAR8 *Name) {
 return FindRegion(T,&T->Controllers[P],Name)->Base;
}
STATIC UINT32 Read(VOID *C,UINT64 A) {
 (VOID)C;
 for(UINTN P=0;P<T->ControllerCount;P++) {
  UINT64 D=Base(P,"dbi");
  if(A==D+0x34)return 0x70;
  if(A==D+0x70)return 0x10;
  if(A==D+0x7c)return 3;
  if(A==D+0x80)return NoLink?0:0x20000000;
 }
 for(UINTN I=0;I<Nr;I++)if(Regs[I].A==A)return Regs[I].V;
 return 0;
}
STATIC VOID Write(VOID *C,UINT64 A,UINT32 V) {
 (VOID)C;
 BOOLEAN InRange=FALSE;
 for(UINTN P=0;P<T->ControllerCount;P++) {
  for(UINTN R=0;R<T->Controllers[P].RegionCount;R++) {
   CONST PcieTargetRegion *Region=&T->Regions[T->Controllers[P].RegionOffset+R];
   if(A>=Region->Base && A+4<=Region->Base+Region->Size)InRange=TRUE;
  }
  CONST PcieTargetPhy *Phy=&T->Phys[P];
  if(A>=Phy->Base && A+4<=Phy->Base+Phy->Size) {InRange=TRUE;assert(Perst[P]&&Power[P]&&Fabric[P]);}
  if(A==Base(P,"parf")+PCIE_PARF_LTSSM && (V&PCIE_PARF_LTSSM_ENABLE))
   assert(!Perst[P]&&Dma[P]&&Endpoint[P]&&Power[P]&&Fabric[P]);
 }
 assert(InRange);
 for(UINTN I=0;I<Nr;I++)if(Regs[I].A==A) {Regs[I].V=V;return;}
 assert(Nr<ARRAY_SIZE(Regs));Regs[Nr].A=A;Regs[Nr++].V=V;
}
STATIC VOID Delay(VOID *C,UINT32 Us) {(VOID)C;Time+=Us;}
STATIC UINT64 Now(VOID *C) {(VOID)C;return Time;}
STATIC CR_STATUS Clock(VOID *C,CONST PcieTargetClock *K,BOOLEAN E) {
 (VOID)C;UINTN I=K-T->Clocks;assert(I<T->ClockCount);
 if(E)ClockVotes[I]++;else {assert(ClockVotes[I]);ClockVotes[I]--;}
 return CR_SUCCESS;
}
STATIC CR_STATUS Supply(VOID *C,CONST PcieTargetSupply *S,BOOLEAN E) {
 (VOID)C;UINTN I=S-T->Supplies;assert(I<T->SupplyCount);
 if(E) {SupplyVotes[I]++;if(++Calls==FailAt)return CR_DEVICE_ERROR;}
 else {if(++ReleaseCalls==FailRelease)return CR_DEVICE_ERROR;assert(SupplyVotes[I]);SupplyVotes[I]--;}
 return CR_SUCCESS;
}
STATIC CR_STATUS Domain(VOID *C,CONST PcieTargetController *P,BOOLEAN E) {
 (VOID)C;Power[P->Domain]=E;return CR_SUCCESS;
}
STATIC CR_STATUS Icc(VOID *C,CONST PcieTargetController *P,BOOLEAN E) {
 (VOID)C;Fabric[P->Domain]=E;
 if(E && (INTN)P->Domain==FailedPort)return CR_DEVICE_ERROR;
 return CR_SUCCESS;
}
STATIC CR_STATUS Iommu(VOID *C,CONST PcieTargetController *P,BOOLEAN E) {
 (VOID)C;assert(Perst[P->Domain]);Dma[P->Domain]=E;return CR_SUCCESS;
}
STATIC CR_STATUS Reset(VOID *C,CONST PcieTargetReset *R,BOOLEAN E) {(VOID)C;(VOID)R;(VOID)E;return CR_SUCCESS;}
STATIC CR_STATUS Gpio(VOID *C,CONST PcieTargetGpio *G,PCIE_GPIO_DIRECTION D,BOOLEAN E) {
 (VOID)C;
 for(UINTN I=0;I<T->ControllerCount;I++) {
  if(G==&T->Gpios[T->Controllers[I].GpioOffset])Perst[I]=E;
  CONST PcieTargetPowerSequence *Seq=T->Controllers[I].PowerSequence;
  for(UINTN J=0;J<Seq->Count;++J) {
   CONST PcieTargetPowerStep *Step=&Seq->Steps[J];
   if((Step->Kind==PCIE_POWER_GPIO || Step->Kind==PCIE_POWER_PINCTRL) &&
      G==&T->Gpios[Step->ResourceIndex] && (E || D==PCIE_GPIO_PRESERVE)) {
    assert(Perst[I]);
    if(++BoardGpioCalls==FailBoardGpio)return CR_DEVICE_ERROR;
   }
  }
 }
 return CR_SUCCESS;
}
STATIC CR_STATUS BoardPower(CONST PcieIoOps *Io,CONST PcieTargetController *P,BOOLEAN E) {
 assert(Perst[P->Domain]);CR_STATUS S=CranePcieSetBoardPower(Io,P,E);
 if(!CR_ERROR(S))Endpoint[P->Domain]=E;
 return S;
}
STATIC VOID Diagnose(CONST PcieIoOps *Ops,CONST PcieTargetController *P) {
 UINTN Port=P->Domain;PciePortRuntime *Runtime=&mCorePcieContext.Ports[Port];
 UINT64 Before=Time;UINTN BeforeRegisters=Nr,BeforeCalls=Calls,BeforeGpios=BoardGpioCalls;
 assert(Endpoint[Port] && Power[Port] && Fabric[Port] && Dma[Port]);
 if(Perst[Port]) {
  assert(Runtime->State==PCIE_PORT_INITIALIZING && Runtime->TimingValidMask==1);
  assert(Time-Runtime->PerstAssertTimeUs>=100000);++PrePerstSnapshots[Port];
 } else {
  assert(NoLink && Runtime->State==PCIE_PORT_READY_NO_LINK && Runtime->TimingValidMask==7);
  ++TimeoutSnapshots[Port];
 }
 CranePcieDiagnoseLink(Ops,P);
 assert(Time==Before && Nr==BeforeRegisters && Calls==BeforeCalls && BoardGpioCalls==BeforeGpios);
}
STATIC PcieIoOps Io={.Read32=Read,.Write32=Write,.DelayUs=Delay,.GetTimeUs=Now,
 .SetClock=Clock,.SetSupply=Supply,.SetReset=Reset,.SetPowerDomain=Domain,
 .SetInterconnect=Icc,.SetIommu=Iommu,.SetGpio=Gpio,.SetEndpointPower=BoardPower,.DiagnoseLink=Diagnose};
STATIC VOID Clear(VOID) {
 Nr=Calls=FailAt=ReleaseCalls=FailRelease=0;Time=0;FailedPort=-1;NoLink=FALSE;
 BoardGpioCalls=FailBoardGpio=0;
 memset(Power,0,sizeof(Power));memset(Fabric,0,sizeof(Fabric));memset(Dma,0,sizeof(Dma));
 memset(Perst,0,sizeof(Perst));memset(Endpoint,0,sizeof(Endpoint));
 memset(ClockVotes,0,sizeof(ClockVotes));memset(SupplyVotes,0,sizeof(SupplyVotes));
 memset(PrePerstSnapshots,0,sizeof(PrePerstSnapshots));memset(TimeoutSnapshots,0,sizeof(TimeoutSnapshots));
 memset(&mPcieResources,0,sizeof(mPcieResources));mPcieTarget=T;
}
STATIC VOID Empty(VOID) {
 for(UINTN I=0;I<T->ClockCount;I++)assert(!ClockVotes[I]);
 for(UINTN I=0;I<T->SupplyCount;I++)assert(!SupplyVotes[I]);
 for(UINTN I=0;I<T->ControllerCount;I++)assert(!Power[I]&&!Fabric[I]&&!Dma[I]&&!Endpoint[I]);
}
int main(void) {
 T=CrTargetGetPcieContext();assert(T->ControllerCount==HDK_PORTS);
 for(UINTN I=0;I<T->ControllerCount;I++)assert(T->Controllers[I].Enabled&&T->Controllers[I].Domain==I);
 CrTargetSmmuContext *S=CrTargetGetSmmuContext();assert(S->SegmentCount==HDK_PORTS && S->CoherentStreamCount==HDK_PORTS*2);
 PcieDeviceContext *Ctx;UINT32 Mask;Clear();assert(PcieLibInit(&Ctx,T,&Io)==CR_SUCCESS);
 assert(PcieInitializeMask(Ctx,(1U<<HDK_PORTS)-1,&Mask)==CR_SUCCESS);assert(Mask==((1U<<HDK_PORTS)-1));
 for(UINTN P=0;P<T->ControllerCount;P++) {
  assert(PcieIsLinkUp(Ctx,P) && Endpoint[P]);
  assert(PrePerstSnapshots[P]==1 && TimeoutSnapshots[P]==0);
  UINT32 V=0;assert(PcieConfigAccess(Ctx,P,1,1,0,0,4,FALSE,&V)==CR_NOT_FOUND);
 }
 UINTN Total=Calls,TotalGpio=BoardGpioCalls;
 for(UINTN P=0;P<T->ControllerCount;P++)assert(PcieShutdownPort(Ctx,P)==CR_SUCCESS);Empty();
 /* A failure on any RC must leave every other RC attempted and working. */
 for(UINTN P=0;P<T->ControllerCount;P++) {
  Clear();FailedPort=P;assert(PcieLibInit(&Ctx,T,&Io)==CR_SUCCESS);
  assert(PcieInitializeMask(Ctx,(1U<<HDK_PORTS)-1,&Mask)==CR_SUCCESS);
  assert(Mask==(((1U<<HDK_PORTS)-1)&~(1U<<P)));
  for(UINTN Q=0;Q<T->ControllerCount;Q++)assert(PcieShutdownPort(Ctx,Q)==CR_SUCCESS);Empty();
 }
 /* Every partial supply acquisition, including endpoint rails, is unwound. */
 for(UINTN F=1;F<=Total;F++) {
  Clear();FailAt=F;assert(PcieLibInit(&Ctx,T,&Io)==CR_SUCCESS);
  (VOID)PcieInitializeMask(Ctx,(1U<<HDK_PORTS)-1,&Mask);
  for(UINTN P=0;P<T->ControllerCount;P++)assert(PcieShutdownPort(Ctx,P)==CR_SUCCESS);Empty();
 }
 for(UINTN F=1;F<=TotalGpio;F++) {
  Clear();FailBoardGpio=F;assert(PcieLibInit(&Ctx,T,&Io)==CR_SUCCESS);
  (VOID)PcieInitializeMask(Ctx,(1U<<HDK_PORTS)-1,&Mask);
  for(UINTN P=0;P<T->ControllerCount;P++)assert(PcieShutdownPort(Ctx,P)==CR_SUCCESS);Empty();
 }
 Clear();NoLink=TRUE;assert(PcieLibInit(&Ctx,T,&Io)==CR_SUCCESS);
 assert(PcieInitializeMask(Ctx,(1U<<HDK_PORTS)-1,&Mask)==CR_SUCCESS && Mask==((1U<<HDK_PORTS)-1));
 assert(Ctx->LinkMask==0);
 for(UINTN P=0;P<T->ControllerCount;P++)assert(PrePerstSnapshots[P]==1 && TimeoutSnapshots[P]==1);
 FailRelease=1;assert(PcieShutdownPort(Ctx,0)==CR_DEVICE_ERROR);assert(Ctx->ResourceMask&1);assert(Power[0]);
 FailRelease=0;assert(PcieShutdownPort(Ctx,0)==CR_SUCCESS);
 for(UINTN P=1;P<T->ControllerCount;P++)assert(PcieShutdownPort(Ctx,P)==CR_SUCCESS);Empty();
 printf("HDK %u RCs: actual target cold-init, endpoint power, all-port isolation, %zu supply failures, no-link and retained cleanup passed\n",HDK_PORTS,(size_t)Total);
 return 0;
}
