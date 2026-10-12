/* Exercise every real PCIe consumer through its real clock provider. MIT */
static void HdkPcieClocks(void) {
  CONST PcieTargetContext *P=CrTargetGetPcieContext();
  SourceState();ClearTrace();
  for(UINTN I=0;I<P->ClockCount;++I) {
    CONST PcieTargetClock *C=&P->Clocks[I];
    CONST CHAR8 *Id=NULL;UINT32 Flags=0;
    assert(CrTargetClockResolveResource(C->Controller,C->Id,CrDalClockResourceClock,&Id,&Flags)==CR_SUCCESS);
    ClockNode *N=Node(Id);
    assert(ClockEnable(&mContext,N,C->RateHz,TRUE)==CR_SUCCESS);
    if(!strcmp(C->Name,"pipe") && N->ParentCount) {
      ClockNode *Mux=N->Parents[0];
      assert(Mux->Type==CLOCK_NODE_TYPE_PHY_MUX);
      assert((*Reg(BASE+Mux->MuxRegister)&3)==0);
    }
  }
  /* RC0's AUX source may feed another RC's PHY: it must survive RC0 release. */
  for(UINTN I=0;I<P->ClockCount;++I) {
    CONST PcieTargetClock *C=&P->Clocks[I];
    assert(ClockEnable(&mContext,Node(C->Id),C->RateHz,FALSE)==CR_SUCCESS);
  }
  for(UINTN I=0;I<mGcc.ClkCount;++I)if(mGcc.Clks[I])assert(!mGcc.Clks[I]->ReferenceCount);
  for(UINTN I=0;I<P->ResetCount;++I) {
    CONST PcieTargetReset *R=&P->Resets[I];
    assert(CrTargetClockReset(R->Controller,R->Id,TRUE)==CR_SUCCESS);
    assert(CrTargetClockReset(R->Controller,R->Id,FALSE)==CR_SUCCESS);
  }
  puts("All HDK PCIe controller/PHY clock requests, shared sources, pipe muxes and reset routes passed");
}
