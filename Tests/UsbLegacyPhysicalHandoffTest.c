/* SPDX-License-Identifier: MIT
 * Composition of the actual native DWC3 publisher, native SMMU adapter and
 * portable SMMU core. Only firmware services and hardware observations are
 * modeled. The existing publisher fixture supplies the firmware services.
 */
static unsigned char LegacyPolicy;
#ifndef _PCD_VALUE_PcdUfsLegacyCoherentDmaHandoff
#define _PCD_VALUE_PcdUfsLegacyCoherentDmaHandoff FALSE
#endif
#define _PCD_VALUE_PcdUsbLegacyPhysicalDmaHandoff LegacyPolicy
#include <PiDxe.h>
// The inherited fixture's service stub has the same spelling as this unused
// library entry point; retain actual EFI_BOOT_SERVICES field names unchanged.
#define FreePages UnusedMemoryAllocationFreePages
#include <Library/MemoryAllocationLib.h>
#undef FreePages
#define main OriginalDwc3HandoffFixtureMain
#include "Dwc3HandoffTest.c"
#undef main
#include "../Driver/SmmuCrDxe/SmmuCrDxe.c"

#define HW_BASE 0x15000000ULL
#define HW_CB(N) (0x80000U + (N) * 4096U)
#define HW_TABLE_BYTES (SMMU_TABLE_PAGES * SMMU_PAGE_SIZE)
STATIC UINT32 Hw[0x100000 / 4], SavedHw[0x100000 / 4];
STATIC UINTN HwWrites, PhysicalWrites, MaintenanceWrites;
STATIC BOOLEAN RejectPhysical, TimeoutPhysical;
STATIC UINT32 Role, StopFailure;
STATIC VOID *OwnedTables;

EFI_LOCK *EFIAPI EfiInitializeLock(EFI_LOCK *L, EFI_TPL T) {
  L->Tpl=T;L->OwnerTpl=0;L->Lock=EfiLockReleased;return L;
}
VOID EFIAPI EfiAcquireLock(EFI_LOCK *L) {
  assert(L->Lock==EfiLockReleased);L->Lock=EfiLockAcquired;
}
VOID EFIAPI EfiReleaseLock(EFI_LOCK *L) {
  assert(L->Lock==EfiLockAcquired);L->Lock=EfiLockReleased;
}
STATIC UINT32 HwRead(VOID *Cookie, UINT64 A) {
  (VOID)Cookie;assert(A>=HW_BASE && A<HW_BASE+sizeof(Hw) && !(A&3));
  if(TimeoutPhysical && PhysicalWrites && (A&0xfff)==0x7f4)return 1;
  return Hw[(A-HW_BASE)/4];
}
STATIC VOID HwWrite(VOID *Cookie, UINT64 A, UINT32 V) {
  (VOID)Cookie;assert(A>=HW_BASE && A<HW_BASE+sizeof(Hw) && !(A&3));++HwWrites;
  if(A>=HW_BASE+HW_CB(0) && (A&0xfff)==0x7f0)++MaintenanceWrites;
  if(Ebs) {
    UINT64 C=mSmmu.Domains[0].Domain.BankBase;
    // Exit writes may touch only the exact owned bank and its maintenance.
    assert(A==C || A==C+0x610 || A==C+0x7f0);
    if(A==C) {
      assert(V==0);++PhysicalWrites;
      assert(mSmmu.AfterExitBootServices && mSmmu.ExitComplete && mSmmu.ExitObserversActive);
      assert(mSmmu.Lock.Lock==EfiLockAcquired);
      assert(mUsbHandoff.Local.RevocationStatus==EFI_UNSUPPORTED);
      SmmuMappingEvidence Zero={0};assert(!memcmp(&mUsbHandoff.Local.Revoked,&Zero,sizeof(Zero)));
      if(RejectPhysical)return;
    }
  }
  Hw[(A-HW_BASE)/4]=V;
}
STATIC VOID HwWrite64(VOID *C, UINT64 A, UINT64 V) {
  HwWrite(C,A,(UINT32)V);HwWrite(C,A+4,(UINT32)(V>>32));
}
STATIC VOID HwPublish(VOID *C, VOID *A, UINTN N) {(VOID)C;assert(A && N);}
STATIC VOID HwDelay(VOID *C, UINT32 Us) {(VOID)C;(VOID)Us;}
UINT32 EFIAPI MmioRead32(UINTN A) {return HwRead(NULL,A);}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) {HwWrite(NULL,A,V);return V;}
UINT64 EFIAPI MmioWrite64(UINTN A,UINT64 V) {HwWrite64(NULL,A,V);return V;}
UINTN EFIAPI MicroSecondDelay(UINTN Us) {return Us;}
STATIC EFI_STATUS FreezeDevice(CR_USB_NATIVE_HANDOFF *R) {
  assert(Ebs);R->FrozenWriters=R->DrainedWriters=7;
  R->Stop=(CR_USB_CONTROLLER_STOP){.Role=Role,.Id=0x33310000,.Revision=0x3139302a,
    .GlobalControl=Role<<12,.HostStatus=1,.DeviceStatus=1U<<22,
    .EventBuffers=1,.MaskedEventBuffers=1};
  R->HaltStatus=R->InterruptStatus=EFI_SUCCESS;
  if(StopFailure==1)R->ActiveCalls[0]=1;
  if(StopFailure==2)R->DrainedWriters=3;
  if(StopFailure==3)R->HaltStatus=EFI_TIMEOUT;
  if(StopFailure==4)R->InterruptStatus=EFI_WARN_UNKNOWN_GLYPH;
  if(StopFailure==5)R->Stop.DeviceControl=1U<<31;
  if(StopFailure==6)R->Stop.MaskedEventBuffers=0;
  if(StopFailure==7)R->FailedWriters=1;
  if(StopFailure==8)R->Stop.DeviceEvents=1;
  if(StopFailure==9)R->Stop.DeviceEndpoints=1;
  if(StopFailure==10)R->Stop.DeviceStatus=0;
  if(StopFailure==11)return EFI_WARN_UNKNOWN_GLYPH;
  return EFI_SUCCESS;
}
STATIC VOID Start(VOID) {
  Setup();LegacyPolicy=TRUE;Role=CR_USBH_FUNCTION;StopFailure=0;
  RejectPhysical=TimeoutPhysical=FALSE;HwWrites=PhysicalWrites=MaintenanceWrites=0;
  memset(&mSmmu,0,sizeof(mSmmu));memset(&mCrProtocol,0,sizeof(mCrProtocol));memset(Hw,0,sizeof(Hw));
  Hw[0x20/4]=0x48000000|128;Hw[0x24/4]=0x60020010;Hw[0x28/4]=0x1022;
  // Preserve a foreign firmware-owned route and an otherwise unused bank.
  Hw[0x800/4]=0x80000300;Hw[0xc00/4]=3;Hw[HW_CB(3)/4]=0x27;
  SmmuIoOps Io={HwRead,HwWrite,HwWrite64,HwPublish,HwDelay,NULL};
  assert(SmmuProbe(&mSmmu.Device,HW_BASE,sizeof(Hw),&Io)==CR_SUCCESS);
  OwnedTables=aligned_alloc(SMMU_PAGE_SIZE,HW_TABLE_BYTES);assert(OwnedTables);
  CR_SMMU_DOMAIN *D=&mSmmu.Domains[0];D->InUse=TRUE;D->Bdf=0xffff;
  D->Tables=OwnedTables;D->TableAddress=0x83000000;
  assert(SmmuAttach(&mSmmu.Device,&D->Domain,0,OwnedTables,D->TableAddress)==CR_SUCCESS);
  assert(SmmuSetMapping(&D->Domain,SMMU_IOVA_BASE,0x90000000,1,SMMU_ACCESS_READ|SMMU_ACCESS_WRITE)==CR_SUCCESS);
  mSmmu.Initialized=TRUE;mSmmu.DomainLimit=32;EfiInitializeLock(&mSmmu.Lock,TPL_NOTIFY);
  mCrProtocol.Revision=EFI_SMMU_CR_PROTOCOL_REVISION;
  mCrProtocol.SetExitPrepareObserver=CrSmmuSetExitPrepareObserver;
  mCrProtocol.SetExitObserver=CrSmmuSetExitObserver;
  mCrProtocol.QueryExitRevokedDomain=CrSmmuQueryExitRevokedDomain;
  mCrProtocol.ReleaseUsbToPhysicalAtExit=CrSmmuReleaseUsbToPhysicalAtExit;
  assert(CrUsbNativeInit(&mCrProtocol,FreezeDevice)==EFI_SUCCESS && mUsbHandoff.Installed);
  memcpy(SavedHw,Hw,sizeof(Hw));
}
STATIC VOID End(VOID) {
  // Only simulated reboot releases resources; production exit retains all.
  Reboot();free(OwnedTables);OwnedTables=NULL;memset(&mSmmu,0,sizeof(mSmmu));
}
STATIC VOID CheckPreserved(VOID) {
  UINTN C=(mSmmu.Domains[0].Domain.BankBase-HW_BASE)/4;
  for(UINTN I=0;I<ARRAY_SIZE(Hw);++I)
    if(I!=C && I!=C+0x610/4 && I!=C+0x7f0/4)assert(Hw[I]==SavedHw[I]);
  assert(mSmmu.Domains[0].InUse && mSmmu.Domains[0].Domain.Active);
  assert(mSmmu.Device.ReservedBanks[mSmmu.Domains[0].Domain.Bank]);
  assert(mSmmu.Device.ReservedStreams[mSmmu.Domains[0].Domain.Stream]);
  assert(mSmmu.Device.ReservedAsids[mSmmu.Domains[0].Domain.Asid]);
  assert(Pages==1 && Pools==1 && Tables==1 && OwnedTables);
}
STATIC VOID Exit(BOOLEAN Early) {
  Ebs=TRUE;if(Early)CrUsbNativeExit();CrSmmuExitBootServices(NULL,NULL);
  assert(mSmmu.ExitComplete && !mSmmu.ExitObserversActive && mUsbHandoff.Published);
  assert(CrUsbHandoffValidate(Page)==CR_SUCCESS);CheckPreserved();
  UINTN N=HwWrites,F=Flushes;CrSmmuExitBootServices(NULL,NULL);CrUsbNativeExit();
  assert(HwWrites==N && Flushes==F);
  assert(CrSmmuReleaseUsbToPhysicalAtExit(&mCrProtocol)==(LegacyPolicy?EFI_ACCESS_DENIED:EFI_UNSUPPORTED));
  assert(HwWrites==N && CrUsbNativeRelease()==EFI_ACCESS_DENIED);
}
STATIC VOID CheckUncertain(VOID) {
  CR_USB_NATIVE_HANDOFF *R=Page;assert(R->State==CR_USBH_UNCERTAIN);
  assert(CrUsbHandoffCanTakeover(R,Locator.Generation)==CR_NOT_READY);
}
STATIC VOID CheckTerminal(BOOLEAN Success) {
  SmmuDomain *D=&mSmmu.Domains[0].Domain;SmmuMappingEvidence Evidence;
  assert(D->ExitState==(Success?SmmuDomainExitPhysical:SmmuDomainExitPending));
  assert(mSmmu.Device.PhysicalExitBanks[D->Bank]);
  assert(mUsbHandoff.Local.RevocationStatus==EFI_UNSUPPORTED);
  assert(CrSmmuQueryExitRevokedDomain(&mCrProtocol,0,0xffff,0,&Evidence)!=EFI_SUCCESS);
  UINTN N=HwWrites;
  assert(SmmuSetMapping(D,SMMU_IOVA_BASE,0x90000000,1,SMMU_ACCESS_READ)!=CR_SUCCESS);
  assert(SmmuBlock(D)!=CR_SUCCESS && SmmuDetach(D)!=CR_SUCCESS);
  assert(SmmuReleaseToPhysicalAtExit(D)!=CR_SUCCESS && HwWrites==N);
  CheckUncertain();
}
STATIC VOID TestProducerAdmission(VOID) {
  for(UINTN Case=0;Case<14;++Case) {
    Start();mSmmu.AfterExitBootServices=mSmmu.ExitComplete=mSmmu.ExitObserversActive=TRUE;
    EFI_SMMU_CR_PROTOCOL *This=&mCrProtocol;EFI_STATUS Expected=EFI_ACCESS_DENIED;
    CR_SMMU_DOMAIN *D=&mSmmu.Domains[0];
    switch(Case) {
      case 0:This=NULL;Expected=EFI_INVALID_PARAMETER;break;
      case 1:LegacyPolicy=FALSE;Expected=EFI_UNSUPPORTED;break;
      case 2:mSmmu.Initialized=FALSE;Expected=EFI_UNSUPPORTED;break;
      case 3:mSmmu.ExistingIoMmu=TRUE;Expected=EFI_UNSUPPORTED;break;
      case 4:mSmmu.Device.Base+=0x100000;Expected=EFI_UNSUPPORTED;break;
      case 5:mSmmu.AfterExitBootServices=FALSE;break;
      case 6:mSmmu.ExitComplete=FALSE;break;
      case 7:mSmmu.ExitObserversActive=FALSE;break;
      case 8:D->InUse=FALSE;Expected=EFI_NOT_FOUND;break;
      case 9:D->Segment=1;break;
      case 10:D->Bdf=0;break;
      case 11:D->Poisoned=TRUE;break;
      case 12:D->PreservedAtExit=TRUE;break;
      case 13:D->Sid=1;Expected=EFI_NOT_FOUND;break;
    }
    UINTN N=HwWrites;
    assert(CrSmmuReleaseUsbToPhysicalAtExit(This)==Expected && HwWrites==N);
    assert(!memcmp(Hw,SavedHw,sizeof(Hw)) && !PhysicalWrites);End();
  }
}
STATIC VOID EFIAPI AddForeignAlias(VOID *Context,EFI_STATUS Status) {
  (VOID)Context;assert(Status==EFI_SUCCESS && mSmmu.ExitObserversActive);
  // A late foreign alias is not covered by the earlier successful revoke.
  // The producer must independently refuse it before maintenance or SCTLR.
  Hw[(0x800+127*4)/4]=0x80000000;Hw[(0xc00+127*4)/4]=4;
  SavedHw[(0x800+127*4)/4]=0x80000000;SavedHw[(0xc00+127*4)/4]=4;
}
int main(VOID) {
  TestProducerAdmission();
  for(UINTN Early=0;Early<2;++Early) {
    Start();UINTN N=HwWrites;assert(CrSmmuReleaseUsbToPhysicalAtExit(&mCrProtocol)==EFI_ACCESS_DENIED);
    mSmmu.ExitComplete=TRUE;assert(CrSmmuReleaseUsbToPhysicalAtExit(&mCrProtocol)==EFI_ACCESS_DENIED);
    mSmmu.ExitComplete=FALSE;assert(HwWrites==N);Exit((BOOLEAN)Early);
    assert(PhysicalWrites==1 && Hw[(mSmmu.Domains[0].Domain.BankBase-HW_BASE)/4]==0);
    CheckTerminal(TRUE);End();
  }
  Start();LegacyPolicy=FALSE;Exit(FALSE);assert(!PhysicalWrites);
  assert(CrUsbHandoffCanTakeover(Page,Locator.Generation)==CR_SUCCESS);End();
  Start();Role=CR_USBH_HOST;Exit(FALSE);assert(!PhysicalWrites);
  assert(CrUsbHandoffCanTakeover(Page,Locator.Generation)==CR_SUCCESS);End();
  for(UINTN Case=0;Case<2;++Case) {
    Start();if(Case)mCrProtocol.ReleaseUsbToPhysicalAtExit=NULL;
    else mCrProtocol.Revision=EFI_SMMU_CR_EXIT_USB_PHYSICAL_REVISION-1;
    Exit(FALSE);assert(!PhysicalWrites);
    assert(CrUsbHandoffCanTakeover(Page,Locator.Generation)==CR_SUCCESS);End();
  }
  for(StopFailure=1;StopFailure<=11;) {
    UINT32 Case=StopFailure;Start();StopFailure=Case;Exit(FALSE);
    assert(!PhysicalWrites && mSmmu.Domains[0].Domain.ExitState==SmmuDomainExitNormal);
    CheckUncertain();End();StopFailure=Case+1;
  }
  Start();RejectPhysical=TRUE;Exit(FALSE);assert(PhysicalWrites==1);CheckTerminal(FALSE);End();
  Start();TimeoutPhysical=TRUE;Exit(FALSE);assert(PhysicalWrites==1);CheckTerminal(FALSE);End();
  Start();
  // Registration selects the last free slot; this subsequent registration
  // therefore executes before the already registered USB completion callback.
  assert(CrSmmuSetExitObserver(&mCrProtocol,&OwnedTables,AddForeignAlias,NULL)==EFI_SUCCESS);
  Exit(FALSE);assert(!PhysicalWrites && mUsbHandoff.Local.RevocationStatus==EFI_UNSUPPORTED);
  assert(mSmmu.Domains[0].Domain.ExitState==SmmuDomainExitNormal);CheckUncertain();End();
  puts("Actual native DWC3/SMMU/core composition: 14 admission guards, both EBS orders, policy off, host/old-protocol skip, 11 invalid stop facts, retained write/sync failures, late foreign alias PASS");
  return 0;
}
