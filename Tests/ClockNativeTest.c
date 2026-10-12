/* Actual clock graph/core plus immutable native handoff parser. MIT */
#define CR_CLOCK_EXTERNAL_FIXTURE_ONLY
#include "ClockExternalTest.c"
#define CR_MDSS_TEST_NO_FENCE
#define main HandoffPriorRegression
#include "MdssHandoffTest.c"
#undef main
static const char *Names[12]={
 "disp_cc_mdss_byte0_clk","disp_cc_mdss_byte0_intf_clk","disp_cc_mdss_pclk0_clk",
 "disp_cc_mdss_dptx0_aux_clk","disp_cc_mdss_dptx0_link_clk","disp_cc_mdss_dptx0_link_intf_clk","disp_cc_mdss_dptx0_pixel0_clk",
 "disp_cc_mdss_byte0_clk_src","disp_cc_mdss_pclk0_clk_src","disp_cc_mdss_dptx0_aux_clk_src","disp_cc_mdss_dptx0_link_clk_src","disp_cc_mdss_dptx0_pixel0_clk_src"};
static VOID NativeRecord(VOID) {
 Build(0);
 /* Replace the general schema fixture's synthetic clocks with the actual
    two native DXE resource inventories. Retain its independently built
    live/revoked SMMU evidence and actual builder/parser. */
 const char *Shared[]={"gcc_disp_ahb_clk","gcc_disp_xo_clk","disp_cc_mdss_ahb_clk",
   "disp_cc_mdss_mdp_clk","gcc_disp_hf_axi_clk","gcc_disp_sf_axi_clk",
   "disp_cc_mdss_mdp_lut_clk","disp_cc_mdss_vsync_clk"};
 const UINT64 Rates[]={0,0,19200000,200000000,0,0,200000000,19200000};
 for(UINT32 Route=0;Route<2;++Route)for(UINT32 I=0;I<12;++I) {
   CR_MDSS_NATIVE_CLOCK *C=&R.Clocks[Route*12+I];memset(C->Name,0,48);memset(C->Controller,0,8);
   C->RateHz=0;
   const char *Name=I<8?Shared[I]:Route?Names[I-5]:I==8?"disp_cc_mdss_esc0_clk":Names[I-9];
   Text(C->Name,48,Name);Text(C->Controller,8,!strncmp(Name,"gcc",3)?"gcc":"dispcc");
   if(I<8)C->RateHz=Rates[I];else if(I==8)C->RateHz=19200000;
 }
 R.Routes[0].SourceRate=111375000;R.Routes[0].SourceSelect=2;R.Routes[0].SourceDivider=2;
 R.Routes[0].PixelSource=1;R.Routes[0].PixelDivider=2;R.Routes[0].InterfaceDivider=2;
 R.Routes[1].SourceRate=270000000;R.Routes[1].SourceSelect=1;R.Routes[1].SourceDivider=2;
 R.Routes[1].PixelSource=2;R.Routes[1].PixelDivider=2;R.Routes[1].PixelM=11;R.Routes[1].PixelN=100;
 R.Routes[1].PixelD=100;R.Routes[1].InterfaceDivider=2;Recrc();assert(CrMdssHandoffCanClaim(&R)==CR_SUCCESS);
}
static VOID Firmware(CR_CLOCK_EXTERNAL *E) {
 NativeRecord();UINTN B=E->Core->ClockControllers[CC_TYPE_DISP_CC].Address;
 for(UINT32 I=0;I<12;++I){ClockNode *N=E->NativeNode[I];assert(!strcmp(N->Name,Names[I]));
   *Reg(B+(I<7?N->EnableRegister:N->CmdRegister))=(I<7?1U:2U)|BIT(7);}
 *Reg(B+0x8138)=0x201;*Reg(B+0x80c0)=0x101;*Reg(B+0x81f0)=1;
 *Reg(B+0x81a0)=0x101;*Reg(B+0x81c0)=0x2201;
 *Reg(B+0x81c4)=11;*Reg(B+0x81c8)=(~89U)&0xffff;*Reg(B+0x81cc)=(~100U)&0xffff;
 *Reg(B+0x814c)=0xa5000001;*Reg(B+0x81b4)=0xb6000001;
}
static VOID AllOff(CR_CLOCK_EXTERNAL *E) {
 assert(E->NativeMask==0xfff&&E->NativeOff==0xfff&&E->NativeTouched==0xfff&&E->NativeComplete);
 for(UINT32 I=0;I<12;++I){ClockNode *N=E->NativeNode[I];UINT32 V=*Reg(N->ParentController->Address+(I<7?N->EnableRegister:N->CmdRegister));
   assert(!(V&(I<7?1U:2U))&&(V&BIT(31))&&(V&BIT(7)));
   assert(!N->ReferenceCount&&!N->ParentsHeld&&!N->ReleasePending&&!N->RestoreControlEnabled);}
}
#ifndef CR_CLOCK_NATIVE_FIXTURE_ONLY
int main(void) {
 CR_CLOCK_EXTERNAL E;int OwnerToken,Foreign;VOID *Storage=ExternalFixture(&E);
 assert(CrClockExternalReserve(&E,&OwnerToken)==CR_SUCCESS);Firmware(&E);UINTN W=Writes;
 assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_NOT_READY&&Writes==W&&!E.NativeOwner);
 Access(&E,TRUE);
 /* Generic disable preserves inherited reference-zero state, demonstrating
    why an explicit retirement is needed before any new source programming. */
 assert(ClockEnable(E.Core,E.Leaf[0],0,FALSE)==CR_SUCCESS&&(*Reg(E.Leaf[0]->ParentController->Address+0x8028)&1));
 assert(CrClockExternalSet(&E,&OwnerToken,148500000,111375000,TRUE)==CR_NOT_READY);
 assert(CrClockExternalNativeRetire(&E,&Foreign,R.TransferId,&R)==CR_BUSY&&Writes==W);
 R.Crc32^=1;assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_COMPROMISED_DATA&&Writes==W);R.Crc32^=1;
 assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId+1,&R)==CR_INVALID_PARAMETER&&Writes==W);
 R.Clocks[0].Name[0]='x';Recrc();assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_UNSUPPORTED&&Writes==W&&!E.NativeOwner);R.Clocks[0].Name[0]='g';Recrc();
 E.NativeNode[11]->ReferenceCount=1;assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_BUSY&&Writes==W);E.NativeNode[11]->ReferenceCount=0;
 UINTN B=E.Core->ClockControllers[CC_TYPE_DISP_CC].Address;
 *Reg(B+0x81c8)^=1;assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_NOT_READY&&Writes==W&&!E.NativeOwner);*Reg(B+0x81c8)^=1;
 assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_SUCCESS);AllOff(&E);
 assert(CrClockExternalSet(&E,&OwnerToken,148500000,111375000,TRUE)==CR_SUCCESS);W=Writes;
 assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_SUCCESS&&Writes==W&&E.Enabled);
 Dispose(&E,Storage,&OwnerToken);
 for(UINT32 Failure=0;Failure<12;++Failure) {
   Storage=ExternalFixture(&E);assert(CrClockExternalReserve(&E,&OwnerToken)==CR_SUCCESS);Access(&E,TRUE);Firmware(&E);
   ClockNode *N=E.NativeNode[Failure];BlockDisable=N->ParentController->Address+(Failure<7?N->EnableRegister:N->CmdRegister);
   assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_TIMEOUT);
   assert(E.NativeOwner==&OwnerToken&&!E.NativeComplete&&(E.NativeTouched&BIT(Failure))&&!(E.NativeOff&BIT(Failure)));
   W=Writes;assert(CrClockExternalReleaseOwner(&E,&OwnerToken)==CR_NOT_READY&&E.Owner==&OwnerToken&&Writes==W);
   assert(CrClockExternalGuard(&E,E.NativeNode[6])==CR_BUSY&&CrClockExternalGuard(&E,E.Access[0])==CR_BUSY);
   BOOLEAN On,Pending;assert(CrClockExternalGet(&E,&OwnerToken,&On,&Pending)==CR_SUCCESS&&!On&&Pending);
   R.Generation++;Recrc();assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_INVALID_PARAMETER&&Writes==W);R=E.NativeRecord;
   Access(&E,FALSE);assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_NOT_READY&&Writes==W);Access(&E,TRUE);
   BlockDisable=0;assert(CrClockExternalNativeRetire(&E,&OwnerToken,R.TransferId,&R)==CR_SUCCESS);AllOff(&E);Dispose(&E,Storage,&OwnerToken);
 }
 puts("Native display clocks: real inherited ON/ref0, immutable transfer, seven leaves/five RCGs, all partial-stop retries, no fabricated references and post-transfer no-op PASS");return 0;
}

#endif
