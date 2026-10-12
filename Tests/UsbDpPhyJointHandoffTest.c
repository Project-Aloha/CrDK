/* Actual USBH/MDSH builders and combo-PHY policy, independent joint boundary.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main ExistingPhyPolicyTest
#include "UsbDpPhyPortableTest.c"
#undef main
#define CR_MDSS_TEST_NO_FENCE
#define main ExistingMdssBuilderTest
#include "MdssHandoffTest.c"
#undef main
#include <Library/UsbDpPhyHandoffCrLib.h>

static unsigned Cases;
static CR_USB_NATIVE_HANDOFF Usb, UsbSaved;
static CR_MDSS_NATIVE_HANDOFF MdssSaved;
static CR_USB_DP_PHY_NATIVE_DEMANDS Demands, Sentinel;
static const char *RecordDirectory;
static void Emit(const char *UsbName,const char *MdssName){
  if(!RecordDirectory)return;
  assert(CrUsbHandoffCanTakeover(&Usb,19)==CR_SUCCESS);
  assert(CrMdssHandoffCanClaim(&R)==CR_SUCCESS);
  char Path[4096];
  assert(snprintf(Path,sizeof(Path),"%s/%s",RecordDirectory,UsbName)>0);
  FILE *F=fopen(Path,"wb");assert(F);assert(fwrite(&Usb,1,sizeof(Usb),F)==sizeof(Usb));assert(fclose(F)==0);
  assert(snprintf(Path,sizeof(Path),"%s/%s",RecordDirectory,MdssName)>0);
  F=fopen(Path,"wb");assert(F);assert(fwrite(&R,1,sizeof(R),F)==sizeof(R));assert(fclose(F)==0);
}
static CR_USB_NATIVE_HANDOFF UsbRecord(UINT32 Role) {
  CR_USB_NATIVE_HANDOFF U = {0};
  U.Signature=CR_USBH_SIGNATURE; U.Revision=CR_USBH_REVISION; U.Length=sizeof(U);
  U.BootGeneration=19; U.TransferId=29; U.Soc=0x8450; U.Board=1;
  U.Controller=CR_USBH_CONTROLLER; U.ControllerBytes=CR_USBH_CONTROLLER_BYTES;
  U.Qscratch=CR_USBH_QSCRATCH; U.QscratchBytes=CR_USBH_QSCRATCH_BYTES;
  U.ExpectedWriters=U.FrozenWriters=U.DrainedWriters=CR_USBH_ALL_WRITERS;
  U.State=CR_USBH_RETIRED;
  U.Stop.Role=Role; U.Stop.Id=0x33310000; U.Stop.Revision=0x3139302a;
  U.Stop.GlobalControl=Role<<12; U.Stop.EventBuffers=1;
  if(Role==CR_USBH_HOST) U.Stop.HostStatus=1;
  else {U.Stop.DeviceStatus=1U<<22;U.Stop.MaskedEventBuffers=1;}
  SmmuMappingEvidence *E=&U.Revoked;
  E->Base=0x15000000;E->BankBase=0x15041000;E->Bank=1;E->Stream=3;E->Asid=5;
  E->Tables=0x83000000;E->TableBytes=34*4096;E->Iova=0x10000000;E->Bytes=0x04000000;
  E->Smr=0x80000000;E->S2cr=1;E->Sctlr=0x1027;E->Cbar=0x1f300;
  E->Cba2r=1;E->Tcr=0x803500|28;E->Tcr2=0x38012;
  E->Ttbr0=E->Tables|((UINT64)5<<48);E->Ttbr1=(UINT64)5<<48;E->Mair0=0x44;
  assert(CrUsbHandoffSeal(&U,2)==CR_SUCCESS);
  return U;
}
static void Qualified(void) {
  Build(0); /* Existing actual MDSH builder, DSI live / DP stopped+revoked. */
  New(); CR_USB_DP_PHY_ROUTE RouteValue=Route(FALSE,FALSE,FALSE); BOOLEAN Owned=FALSE;
  assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&RouteValue,FALSE,&Owned)==CR_SUCCESS&&Owned);
  assert(CrUsbDpPhyFreeze(&Phy)==CR_SUCCESS);
  /* Snapshot known successful source journals; expected names/rates come
     from the actual core, never the qualifier's private policy arrays. */
  for(UINT32 I=0;I<4;++I){CR_MDSS_NATIVE_CLOCK *C=&R.Clocks[24+I];
    assert(Phy.Clocks[I].Owned&&Phy.Clocks[I].Status==CR_SUCCESS);
    memset(C,0,sizeof(*C));C->Owner=CR_MDSS_NATIVE_PHY;C->Flags=CR_MDSS_NATIVE_OWNED;
    Text(C->Controller,sizeof(C->Controller),"gcc");Text(C->Name,sizeof(C->Name),Phy.Clocks[I].Name);C->RateHz=Phy.Clocks[I].Rate;
  }
  for(UINT32 I=0;I<6;++I){CR_MDSS_NATIVE_RAIL *V=&R.Rails[2+I];
    assert(Phy.Rails[I].Owned&&Phy.Rails[I].Status==CR_SUCCESS);
    memset(V,0,sizeof(*V));V->Owner=CR_MDSS_NATIVE_PHY;V->Flags=CR_MDSS_NATIVE_OWNED;
    Text(V->Name,sizeof(V->Name),Phy.Rails[I].Name);V->Millivolts=Phy.Rails[I].Millivolts;V->Mode=Phy.Rails[I].Mode;
  }
  Recrc();assert(CrMdssHandoffCanClaim(&R)==CR_SUCCESS);
  Usb=UsbRecord(CR_USBH_HOST); UsbSaved=Usb; MdssSaved=R;
}
static void Decline(void) {
  UINT32 BeforeCalls=Calls; Demands=Sentinel;++Cases;
  assert(CrUsbDpPhyHandoffQualify(&Usb,19,&R,&Demands)!=CR_SUCCESS);
  assert(Calls==BeforeCalls);
  assert(!memcmp(&Demands,&Sentinel,sizeof(Demands)));
  Usb=UsbSaved;R=MdssSaved;
}
static void DeclineMdss(void){Recrc();Decline();}
static void Success(UINT32 ClockMask,UINT32 RailMask) {
  CR_USB_NATIVE_HANDOFF U=Usb; CR_MDSS_NATIVE_HANDOFF M=R;
  UINT32 BeforeCalls=Calls;
  Demands=Sentinel; ++Cases;
  assert(CrUsbDpPhyHandoffQualify(&Usb,19,&R,&Demands)==CR_SUCCESS);
  assert(Calls==BeforeCalls);
  assert(!memcmp(&U,&Usb,sizeof(U))&&!memcmp(&M,&R,sizeof(M)));
  assert(Demands.UsbBootGeneration==19&&Demands.UsbTransferId==29);
  assert(Demands.MdssGeneration==13&&Demands.MdssTransferId==19);
  assert(Demands.ClockMask==ClockMask&&Demands.RailMask==RailMask);
  for(UINT32 I=0;I<4;++I){CR_MDSS_NATIVE_CLOCK Zero={0};
    if(ClockMask&(1U<<I)){assert(!strcmp(Demands.Clocks[I].Name,Phy.Clocks[I].Name));assert(Demands.Clocks[I].RateHz==Phy.Clocks[I].Rate);}
    else assert(!memcmp(&Demands.Clocks[I],&Zero,sizeof(Zero)));
  }
  for(UINT32 I=0;I<6;++I){CR_MDSS_NATIVE_RAIL Zero={0};
    if(RailMask&(1U<<I)){assert(!strcmp(Demands.Rails[I].Name,Phy.Rails[I].Name));assert(Demands.Rails[I].Millivolts==Phy.Rails[I].Millivolts&&Demands.Rails[I].Mode==Phy.Rails[I].Mode);}
    else assert(!memcmp(&Demands.Rails[I],&Zero,sizeof(Zero)));
  }
}
static void RemoveClock(UINT32 I){memmove(&R.Clocks[I],&R.Clocks[I+1],(R.ClockCount-I-1)*sizeof(R.Clocks[0]));memset(&R.Clocks[--R.ClockCount],0,sizeof(R.Clocks[0]));}
static void RemoveRail(UINT32 I){memmove(&R.Rails[I],&R.Rails[I+1],(R.RailCount-I-1)*sizeof(R.Rails[0]));memset(&R.Rails[--R.RailCount],0,sizeof(R.Rails[0]));}
int main(int Argc,char **Argv) {
  (void)ExistingPhyPolicyTest;(void)ExistingMdssBuilderTest;
  assert(Argc==1||Argc==2);if(Argc==2)RecordDirectory=Argv[1];
  memset(&Sentinel,0xa7,sizeof(Sentinel));Qualified();Success(15,63);Emit("usb-native.bin","mdss-native.bin");
  /* Host/function independently retired USB writers; producer generation
     equality is deliberately neither required nor expected. */
  Usb=UsbRecord(CR_USBH_FUNCTION);Success(15,63);Usb=UsbSaved;
  /* Valid diagnostic CRC/schema with missing hardware retirement facts
     must still decline. Re-sealing UNCERTAIN is no transfer substitute. */
#define USB_UNCERTAIN(STATEMENT) do {Usb.State=CR_USBH_UNCERTAIN;STATEMENT;\
  assert(CrUsbHandoffSeal(&Usb,4)==CR_SUCCESS);assert(CrUsbHandoffValidate(&Usb)==CR_SUCCESS);Decline();} while(0)
  USB_UNCERTAIN(Usb.ExpectedWriters=Usb.FrozenWriters=Usb.DrainedWriters=3);
  USB_UNCERTAIN(Usb.DrainedWriters=3);USB_UNCERTAIN(Usb.FailedWriters=1);
  for(UINT32 I=0;I<3;++I){USB_UNCERTAIN(Usb.ActiveCalls[I]=1);}
  USB_UNCERTAIN(Usb.FreezeStatus=1);USB_UNCERTAIN(Usb.HaltStatus=1);
  USB_UNCERTAIN(Usb.InterruptStatus=1);USB_UNCERTAIN(Usb.RevocationStatus=1);
  USB_UNCERTAIN(Usb.Stop.HostCommand=1);USB_UNCERTAIN(Usb.Stop.HostCommand=4);
  USB_UNCERTAIN(Usb.Stop.HostStatus=0);USB_UNCERTAIN(Usb.Revoked.Access=1);
  USB_UNCERTAIN(Usb.Revoked.Physical=0x83000000);USB_UNCERTAIN(Usb.Revoked.Sid=1);
#undef USB_UNCERTAIN
  for(UINT32 I=0;I<4;++I){R.Clocks[24+I].RateHz++;DeclineMdss();
    R.Clocks[24+I].Controller[0]='x';DeclineMdss();R.Clocks[24+I].Name[0]='x';DeclineMdss();
    R.Clocks[24+I].Flags|=CR_MDSS_NATIVE_RETAINED_ERROR;DeclineMdss();RemoveClock(24+I);DeclineMdss();}
  for(UINT32 I=0;I<6;++I){R.Rails[2+I].Millivolts++;DeclineMdss();R.Rails[2+I].Mode++;DeclineMdss();
    R.Rails[2+I].Name[0]='x';DeclineMdss();R.Rails[2+I].Flags|=CR_MDSS_NATIVE_RETAINED_ERROR;DeclineMdss();RemoveRail(2+I);DeclineMdss();}
  R.Phy.State=CR_MDSS_NATIVE_UNCERTAIN;R.Phy.Flags|=CR_MDSS_NATIVE_RETAINED_ERROR;R.Phy.Status=0x8000000000000007ULL;DeclineMdss();
  R.Phy.State=CR_MDSS_NATIVE_STOPPED;DeclineMdss();R.Phy.Base+=4;DeclineMdss();R.Phy.Bytes=0x3000;DeclineMdss();
  R.Phy.UsbOwners=0;DeclineMdss();R.Phy.ReadyMask&=~CR_MDSS_NATIVE_PHY_USB;DeclineMdss();
  R.Phy.ReadyMask|=CR_MDSS_NATIVE_PHY_DP;DeclineMdss();R.Phy.TouchedMask&=~CR_MDSS_NATIVE_PHY_FSA;DeclineMdss();
  R.Phy.ReadyMask&=~CR_MDSS_NATIVE_PHY_FSA;DeclineMdss();R.Phy.TouchedMask|=CR_MDSS_NATIVE_PHY_DP;DeclineMdss();
  R.RegisteredOwners&=~CR_MDSS_NATIVE_PHY;DeclineMdss();R.FrozenOwners&=~CR_MDSS_NATIVE_PHY;DeclineMdss();R.Phase=CR_MDSS_NATIVE_PENDING;DeclineMdss();
  R.Routes[1].State=CR_MDSS_NATIVE_LIVE;R.Routes[1].Flags=CR_MDSS_NATIVE_OWNED;R.Routes[1].MappingState=CR_MDSS_NATIVE_MAPPING_PRESERVED;
  for(UINT32 I=0;I<4;++I){R.Routes[1].Streams[I].Physical=R.Routes[1].FrameBuffer;R.Routes[1].Streams[I].Access=SMMU_ACCESS_READ;}
  Recrc();assert(CrMdssHandoffCanClaim(&R)==CR_SUCCESS);Decline();
  R.Routes[1].MappingState=CR_MDSS_NATIVE_MAPPING_NONE;R.Routes[1].MappingId=0;R.Routes[1].StreamCount=0;R.Routes[1].DeviceAddress=0;memset(R.Routes[1].Streams,0,sizeof(R.Routes[1].Streams));
  Recrc();assert(CrMdssHandoffCanClaim(&R)==CR_SUCCESS);Decline();
  /* Committed native demands can be interleaved in the record: output order
     remains fixed and independent from append order. */
  CR_MDSS_NATIVE_CLOCK C=R.Clocks[24];R.Clocks[24]=R.Clocks[27];R.Clocks[27]=C;
  CR_MDSS_NATIVE_RAIL V=R.Rails[2];R.Rails[2]=R.Rails[7];R.Rails[7]=V;Recrc();Success(15,63);R=MdssSaved;
  /* Absent route is an actual complete SMMU NONE snapshot, not a missing
     producer. Owned PHY may still have a frozen DP client while output is off. */
  memset(&R.Routes[1],0,sizeof(R.Routes[1]));R.Routes[1].Owner=CR_MDSS_NATIVE_DP;
  R.Routes[1].MappingOwner=CR_MDSS_NATIVE_SMMU;R.Routes[1].MappingState=CR_MDSS_NATIVE_MAPPING_NONE;
  for(UINT32 I=12;I<24;++I)memset(&R.Clocks[I],0,sizeof(R.Clocks[I]));
  memmove(&R.Clocks[12],&R.Clocks[24],4*sizeof(R.Clocks[0]));memset(&R.Clocks[16],0,16*sizeof(R.Clocks[0]));R.ClockCount=16;
  memset(&R.Paths[1],0,sizeof(R.Paths[1]));R.PathCount=1;Recrc();Success(15,63);R=MdssSaved;
  R.Phy.UsbOwners=0;R.Phy.DpOwners=1;R.Phy.ReadyMask=R.Phy.TouchedMask=CR_MDSS_NATIVE_PHY_COMMON|CR_MDSS_NATIVE_PHY_DP|CR_MDSS_NATIVE_PHY_FSA;
  memset(&R.Rails[5],0,3*sizeof(R.Rails[0]));R.RailCount=5;Recrc();Success(15,7);Emit("usb-native-dp-only.bin","mdss-native-dp-only.bin");R=MdssSaved;
  R.Phy.DpOwners=1;R.Phy.ReadyMask|=CR_MDSS_NATIVE_PHY_DP;R.Phy.TouchedMask|=CR_MDSS_NATIVE_PHY_DP;Recrc();Success(15,63);R=MdssSaved;
  R.Phy.ReadyMask|=CR_MDSS_NATIVE_PHY_USB_SS;Recrc();Success(15,63);R=MdssSaved;
  memset(&R.Phy,0,sizeof(R.Phy));memset(&R.Clocks[24],0,4*sizeof(R.Clocks[0]));R.ClockCount=24;
  memset(&R.Rails[2],0,6*sizeof(R.Rails[0]));R.RailCount=2;Recrc();Success(0,0);Emit("usb-native-absent.bin","mdss-native-absent.bin");R=MdssSaved;
  for(UINTN I=0;I<sizeof(Usb);++I){((UINT8*)&Usb)[I]^=1;Decline();}
  for(UINTN I=0;I<sizeof(R);++I){((UINT8*)&R)[I]^=1;Decline();}
  Demands=Sentinel;assert(CrUsbDpPhyHandoffQualify(&Usb,18,&R,&Demands)==CR_NOT_READY&&!memcmp(&Demands,&Sentinel,sizeof(Demands)));
  assert(CrUsbDpPhyHandoffQualify(NULL,19,&R,&Demands)==CR_INVALID_PARAMETER);
  assert(CrUsbDpPhyHandoffQualify(&Usb,19,NULL,&Demands)==CR_INVALID_PARAMETER);
  assert(CrUsbDpPhyHandoffQualify(&Usb,19,&R,NULL)==CR_INVALID_PARAMETER);
  CR_MDSS_NATIVE_HANDOFF Before=R;
  assert(CrUsbDpPhyHandoffQualify(&Usb,19,&R,(VOID*)&R)==CR_INVALID_PARAMETER&&!memcmp(&Before,&R,sizeof(R)));
  printf("PASS joint native USB/MDSS PHY qualification: %u cases, actual core journals, independent generations, DP retirement and unchanged failure output\n",Cases);
  return 0;
}
