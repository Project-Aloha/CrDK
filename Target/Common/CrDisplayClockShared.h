/* Shared selected-target adapter; portable algorithms remain ClockLib. MIT */
STATIC CR_STATUS CheckDisplayPll(ClockNode *Node,UINT64 RateHz) {
  if(Node!=&disp_cc_pll1 || Node->ParentController!=&mDisp ||
      !mInitialized || !mContext.LockInitialized)return CR_INVALID_PARAMETER;
  if(!mDisp.MemMapped || !mDisp.Address || (mDisp.Address&3U) || mDisp.Size<0x1044 ||
      mDisp.Address>(UINTN)-1-mDisp.Size)return CR_NOT_READY;
  CONST CR_INHERITED_PLL_DESCRIPTION D={.Base=mDisp.Address+0x1000,.Size=mDisp.Size-0x1000,
    .ExpectedL=31,.ExpectedAlpha=0x4000,.ParentHz=19200000,.FrequencyHz=600000000,
    .FsmInModeRegister=CR_DISPLAY_PLL_FSM_IN_MODE};
  return CrInheritedPllCheck(&D,RateHz);
}
STATIC CR_STATUS PrepareDisplaySource(ClockNode *Node,CONST ClockRcgFreqTable *Freq) {
  if(Node->ParentController!=&mDisp)return CR_INVALID_PARAMETER;
  if(Node==&disp_cc_mdss_mdp_clk_src) {
    if(Freq->FrequencyHz!=200000000 || Freq->Source.Config!=4 || Freq->PreDiv!=5 ||
        !disp_cc_pll1.ReferenceCount || disp_cc_pll1.ReleasePending)return CR_NOT_READY;
    return CheckDisplayPll(&disp_cc_pll1,600000000);
  }
  if(Node!=&disp_cc_mdss_ahb_clk_src && Node!=&disp_cc_mdss_esc0_clk_src &&
      Node!=&disp_cc_mdss_esc1_clk_src && Node!=&disp_cc_mdss_vsync_clk_src)return CR_UNSUPPORTED;
  return Freq->FrequencyHz==19200000 && Freq->Source.Config==0 && Freq->PreDiv==1?
    CR_SUCCESS:CR_UNSUPPORTED;
}
STATIC CR_STATUS ResetDisplayCore(CONST CHAR8 *Id,BOOLEAN Assert) {
  if(cr_strcmp(Id,"disp_cc_mdss_core_bcr"))return CR_NOT_FOUND;
  if(!mInitialized || !mContext.LockInitialized || !mDisp.MemMapped ||
      !mDisp.Address || (mDisp.Address&3U) || mDisp.Size<0x2004 ||
      mDisp.Address>(UINTN)-1-mDisp.Size)return CR_NOT_READY;
  UINTN Address=mDisp.Address+0x2000;CrLockAcquire(&mContext.Lock);
  CrMmioUpdateBits32(Address,BIT(0),Assert?BIT(0):0);CrMemoryFence();
  CR_STATUS Status=(CrMmioRead32(Address)&BIT(0))==(Assert?BIT(0):0)?CR_SUCCESS:CR_DEVICE_ERROR;
  CrLockRelease(&mContext.Lock);return Status;
}
STATIC CR_STATUS ResolveDisplayResource(CONST CHAR8 *Id,CR_DAL_CLOCK_RESOURCE_KIND Kind,
    CONST CHAR8 **NativeId,UINT32 *Flags) {
  if(Kind==CrDalClockResourceReset) {
    if(cr_strcmp(Id,"disp_cc_mdss_core_bcr"))return CR_NOT_FOUND;
    *NativeId=disp_cc_mdss_ahb_clk.Name;return CR_SUCCESS;
  }
  ClockNode **Nodes=Kind==CrDalClockResourcePowerDomain?mDisplayGdscs:mDisplayClocks;
  UINTN Count=Kind==CrDalClockResourcePowerDomain?ARRAY_SIZE(mDisplayGdscs):ARRAY_SIZE(mDisplayClocks);
  for(UINTN I=0;I<Count;++I)if(!cr_strcmp(Id,Nodes[I]->Name)) {
    *NativeId=Nodes[I]->Name;
    if(Nodes[I]==&disp_cc_mdss_byte0_clk || Nodes[I]==&disp_cc_mdss_byte1_clk ||
       Nodes[I]==&disp_cc_mdss_byte0_intf_clk || Nodes[I]==&disp_cc_mdss_byte1_intf_clk ||
       Nodes[I]==&disp_cc_mdss_pclk0_clk || Nodes[I]==&disp_cc_mdss_pclk1_clk)
      *Flags=CR_DAL_CLOCK_RESOURCE_NO_RATE | CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE;
    return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}
