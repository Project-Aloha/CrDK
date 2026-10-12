/* Actual UCSI driver with source-selected HDK8350 resources. MIT */
#define UCSI_TEST_SM8350
#define main LegacyUcsiTestMain
#include "UcsiDxeLifecycleTest.c"
#undef main
int main(void) {
  Fixture();assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  assert(Installed&&ClientRegistered&&!SupplyPowered&&!SupplyAcquires&&OrientationOwned);
  Kick(NULL);assert(mIpcc.Registered);
  EFI_USB_TYPEC_STATE State;assert(GetState(&mProtocol,&State)==EFI_SUCCESS&&!State.PdReady);
  PilRunning=FALSE;Watchdog();assert(!mPeerReady&&!mPmic.State.PdReady);
  assert(SetDataRole(&mProtocol,UsbTypeCHost)!=EFI_SUCCESS);
  PilRunning=TRUE;NextReady=TRUE;Tick();assert(mPeerReady);
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS&&!OrientationOwned&&!ClientRegistered&&!SupplyReleases);
  Fixture();OrientationAcquireStatus=EFI_ACCESS_DENIED;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_ACCESS_DENIED&&!Installed&&!OrientationOwned&&!ClientRegistered);
  Fixture();assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  OrientationReleaseStatus=EFI_DEVICE_ERROR;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_DEVICE_ERROR&&OrientationOwned&&ClientRegistered);
  OrientationReleaseStatus=EFI_SUCCESS;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS&&!OrientationOwned&&!ClientRegistered);
  for(UINT32 At=1;At<=5;++At){
    if(At==4)continue; /* SM8350 does not acquire a local PD supply. */
    Fixture();ExitInject=At;
    assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
    assert(mStopped&&Arms==0&&!mPeerReady&&!mPmic.State.PdReady);
    if(At<3)assert(IrqStarts==0);
    if(At<5)assert(!Installed);
    UINT32 L=LocateCalls;
    assert(GetState(&mProtocol,&State)==EFI_NOT_READY&&LocateCalls==L);
    assert(SetDataRole(&mProtocol,UsbTypeCHost)==EFI_NOT_READY);
    WorkCallback((EFI_EVENT)3,NULL);assert(!Polls&&!Arms);
    assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)!=EFI_SUCCESS);
  }
  Fixture();OrientationReturnsOwned=FALSE;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  assert(!Installed&&!OrientationOwned&&ClientRegistered&&mStopped);
  assert(UCSICrUnload((EFI_HANDLE)0x1234)!=EFI_SUCCESS&&ClientRegistered);
  Fixture();assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);CloseFailAt=Closes+1;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_DEVICE_ERROR&&mExit!=NULL&&ExitOpen);
  CloseFailAt=0;assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS&&!ExitOpen&&!WorkOpen);
  puts("HDK8350 actual UCSI lifecycle: IPCC408000/GPIO81 lease, real PIL readiness gate, no guessed PD rail, denied-role and failed-release retention PASS");return 0;
}
