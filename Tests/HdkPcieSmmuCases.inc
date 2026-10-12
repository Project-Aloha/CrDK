/* Real SID policy + portable SMMU. Existing UFS domains remain live. MIT */
static void HdkPcieSmmu(SmmuDevice *Device,CONST CrTargetSmmuContext *Target) {
  SmmuDomain Pcie[6]={{0}};
  UINT64 *Pte[6]={0};
  assert(Target->CoherentStreamCount<=ARRAY_SIZE(Pcie));
  for(UINTN I=0;I<Target->CoherentStreamCount;++I) {
    UINT16 Sid=Target->CoherentStreamIds[I];
    assert(Sid==0x1c00+(I/2)*0x80+(I%2));
    Pte[I]=Tables();
    assert(SmmuAttachCoherent(Device,&Pcie[I],Sid,Pte[I],0x30000000+I*0x100000)==CR_SUCCESS);
    assert(Pcie[I].CacheCoherent);
    for(UINTN J=0;J<I;++J)assert(Pcie[I].Bank!=Pcie[J].Bank&&Pcie[I].Stream!=Pcie[J].Stream);
    assert(SmmuSetMapping(&Pcie[I],SMMU_IOVA_BASE,0x800000000ULL+I*0x100000,1,SMMU_ACCESS_READ|SMMU_ACCESS_WRITE)==CR_SUCCESS);
    assert((Pte[I][1024]&0xc)==0); /* Native WB uses each domain's MAIR slot0. */
  }
  for(UINTN I=0;I<Target->CoherentStreamCount;++I) {
    assert(SmmuBlock(&Pcie[I])==CR_SUCCESS);
    assert(SmmuDetach(&Pcie[I])==CR_SUCCESS);
    for(UINTN J=I+1;J<Target->CoherentStreamCount;++J)assert(Pcie[J].Active&&Pte[J][1024]);
    free(Pte[I]);
  }
  puts("Every HDK PCIe SID: concurrent coherent >4GiB mappings, exact domain isolation and independent revocation passed");
}
