/* Real UCSI, GPIO lease and IPCC startup with EBS at OSKAL registration. MIT */
#define UCSI_REAL_IPCC
#define UCSI_TEST_SM8350
#define main ModelUcsi8350Main
#include "UcsiDxeLifecycleTest.c"
#undef main
int main(void){
 Fixture();assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS&&IpccEnables==1);
 assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS&&!IrqRegistered);
 Fixture();FireOnEnable=TRUE;
 assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
 assert(mIpcc.Error==EFI_SUCCESS&&mIpcc.Pending&&mIpcc.Received==1&&!mStopped);
 Watchdog();assert(!mStopped&&mIpcc.Error==EFI_SUCCESS);
 assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS);
 Fixture();ExitInject=3;
 assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
 assert(mExiting&&mStopped&&!Installed&&!Arms&&IrqStarts==1&&IrqStops==1&&!IrqRegistered);
 assert(!PostExitEnables&&!IpccEnables&&!mIpcc.Registered&&!mIpcc.SignalOwned);
 EFI_USB_TYPEC_STATE S;UINT32 L=LocateCalls;
 assert(GetState(&mProtocol,&S)==EFI_NOT_READY&&LocateCalls==L);
 puts("Actual UCSI + native GPIO lease + actual IPCC: EBS during OSKAL registration freezes before signal enable and safely drains retained IRQ PASS");return 0;
}
