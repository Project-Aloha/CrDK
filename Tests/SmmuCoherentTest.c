/* SPDX-License-Identifier: MIT */
#define main OriginalSmmuRegressionSuite
#include "SmmuLibTest.c"
#undef main
#include "AndroidSctlrOracle.h"
static SmmuDevice Device;
static SmmuDomain Cached, Plain;
static UINT64 *CachedTables, *PlainTables;
static UINT32 IgnoreMair;
static unsigned CoherentEnable;
#ifndef EXPECT_OLD_ATTACH
static VOID CoherentWrite(VOID *Cookie, UINT64 Address, UINT32 Value) {
  if(Address==BASE+CB(2)+0x38) Value &= ~IgnoreMair;
  if(Address==BASE+CB(2) && (Value&1)) {
    assert(Registers[(CB(2)+0x38)/4]==0xff);
    CoherentEnable++;
  }
  Write32(Cookie,Address,Value);
}
#endif
static VOID Start(void) {
  Init(&Device,&Cached,&Plain);
  assert(SmmuProbe(&Device,BASE,sizeof(Registers),&Ops)==CR_SUCCESS);
  CachedTables=Tables();PlainTables=Tables();IgnoreMair=CoherentEnable=0;
}
#ifndef EXPECT_OLD_ATTACH
static VOID Isolation(void) {
  Start();Device.Io.Write32=CoherentWrite;
  assert(SmmuAttachCoherent(&Device,&Cached,0xe0,CachedTables,0xa77173000ULL)==CR_SUCCESS);
  assert(Cached.CacheCoherent && CoherentEnable==1);
  assert(SmmuAttach(&Device,&Plain,0,PlainTables,0xa88173000ULL)==CR_SUCCESS);
  assert(!Plain.CacheCoherent && Plain.Bank!=Cached.Bank);
  UINT32 BeforeGlobal=Registers[0],BeforeSmr=Registers[SMR(Plain.Stream)/4];
  assert(Registers[(CB(Plain.Bank)+0x38)/4]==0x44);
  const UINT64 Pa=0xa76e83000ULL;
  SmmuMappingEvidence E;
  for(unsigned Access=1;Access<=3;Access+=2) {
    assert(SmmuSetMapping(&Cached,SMMU_IOVA_BASE,Pa,3,Access)==CR_SUCCESS);
    UINT64 Oracle=AndroidCoherentPte(Access);
    /* Native owns one attribute byte: move Android's slot1 policy to slot0.
     * All permission/shareability/XN/AF/nG bits come from the real encoder. */
    Oracle=(Oracle & ~0x1cULL)|3;
    for(unsigned I=0;I<3;++I) assert(CachedTables[1024+I]==((Pa+I*4096)|Oracle));
    assert(Translate(&Cached,SMMU_IOVA_BASE)==Pa);
    assert(SmmuQueryMappingEvidence(&Cached,SMMU_IOVA_BASE,Pa,3,Access,&E)==CR_SUCCESS);
    assert(E.Mair0==AndroidCoherentMair() && E.Mair1==0);
  }
  assert(SmmuSetMapping(&Plain,SMMU_IOVA_BASE,Pa+0x10000,1,3)==CR_SUCCESS);
  assert(SmmuQueryMappingEvidence(&Plain,SMMU_IOVA_BASE,Pa+0x10000,1,3,&E)==CR_SUCCESS && E.Mair0==0x44);
  assert(Registers[0]==BeforeGlobal && Registers[SMR(Plain.Stream)/4]==BeforeSmr);
  for(unsigned Bit=0;Bit<8;++Bit) {
    Registers[(CB(Cached.Bank)+0x38)/4]=0xff^(1U<<Bit);
    unsigned W=Writes,P=Publishes;UINT64 Old=CachedTables[1024];
    assert(SmmuSetMapping(&Cached,SMMU_IOVA_BASE,Pa+4096,1,3)==CR_DEVICE_ERROR);
    assert(Writes==W && Publishes==P && CachedTables[1024]==Old);
    memset(&E,0xa5,sizeof(E));
    assert(SmmuQueryMappingEvidence(&Cached,SMMU_IOVA_BASE,Pa,3,3,&E)==CR_DEVICE_ERROR);
    SmmuMappingEvidence Empty={0};assert(memcmp(&E,&Empty,sizeof(E))==0);
  }
  Registers[(CB(Cached.Bank)+0x38)/4]=0xff;
  UINT64 Leaf=CachedTables[1024];
  for(unsigned Sh=0;Sh<3;++Sh) {
    CachedTables[1024]=(Leaf&~0x300ULL)|((UINT64)Sh<<8);
    assert(SmmuQueryMappingEvidence(&Cached,SMMU_IOVA_BASE,Pa,3,3,&E)==CR_DEVICE_ERROR);
  }
  CachedTables[1024]=Leaf;
  Cached.CacheCoherent=FALSE;
  assert(SmmuQueryMappingEvidence(&Cached,SMMU_IOVA_BASE,Pa,3,3,&E)==CR_DEVICE_ERROR);
  Cached.CacheCoherent=TRUE;
  unsigned W=Writes;assert(SmmuSetMapping(&Cached,SMMU_IOVA_BASE,1ULL<<Device.Stage1OutputAddressBits,1,3)==CR_INVALID_PARAMETER);assert(W==Writes);
  /* Revocation and per-domain physical exit work from the coherent policy;
   * the other domain remains translating and noncacheable. */
  assert(SmmuBlock(&Cached)==CR_SUCCESS);
  assert(SmmuQueryRevokedMapping(&Cached,SMMU_IOVA_BASE,SMMU_IOVA_SIZE/4096,&E)==CR_SUCCESS && E.Mair0==0xff);
  assert(SmmuReleaseToCoherentPhysicalAtExit(&Cached)==CR_SUCCESS);
  assert(Registers[CB(Cached.Bank)/4]==AndroidCoherentAttributes());
  assert(SmmuQueryMappingEvidence(&Plain,SMMU_IOVA_BASE,Pa+0x10000,1,3,&E)==CR_SUCCESS && E.Mair0==0x44);
  free(CachedTables);free(PlainTables);
}
static VOID RejectedMair(void) {
  for(unsigned Bit=0;Bit<8;++Bit) {
    Start();Device.Io.Write32=CoherentWrite;IgnoreMair=1U<<Bit;
    assert(SmmuAttachCoherent(&Device,&Cached,0x1c00,CachedTables,0xa77173000ULL)==CR_DEVICE_ERROR);
    assert(Cached.CacheCoherent && Cached.Failed && !Cached.Active && CoherentEnable==0);
    assert(Device.ReservedBanks[Cached.Bank] && Device.ReservedStreams[Cached.Stream] && Device.ReservedAsids[Cached.Asid]);
    assert(!(Registers[SMR(Cached.Stream)/4]&0x80000000));
    unsigned W=Writes;assert(SmmuAttach(&Device,&Cached,0x1c00,CachedTables,0xa77173000ULL)==CR_BUSY);assert(Writes==W);
    free(CachedTables);free(PlainTables);
  }
}
static VOID RestorePolicy(void) {
  Start();Registers[(CB(2)+0x38)/4]=0x7b;
  assert(SmmuAttachCoherent(&Device,&Cached,0x1c80,CachedTables,0xa77173000ULL)==CR_SUCCESS);
  assert(Cached.OriginalMair0==0x7b && Cached.CacheCoherent);
  assert(SmmuDetach(&Cached)==CR_SUCCESS && Registers[(CB(2)+0x38)/4]==0x7b);
  assert(SmmuAttach(&Device,&Cached,0x1c80,CachedTables,0xa77173000ULL)==CR_SUCCESS);
  assert(!Cached.CacheCoherent && Registers[(CB(Cached.Bank)+0x38)/4]==0x44);
  assert(SmmuDetach(&Cached)==CR_SUCCESS && Registers[(CB(2)+0x38)/4]==0x7b);
  free(CachedTables);free(PlainTables);
}
#endif
int main(void) {
#ifdef EXPECT_OLD_ATTACH
  Start();assert(SmmuAttach(&Device,&Cached,0xe0,CachedTables,0xa77173000ULL)==CR_SUCCESS);
  assert(Registers[(CB(Cached.Bank)+0x38)/4]==AndroidCoherentMair());
#else
  Isolation();RejectedMair();RestorePolicy();
  puts("PASS: coherent attach isolation, source-encoded PTE/MAIR, high PA, policy drift refusal, per-bit MAIR failure before enable, retention and saved policy restoration.");
#endif
  return 0;
}
