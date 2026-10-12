/* Native frontend parity oracle. Use --legacy-source before extraction.
   SPDX-License-Identifier: MIT */
#define main ExistingPhyTestMain
#ifndef CR_USB_DP_PHY_LEGACY_TEST
#define CR_USB_DP_PHY_LEGACY_TEST "UsbDpPhyTest.c"
#endif
#include CR_USB_DP_PHY_LEGACY_TEST
#undef main
STATIC UINT64 Hash(VOID){UINT64 H=1469598103934665603ULL;
 for(UINT32 I=0;I<0x2b00/4;++I){H^=mRegisters[I];H*=1099511628211ULL;}
 for(UINT32 I=0;I<0xa4/4;++I){H^=mHsRegisters[I];H*=1099511628211ULL;}
 for(UINT32 I=0;I<32;++I){H^=mFsaRegs[I];H*=1099511628211ULL;}return H;}
int main(VOID){
 for(UINT32 Reverse=0;Reverse<2;++Reverse)for(UINT32 Four=0;Four<2;++Four)for(UINT32 Ss=0;Ss<2;++Ss)
 for(UINT32 First=0;First<2;++First)for(UINT32 Rate=1620;Rate<=8100;Rate=Rate==1620?2700:Rate==2700?5400:Rate==5400?8100:8101){
  Clear();SeedHsTune();EFI_USB_TYPEC_STATE S=State(TRUE,(BOOLEAN)Reverse);BOOLEAN Owned;
  S.DpLanes=Four?4:2;S.PinAssignment=Four?3:4;
  for(UINT32 I=0;I<2;++I){EFI_USB_DP_PHY_CLIENT C=(EFI_USB_DP_PHY_CLIENT)(First^I);
   assert(Acquire(&mProtocol,C,&S,(BOOLEAN)(C?0:Ss),&Owned)==EFI_SUCCESS&&Owned);}
  assert(PowerDp(&mProtocol,Rate,Four?4:2)==EFI_SUCCESS);
  assert(Levels(&mProtocol,1,1)==EFI_SUCCESS);
  printf("%u %u %u %u %u %016llx\n",Reverse,Four,Ss,First,Rate,(unsigned long long)Hash());
  assert(Release(&mProtocol,UsbDpPhyUsb)==EFI_SUCCESS);assert(Release(&mProtocol,UsbDpPhyDp)==EFI_SUCCESS);
 }
 return 0;
}
