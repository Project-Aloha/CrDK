/* Actual observer tests with isolated hardware/table provider.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/SmmuDisplayCrLib.h>
static UINT32 Regs[0x100000/4];
static UINT64 Tables[4][512];
static UINT32 Writes, TableReads, FailTable, ChangeReg;
static UINT32 Read32(VOID *C,UINT64 A) {
  (void)C; assert(A>=0x15000000 && A<0x15100000 && !(A&3));
  UINT32 O=(UINT32)(A-0x15000000);
  if(ChangeReg && O==0x800) {if(++ChangeReg>2)Regs[O/4]^=2;}
  return Regs[O/4];
}
static VOID Write32(VOID *C,UINT64 A,UINT32 V){(void)C;(void)A;(void)V;++Writes;}
static VOID Write64(VOID *C,UINT64 A,UINT64 V){(void)C;(void)A;(void)V;++Writes;}
static CR_STATUS TableRead(VOID *C,UINT64 A,UINT64 *V) {
  (void)C;++TableReads;if(FailTable)return CR_UNSUPPORTED;
  if(A<0x90000000 || A>=0x90004000 || (A&7))return CR_UNSUPPORTED;
  *V=Tables[(A-0x90000000)/4096][((A-0x90000000)%4096)/8];return CR_SUCCESS;
}
/* The RED fixture models the absent observer, not an old production function. */
#ifdef ABSENT_DISPLAY_OBSERVER
CR_STATUS CrSmmuDisplayObserve(SmmuDevice *D,UINT64 A,UINT64 B,UINT64 C,UINT64 E,
    CR_SMMU_DISPLAY_TABLE_READ R,VOID *X,CR_SMMU_DISPLAY_OBSERVATION *O) {
  (void)D;(void)A;(void)B;(void)C;(void)E;(void)R;(void)X;memset(O,0,sizeof(*O));return CR_UNSUPPORTED;
}
#endif
static SmmuDevice Device;
static CR_SMMU_DISPLAY_OBSERVATION Observation;
static void Seed(void) {
  memset(Regs,0,sizeof(Regs));memset(Tables,0,sizeof(Tables));memset(&Device,0,sizeof(Device));
  Writes=TableReads=FailTable=ChangeReg=0;Device.ProbeState=SmmuProbeReady;
  Device.Base=0x15000000;Device.ContextBase=0x15080000;Device.PageSize=4096;
  Device.StreamCount=8;Device.BankCount=8;Device.AddressBits=48;
  Device.Stage1OutputAddressBits=48;Device.InputAddressBits=48;
  Device.Io=(SmmuIoOps){.Read32=Read32,.Write32=Write32,.Write64=Write64};
  Regs[0x800/4]=0x84020820;Regs[0xc00/4]=1U<<16;
}
static CR_STATUS Observe(void) {
  memset(&Observation,0xa5,sizeof(Observation));
  CR_STATUS S=CrSmmuDisplayObserve(&Device,0xe1000800,0x1800,
      0xe1000000,0x2300000,TableRead,NULL,&Observation);
  assert(!Writes);
  if(S!=CR_SUCCESS){CR_SMMU_DISPLAY_OBSERVATION Z={0};assert(!memcmp(&Observation,&Z,sizeof(Z)));}
  return S;
}
static void Translate(void) {
  Regs[0xc00/4]=2;Regs[0x1000/4+2]=1U<<16;Regs[0x1800/4+2]=1;
  Regs[0x82000/4]=1;Regs[0x82030/4]=32|(1U<<23);
  Regs[0x82010/4]=0x10;Regs[0x82020/4]=0x90000000;
  Tables[0][3]=0x90001003;Tables[1][0x108]=0xe1000401;
}
int main(void) {
  Seed();assert(Observe()==CR_SUCCESS);assert(Observation.PhysicalAddress==0xe1000800);
  assert(Observation.Count==1 && !TableReads);puts("PASS exact inherited bypass");
  Seed();Translate();assert(Observe()==CR_SUCCESS);assert(TableReads);puts("PASS 4K stage1 2MiB block and range crossing");
  Seed();Translate();Tables[1][0x108]=0x90002003;Tables[2][0]=0xe1000403;
  Tables[2][1]=0xe1001403;assert(Observe()==CR_SUCCESS);puts("PASS 4K leaf mapping nonaligned span");
  Seed();Translate();Tables[1][0x108]=0x90002003;Tables[2][0]=0xe1000403;
  Tables[2][1]=0xe1002403;assert(Observe()!=CR_SUCCESS);puts("PASS discontiguous final page refused");
  Seed();Translate();Tables[1][0x108]&=~(1ULL<<10);assert(Observe()!=CR_SUCCESS);
  Seed();Translate();FailTable=1;assert(Observe()!=CR_SUCCESS);puts("PASS inaccessible/invalid table rejected");
  Seed();Regs[0x800/4]=0x80000820;assert(Observe()!=CR_SUCCESS);
  Seed();Regs[0x804/4]=0x80010820;assert(Observe()!=CR_SUCCESS);
  Seed();Regs[0x800/4]=0x84060820;assert(Observe()!=CR_SUCCESS);puts("PASS incomplete overlapping or broad streams rejected");
  Seed();Translate();Regs[0x804/4]=0x80001c00;Regs[0xc04/4]=2;assert(Observe()!=CR_SUCCESS);
  Seed();Translate();Regs[0x1000/4+2]=3U<<16;assert(Observe()!=CR_SUCCESS);
  Seed();Translate();Regs[0x1800/4+2]=0;assert(Observe()!=CR_SUCCESS);
  Seed();Translate();Regs[0x82030/4]|=1U<<14;assert(Observe()!=CR_SUCCESS);puts("PASS shared bank nested and unsupported format rejected");
  Seed();Translate();Regs[0x82000/4]|=1U<<4;assert(Observe()!=CR_SUCCESS);
  Seed();Translate();Regs[0x82010/4]|=6;assert(Observe()!=CR_SUCCESS);
  Seed();Translate();Regs[0x82010/4]|=1U<<15;assert(Observe()!=CR_SUCCESS);
  Seed();Translate();Device.InputAddressBits=31;assert(Observe()!=CR_SUCCESS);
  puts("PASS endian and address-geometry rejection");
  Seed();ChangeReg=1;assert(Observe()!=CR_SUCCESS);puts("PASS changed inventory and zeroed failed evidence");
  Seed();Regs[0xc00/4]=2U<<16;assert(Observe()!=CR_SUCCESS);
  Seed();Device.ProbeState=SmmuProbeFailed;assert(Observe()!=CR_SUCCESS);
  Seed();Device.ReservedStreams[0]=TRUE;assert(Observe()!=CR_SUCCESS);
  Seed();Translate();Device.ReservedBanks[2]=TRUE;assert(Observe()!=CR_SUCCESS);
  Seed();Translate();Device.PhysicalExitBanks[2]=TRUE;assert(Observe()!=CR_SUCCESS);
  Seed();Device.InheritedStreams[0]=TRUE;assert(Observe()!=CR_SUCCESS);
  Seed();Regs[0]=1;assert(Observe()!=CR_SUCCESS);
  Seed();Regs[0]=8;assert(Observe()!=CR_SUCCESS);
  Seed();assert(CrSmmuDisplayObserve(&Device,~0ULL,2,0xe1000000,0x2300000,
    TableRead,NULL,&Observation)!=CR_SUCCESS);puts("PASS fault route probe and overflow bounds");
  return 0;
}
