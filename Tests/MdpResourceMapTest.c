/* Exercise the actual framebuffer mapping consumer with provider failures. */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../Driver/MDSSCrDxe/MdpResources.c"
EFI_GUID gEfiSmmuCrProtocolGuid, gEdkiiIoMmuProtocolGuid;
EFI_GUID gEfiClockCrProtocolGuid, gEfiRpmhCrProtocolGuid, gEfiInterconnectCrProtocolGuid;
static UINT32 expectedController;
static BOOLEAN fullInit, failSource, failAhbRelease;
static unsigned clockVotes, railVotes, pathVotes;
static unsigned scenario, grants, revokes, maps, preserves, attaches;
static BOOLEAN failPreserve;
static BOOLEAN commandTest, failUnmap, failFree;
static unsigned allocations, frees, failGrant;
static VOID *commandHost;
static UINT32 phyRegisters[0x2000 / sizeof (UINT32)];
static BOOLEAN biasEnabled, biasSettled;
static unsigned biasEnableWrites, biasDisableWrites;
UINT32 EFIAPI MmioRead32(UINTN address) {
  if (fullInit && address >= DISP_CC_BASE && address < DISP_CC_BASE + 0x10000) {
    if (address == DISP_CC_BASE + 0x9000) return BIT31;
    if (address == DISP_CC_BASE + mDsiTarget->ByteConfig) return (mDsiTarget->ByteSource << 8) | 1;
    if (address == DISP_CC_BASE + mDsiTarget->PixelConfig) return (mDsiTarget->PixelSource << 8) | 1;
    assert(address == DISP_CC_BASE + mDsiTarget->ByteDivider); return 1;
  }
  assert(address >= PHY_BASE && address < PHY_BASE + sizeof(phyRegisters));
  if (address == PHY_BASE + PHY_STATUS || address == PLL_BASE + 0x1B0) return BIT0;
  return phyRegisters[(address - PHY_BASE) / sizeof(UINT32)];
}
UINT32 EFIAPI MmioWrite32(UINTN address, UINT32 value) {
  assert(address >= PHY_BASE && address < PHY_BASE + sizeof(phyRegisters));
  if (address == PLL_BASE + PLL_SYSTEM_MUXES) {
    biasEnabled = value == 0xC0;
    biasSettled = FALSE;
    if (biasEnabled) biasEnableWrites++; else biasDisableWrites++;
  }
  if (address == PLL_BASE + 0x0BC) assert(biasEnabled && biasSettled);
  phyRegisters[(address - PHY_BASE) / sizeof(UINT32)] = value;
  return value;
}
UINT32 EFIAPI MmioOr32(UINTN address, UINT32 value) { return MmioWrite32(address, MmioRead32(address) | value); }
UINT32 EFIAPI MmioAnd32(UINTN address, UINT32 value) { return MmioWrite32(address, MmioRead32(address) & value); }
UINTN EFIAPI NanoSecondDelay(UINTN delay) {
  if (biasEnabled) { assert(delay >= 250); biasSettled = TRUE; }
  return delay;
}
UINTN EFIAPI MicroSecondDelay(UINTN delay) { return delay; }
VOID EFIAPI MemoryFence(VOID) { __asm__ __volatile__("" ::: "memory"); }
VOID *EFIAPI ZeroMem(VOID *buffer, UINTN length) { return memset(buffer, 0, length); }
RETURN_STATUS EFIAPI AsciiStrCpyS(CHAR8 *d,UINTN n,CONST CHAR8 *s){assert(strlen(s)<n);strcpy(d,s);return RETURN_SUCCESS;}
static EFI_STATUS EFIAPI Attach(EFI_SMMU_CR_PROTOCOL *p, UINT16 seg, UINT16 bdf, UINT16 sid) { attaches++; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Map(EDKII_IOMMU_PROTOCOL *p, EDKII_IOMMU_OPERATION op, VOID *host, UINTN *size, EFI_PHYSICAL_ADDRESS *addr, VOID **mapping) {
  maps++;
  if (commandTest) {
    assert(op == EdkiiIoMmuOperationBusMasterCommonBuffer && host == commandHost);
    assert(*size == EFI_PAGE_SIZE);
  }
  if (scenario == 5) return EFI_OUT_OF_RESOURCES;
  *addr = scenario == 4 ? 0x100000000ULL : scenario == 2 ? 0xfffff000ULL : 0x100000;
  if (scenario == 1 || scenario == 3) *size /= 2;
  *mapping = (VOID *)0x1234;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap(EDKII_IOMMU_PROTOCOL *p, VOID *mapping) { ++revokes; return (scenario == 3 || failUnmap) ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
static EFI_STATUS EFIAPI Grant(EDKII_IOMMU_PROTOCOL *p, UINT64 base, UINT32 sid, VOID *mapping, UINT64 access) {
  static const UINT32 sids[] = {0x2800, 0x2802, 0x2c00, 0x2c02};
  assert(access == EDKII_IOMMU_ACCESS_READ && base == SMMU_BASE);
  assert(sid == sids[grants % 4]);
  ++grants; return grants == failGrant ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Allocate(EDKII_IOMMU_PROTOCOL *p, EFI_ALLOCATE_TYPE type,
    EFI_MEMORY_TYPE memory, UINTN pages, VOID **host, UINT64 attributes) {
  assert(pages == 1); ++allocations;
  commandHost = aligned_alloc(EFI_PAGE_SIZE, EFI_PAGE_SIZE);
  assert(commandHost); *host = commandHost; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free(EDKII_IOMMU_PROTOCOL *p, UINTN pages, VOID *host) {
  assert(pages == 1 && host == commandHost);
  if (failFree) return EFI_DEVICE_ERROR;
  ++frees; free(host); commandHost = NULL; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Preserve(EFI_SMMU_CR_PROTOCOL *p, VOID *mapping) {
  assert(!commandTest && mapping == (VOID *)0x1234);
  /* A mapping becomes eligible only after every read-only stream grant. */
  assert(grants == 4 && revokes == 0 && maps == 1);
  preserves++;
  return failPreserve ? EFI_ACCESS_DENIED : EFI_SUCCESS;
}
static EFI_SMMU_CR_PROTOCOL smmu = {
  .Revision = EFI_SMMU_CR_PRESERVE_MAPPING_REVISION,
  .Attach = Attach, .PreserveReadOnlyMapping = Preserve
};
static EDKII_IOMMU_PROTOCOL iommu = { .Map = Map, .Unmap = Unmap, .SetAttributeById = Grant, .AllocateBuffer = Allocate, .FreeBuffer = Free };
static void CheckClock(CONST CHAR8 *name) {
  if (strstr(name,"byte") || strstr(name,"pclk") || strstr(name,"esc")) {
    assert(strchr(name, expectedController ? '1' : '0'));
    assert(!strchr(name, expectedController ? '0' : '1'));
  }
}
static EFI_STATUS EFIAPI Clock(EFI_CLOCK_CR_PROTOCOL *p, CONST CHAR8 *c, CONST CHAR8 *n, UINT64 r, BOOLEAN enable) {
  /* The two AHB leaves share an RCG. An extra zero-rate non-GDSC vote used
     to reject the later explicit AHB request with EFI_ACCESS_DENIED. */
  assert(strcmp(n, "disp_cc_mdss_non_gdsc_ahb_clk"));
  if (enable && !strcmp(n, "disp_cc_mdss_ahb_clk")) assert(r == 19200000);
  if (enable && (!strcmp(n, "disp_cc_mdss_mdp_clk") || !strcmp(n, "disp_cc_mdss_mdp_lut_clk")))
    assert(r == 200000000);
  if (fullInit && strcmp(n, "gcc_disp_ahb_clk") && strcmp(n, "gcc_disp_xo_clk"))
    assert(mGdscOn);
  if (!enable && failAhbRelease && !strcmp(n, "disp_cc_mdss_ahb_clk"))
    return EFI_DEVICE_ERROR;
  CheckClock(n); if(enable)clockVotes++;else {assert(clockVotes);clockVotes--;} return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Gdsc(EFI_CLOCK_CR_PROTOCOL *p, CONST CHAR8 *c, CONST CHAR8 *n, BOOLEAN enable) {
  assert(mClocks[0].Enabled && mClocks[1].Enabled);
  if (!enable)
    for (UINTN i = ACCESS_CLOCK_COUNT; i < ARRAY_SIZE(mClocks); ++i)
      assert(!mClocks[i].Enabled);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI AcquireClockMock(EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN *Owned)
{
  EFI_STATUS Status = Clock(P, C, N, R, TRUE);
  *Owned = !EFI_ERROR(Status);
  return Status;
}
static EFI_STATUS EFIAPI AcquireGdscMock(EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, BOOLEAN *Owned)
{
  EFI_STATUS Status = Gdsc(P, C, N, TRUE);
  *Owned = !EFI_ERROR(Status);
  return Status;
}
static EFI_STATUS EFIAPI Source(EFI_CLOCK_CR_PROTOCOL *p, CONST CHAR8 *c, CONST CHAR8 *n, UINT32 rate, UINT32 source, UINT32 div, UINT32 m, UINT32 d, UINT32 twice) {
  CheckClock(n); assert(source == (strstr(n,"byte") ? 2U : 1U) + expectedController * 2);
  assert(div == 2); return failSource ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Divider(EFI_CLOCK_CR_PROTOCOL *p, CONST CHAR8 *c, CONST CHAR8 *n, UINT32 div) { CheckClock(n); assert(div==2);return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Rail(EFI_RPMH_CR_PROTOCOL *p, CONST CHAR8 *n, UINT32 mv, UINT8 mode, BOOLEAN *owned) {
  assert((!strcmp(n,"ldob6") && mv==1200) || (!strcmp(n,"ldob5") && mv==880));
  assert(mode==7);railVotes++;*owned=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI RailEnable(EFI_RPMH_CR_PROTOCOL *p, CONST CHAR8 *n, BOOLEAN enable) { assert(!enable && railVotes);railVotes--;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI RailVoltage(EFI_RPMH_CR_PROTOCOL *p, CONST CHAR8 *n, UINT32 v) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI RailMode(EFI_RPMH_CR_PROTOCOL *p, CONST CHAR8 *n, UINT8 v) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Path(EFI_INTERCONNECT_CR_PROTOCOL *p, CONST CHAR8 *c, UINT32 src, UINT32 dst, INTERCONNECT_PATH_HANDLE *h) { *h=++pathVotes;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Bandwidth(EFI_INTERCONNECT_CR_PROTOCOL *p, INTERCONNECT_PATH_HANDLE h, UINT64 a, UINT64 b) {return EFI_SUCCESS;}
static EFI_STATUS EFIAPI ReleasePath(EFI_INTERCONNECT_CR_PROTOCOL *p, INTERCONNECT_PATH_HANDLE h) {assert(pathVotes);pathVotes--;return EFI_SUCCESS;}
static EFI_CLOCK_CR_PROTOCOL clocks={.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,.SetClock=Clock,.SetGdsc=Gdsc,.AcquireClock=AcquireClockMock,.AcquireGdsc=AcquireGdscMock,.ConfigureExternalSource=Source,.SetDivider=Divider};
static EFI_RPMH_CR_PROTOCOL rails={.Revision=EFI_RPMH_CR_PROTOCOL_REVISION,.AcquireVreg=Rail,.RpmhEnableVreg=RailEnable,.RpmhSetVregVoltage=RailVoltage,.RpmhSetVregMode=RailMode};
static EFI_INTERCONNECT_CR_PROTOCOL paths={.Revision=EFI_INTERCONNECT_CR_PROTOCOL_REVISION,.AcquirePath=Path,.SetBandwidth=Bandwidth,.ReleasePath=ReleasePath};
static EFI_STATUS EFIAPI Locate(EFI_GUID *guid, VOID *registration, VOID **result) { if(guid==&gEfiClockCrProtocolGuid) {*result=&clocks;return EFI_SUCCESS;}
if(guid==&gEfiRpmhCrProtocolGuid) {*result=&rails;return EFI_SUCCESS;}
if(guid==&gEfiInterconnectCrProtocolGuid) {*result=&paths;return EFI_SUCCESS;}
*result = guid == &gEfiSmmuCrProtocolGuid ? (VOID *)&smmu : (VOID *)&iommu; return EFI_SUCCESS; }
static EFI_BOOT_SERVICES services = { .LocateProtocol = Locate };
EFI_BOOT_SERVICES *gBS = &services;

static void TestCommandBuffer(void) {
  unsigned previousPreserves = preserves;
  VOID *cpu;
  EFI_PHYSICAL_ADDRESS device;
  UINTN capacity;
  commandTest = TRUE; scenario = 0; grants = revokes = 0;
  mReady = TRUE; mRetained = FALSE; mFrameBufferMapping = NULL;
  assert(MdpAcquireCommandBuffer(&cpu, &device, &capacity) == EFI_SUCCESS);
  assert(cpu == commandHost && device == 0x100000 && capacity == EFI_PAGE_SIZE);
  memset(cpu, 0x5a, capacity); /* packet writes happen after the common map */
  assert(MdpAcquireCommandBuffer(&cpu, &device, &capacity) == EFI_ALREADY_STARTED);
  assert(cpu == NULL && device == 0 && capacity == 0);
  failUnmap = TRUE;
  assert(MdpReleaseResources() == EFI_DEVICE_ERROR);
  assert(mReady && mCommandMapping && commandHost && frees == 0);
  assert(MdpAcquireCommandBuffer(&cpu, &device, &capacity) == EFI_NOT_READY);
  failUnmap = FALSE; failFree = TRUE;
  assert(MdpReleaseCommandBuffer() == EFI_DEVICE_ERROR);
  assert(mCommandMapping == NULL && commandHost && frees == 0);
  failFree = FALSE;
  assert(MdpReleaseCommandBuffer() == EFI_SUCCESS && frees == 1);
  for (unsigned fault = 0; fault < 6; ++fault) {
    grants = revokes = 0; scenario = fault == 0 ? 1 : 0;
    failGrant = fault >= 2 ? fault - 1 : 0;
    /* Invalid IOVA alignment/range is checked before granting SIDs. */
    if (fault == 1) scenario = 4;
    EFI_STATUS status = MdpAcquireCommandBuffer(&cpu, &device, &capacity);
    assert(EFI_ERROR(status) && cpu == NULL && device == 0 && capacity == 0);
    assert(commandHost == NULL && mCommandMapping == NULL && revokes == 1);
  }
  scenario = 0; grants = 0; failGrant = 2; failUnmap = TRUE;
  assert(MdpAcquireCommandBuffer(&cpu, &device, &capacity) == EFI_DEVICE_ERROR);
  assert(commandHost && mCommandMapping && mCommandRetained);
  failUnmap = FALSE; failGrant = 0;
  assert(MdpReleaseCommandBuffer() == EFI_SUCCESS);
  grants = revokes = 0; scenario = 5;
  assert(MdpAcquireCommandBuffer(&cpu, &device, &capacity) == EFI_OUT_OF_RESOURCES);
  assert(commandHost == NULL && mCommandMapping == NULL && revokes == 0);
  scenario = 0;
  assert(allocations == frees);
  assert(preserves == previousPreserves);
  commandTest = FALSE;
}
static void ResetMapCase(void) {
  scenario = maps = attaches = grants = revokes = preserves = failGrant = 0;
  failPreserve = failUnmap = FALSE;
  mReady = TRUE; mRetained = FALSE; mMappedSize = 0; mFrameBufferMapping = NULL;
}
static void TestPreserveMapping(void) {
  EFI_PHYSICAL_ADDRESS address;
  ResetMapCase();
  smmu.Revision = EFI_SMMU_CR_PRESERVE_MAPPING_REVISION - 1;
  assert(MdpMapFrameBuffer(0x800000, 8192, &address) == EFI_NOT_READY);
  assert(!address && !maps && !attaches && !grants && !preserves);
  smmu.Revision = EFI_SMMU_CR_PRESERVE_MAPPING_REVISION;
  smmu.PreserveReadOnlyMapping = NULL;
  assert(MdpMapFrameBuffer(0x800000, 8192, &address) == EFI_NOT_READY);
  assert(!address && !maps && !attaches && !grants && !preserves);
  smmu.PreserveReadOnlyMapping = Preserve;
  for(unsigned grant = 1; grant <= 4; grant++) {
    ResetMapCase(); failGrant = grant;
    assert(MdpMapFrameBuffer(0x800000, 8192, &address) == EFI_DEVICE_ERROR);
    assert(!address && maps == 1 && grants == grant && !preserves && revokes == 1);
    assert(!mRetained && !mFrameBufferMapping);
  }
  for(unsigned retain = 0; retain < 2; retain++) {
    ResetMapCase(); failPreserve = TRUE; failUnmap = retain;
    assert(MdpMapFrameBuffer(0x800000, 8192, &address) == EFI_ACCESS_DENIED);
    assert(!address && maps == 1 && grants == 4 && preserves == 1 && revokes == 1);
    assert(mRetained == retain && (mFrameBufferMapping != NULL) == retain && !mMappedSize);
    if(retain) {
      assert(MdpMapFrameBuffer(0x800000, 8192, &address) == EFI_NOT_READY);
      assert(maps == 1 && preserves == 1);
    }
  }
  ResetMapCase();
  assert(MdpMapFrameBuffer(0x800000, 8192, &address) == EFI_SUCCESS);
  assert(address == 0x100000 && maps == 1 && grants == 4 && preserves == 1 && !revokes);
  assert(MdpMapFrameBuffer(0x800000, 8192, &address) == EFI_SUCCESS);
  assert(maps == 1 && grants == 4 && preserves == 1);
  ResetMapCase();
}
int main(void) {
  EFI_PHYSICAL_ADDRESS addr;
  TestPreserveMapping();
  for (scenario = 0; scenario < 4; ++scenario) {
    grants = revokes = maps = preserves = 0; mReady = TRUE; mRetained = FALSE;
    mMappedSize = 0; mFrameBufferMapping = NULL;
    EFI_STATUS status = MdpMapFrameBuffer(0x800000, 8192, &addr);
    if (scenario == 0) assert(status == EFI_SUCCESS && grants == 4 && preserves == 1 && revokes == 0);
    else {
      assert(status == (scenario == 2 ? EFI_UNSUPPORTED : EFI_BAD_BUFFER_SIZE));
      assert(addr == 0 && grants == 0 && preserves == 0 && revokes == 1);
      assert(mRetained == (scenario == 3));
      assert((mFrameBufferMapping != NULL) == (scenario == 3));
    }
  }
  assert(MdpMapFrameBuffer(0, MAX_UINTN, &addr) == EFI_INVALID_PARAMETER);
  mFrameBufferMapping = NULL;
  mRetained = FALSE;
  for (UINTN index = 0; index < ARRAY_SIZE(mIcbHandles); ++index)
    mIcbHandles[index] = INTERCONNECT_INVALID_PATH;
  for (UINT32 controller = 0; controller < 2; ++controller) {
    UINT32 active;
    /* Exercise each register bank independently before testing the full
       provider lifecycle and Android-proven shared rail votes below. */
    mDsiTarget = &mDsiTargets[controller];
    mActiveController = controller;
    memset(phyRegisters, 0, sizeof(phyRegisters));
    assert(PHY_BASE == 0x0ae94400U + controller * 0x2000U);
    assert(LANE_BASE == 0x0ae94600U + controller * 0x2000U);
    assert(PLL_BASE == 0x0ae94900U + controller * 0x2000U);
    assert(strstr(mDsiTarget->ByteClock, controller ? "byte1" : "byte0"));
    assert(strstr(mDsiTarget->PixelClock, controller ? "pclk1" : "pclk0"));
    assert(strstr(mDsiTarget->EscapeClock, controller ? "esc1" : "esc0"));
    assert(MdpGetActiveController(&active) == EFI_SUCCESS && active == controller);
    assert(EnablePhy(148500000ULL, 4) == EFI_SUCCESS);
    assert(biasEnableWrites == controller + 1 && biasSettled && mPhyTouched);
    assert(MdpReleaseResources() == EFI_SUCCESS);
    assert(biasDisableWrites == controller + 1 && !biasEnabled && !mPhyTouched);
    assert(MdpGetActiveController(&active) == EFI_NOT_READY);
  }
  TestCommandBuffer();
  mReady=FALSE; fullInit=TRUE;
  for(expectedController=0;expectedController<2;expectedController++) {
    EFI_MDP_DSI_TIMING timing={.Width=1920,.Height=1080,.HFrontPorch=88,.HBackPorch=148,.HSync=44,
      .VFrontPorch=4,.VBackPorch=36,.VSync=5,.DsiController=expectedController,.DsiColorFormat=36,.DsiLanes=4,.DsiRefreshRate=60U<<16};
    for(unsigned fault=0;fault<2;fault++) {
      UINT32 active;
      memset(phyRegisters,0,sizeof(phyRegisters));failSource=fault;
      assert(MdpInitializeResources(&timing)==(fault ? EFI_DEVICE_ERROR:EFI_SUCCESS));
      if(!fault) {
        assert(MdpGetActiveController(&active)==EFI_SUCCESS && active==expectedController);
        CR_MDSS_HANDOFF_PARTICIPANT snapshot={.Owner=CR_MDSS_NATIVE_DSI};VOID *mapping=(VOID*)1;
        assert(MdpSnapshotResources(&snapshot,&mapping)==EFI_SUCCESS);
        assert(snapshot.ClockCount==12 && snapshot.RailCount==2 && snapshot.PathCount==2 && snapshot.DomainCount==1);
        assert(snapshot.Route.SourceRate==111375000 && snapshot.Route.SourceSelect==(expectedController?4:2));
        assert(snapshot.Route.SourceDivider==2 && snapshot.Route.PixelDivider==2 && snapshot.Route.InterfaceDivider==2);
        assert(snapshot.Route.PixelSource==(expectedController?3:1));
        assert(!strcmp(snapshot.Rails[0].Name,"ldob6") && snapshot.Rails[0].Millivolts==1200 && snapshot.Rails[0].Mode==7);
        assert(snapshot.Paths[0].Average==1000 && snapshot.Paths[0].Peak==1000);
        assert(snapshot.Paths[1].Average==594000 && snapshot.Paths[1].Peak==1188000);
        assert(!strcmp(snapshot.Domains[0].Name,"disp_cc_mdss_core_gdsc"));
        assert(!snapshot.Route.MappingId && !snapshot.Route.MappingOwner && !snapshot.Route.StreamCount);
        failAhbRelease = TRUE;
        assert(MdpReleaseResources()==EFI_DEVICE_ERROR);
        assert(mRetained && mGdscOn && mClocks[ACCESS_CLOCK_COUNT].Enabled);
        assert(mClocks[0].Enabled && mClocks[1].Enabled);
        memset(&snapshot,0,sizeof(snapshot));snapshot.Owner=CR_MDSS_NATIVE_DSI;
        assert(MdpSnapshotResources(&snapshot,&mapping)==EFI_NOT_READY);
        assert(snapshot.ClockCount==3 && snapshot.RailCount==2 && snapshot.PathCount==2 && snapshot.DomainCount==1);
        for(unsigned n=0;n<snapshot.ClockCount;n++)assert(snapshot.Clocks[n].Flags==3);
        failAhbRelease = FALSE;
        assert(MdpReleaseResources()==EFI_SUCCESS);
      }
      assert(!clockVotes && !railVotes && !pathVotes && !mPhyTouched);
      assert(MdpGetActiveController(&active)==EFI_NOT_READY);
    }
  }
  puts("MDP framebuffer preservation revision/order/failure retention, command exclusion and DSI PLL sequencing PASS");
  return 0;
}
