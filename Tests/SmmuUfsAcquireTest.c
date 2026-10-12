/* SPDX-License-Identifier: MIT
 * Real core regression for the observed HDK8245 exact inherited E0 route.
 * Removing the explicit acquisition or preparing/committing out of order must
 * fail; MMIO is modeled, native ownership and page-table code remain real. */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/smmu.h>
#include <Library/DebugLib.h>
#define BASE 0x15000000ULL
#define CB(n) (0x80000U + (n) * 4096U)
#define SMR(n) (0x800U + 4U*(n))
#define S2CR(n) (0xc00U + 4U*(n))
static UINT32 R[0x100000/4], Before[0x100000/4];
static UINTN Writes, Checks, FailCheck, Publishes;
static UINT64 Reject;
static BOOLEAN Watch, Timeout;
static UINTN RaceCheck;
static UINT32 RaceOffset;
static BOOLEAN CorruptS2cr;
static CR_STATUS StopStatus;
static SmmuDevice Device;
static SmmuDomain Domain, Pci[6];
static VOID *Table, *PciTables[6];
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) {(void)Level;(void)Format;}
static UINT32 Read32(VOID *Cookie, UINT64 Address) {
  (void)Cookie;assert(Address>=BASE&&Address<BASE+sizeof(R));
  if(Timeout && Address>=BASE+CB(0) && (Address&0xfff)==0x7f4)return 1;
  return R[(Address-BASE)/4];
}
static VOID Write32(VOID *Cookie,UINT64 Address,UINT32 Value) {
  (void)Cookie;assert(Address>=BASE&&Address<BASE+sizeof(R));Writes++;
  if(Watch) {
    UINT64 Offset=Address-BASE;
    assert(R[SMR(3)/4]==0x800000e0);
    assert(Offset==S2CR(3) || (Offset>=CB(11)&&Offset<CB(12)) ||
           Offset==0x1000+11*4 || Offset==0x1800+11*4);
    if(Offset==S2CR(3)) {
      assert(Value==11 && (R[CB(11)/4]&1) && Publishes && Checks>=3);
      assert(R[(CB(11)+0x38)/4]==0x44);
    }
  }
  if(Address==Reject)return;
  if(CorruptS2cr&&Address==BASE+S2CR(3))Value=12;
  if(Address>=BASE+CB(0)&&(Address&0xfff)==0x10)Value|=0x60;
  if(Address>=BASE+CB(0)&&(Address&0xfff)==0)Value&=~0x1000U;
  R[(Address-BASE)/4]=Value;
}
static VOID Write64(VOID *C,UINT64 A,UINT64 V){Write32(C,A,(UINT32)V);Write32(C,A+4,(UINT32)(V>>32));}
static VOID Publish(VOID *C,VOID *A,UINTN N){(void)C;assert(A&&N);Publishes++;}
static VOID Delay(VOID *C,UINT32 N){(void)C;(void)N;}
static CR_STATUS Stopped(VOID *Cookie){
  assert(Cookie==&Device);Checks++;
  if(Checks==RaceCheck)R[RaceOffset/4]^=1;
  return Checks==FailCheck?StopStatus:CR_SUCCESS;
}
static SmmuIoOps Ops={Read32,Write32,Write64,Publish,Delay,NULL};
static VOID *Tables(VOID){VOID *P=NULL;assert(!posix_memalign(&P,4096,SMMU_TABLE_PAGES*4096));memset(P,0xa5,SMMU_TABLE_PAGES*4096);return P;}
static VOID Start(VOID){
  static CONST UINT16 Sids[]={0x1c00,0x1c01,0x1c80,0x1c81,0x1d00,0x1d01};
  memset(R,0,sizeof(R));memset(&Device,0,sizeof(Device));memset(&Domain,0,sizeof(Domain));memset(Pci,0,sizeof(Pci));
  R[0x20/4]=0x4c017e57;R[0x24/4]=0x6000004a;R[0x28/4]=0x5111;R[0]=0x270406;
  for(UINTN I=0;I<5;I++) {
    R[SMR(I)/4]=0x80000000U+(UINT32)(0x900+I);R[S2CR(I)/4]=(UINT32)I;
    R[CB(I)/4]=0xe0;R[(0x1000+I*4)/4]=0x100ff;R[(0x1800+I*4)/4]=1;
  }
  R[SMR(3)/4]=0x800000e0;R[(CB(3)+0x10)/4]=0xc061;
  Writes=Checks=FailCheck=Publishes=0;Watch=Timeout=FALSE;Reject=0;
  RaceCheck=RaceOffset=0;CorruptS2cr=FALSE;StopStatus=CR_BUSY;
  assert(SmmuProbe(&Device,BASE,sizeof(R),&Ops)==CR_SUCCESS);
  Device.SctlrReadbackIgnoreMask=0x1000;
  for(UINTN I=0;I<6;I++)assert(SmmuAttachCoherent(&Device,&Pci[I],Sids[I],PciTables[I],0x21000000+I*0x22000)==CR_SUCCESS);
  memcpy(Before,R,sizeof(R));Writes=Publishes=0;Watch=TRUE;
}
static CR_STATUS Acquire(VOID) {
#ifdef SMMU_STOPPED_STREAM_REVISION
  return SmmuAcquireStoppedStream(&Device,&Domain,0xe0,Table,0x23000000,FALSE,Stopped,&Device);
#else
  (void)Stopped;
  return SmmuAttach(&Device,&Domain,0xe0,Table,0x23000000);
#endif
}
static VOID Preserved(VOID) {
  for(UINTN I=0;I<sizeof(R)/sizeof(R[0]);I++) {
    UINTN O=I*4;
    if(O==S2CR(3)||(O>=CB(11)&&O<CB(12))||O==0x1000+11*4||O==0x1800+11*4)continue;
    assert(R[I]==Before[I]);
  }
}
int main(void){
  Table=Tables();for(UINTN I=0;I<6;I++)PciTables[I]=Tables();
  Start();assert(SmmuAttach(&Device,&Domain,0xe0,Table,0x23000000)==CR_BUSY&&Writes==0);
  /* RED on the current source: valid SMR3 blocks native UFS acquisition. */
  assert(Acquire()==CR_SUCCESS);
#ifdef SMMU_STOPPED_STREAM_REVISION
  assert(Domain.Active&&Domain.InheritedStreamOwned&&Domain.Stream==3&&Domain.Bank==11);
  assert(Device.ReservedBanks[3]&&Device.ReservedBanks[11]&&R[SMR(3)/4]==0x800000e0&&R[S2CR(3)/4]==11);
  Preserved();
  for(UINTN I=1024;I<SMMU_TABLE_PAGES*4096/8;I++)assert(Domain.Tables[I]==0);
  UINTN W=Writes;assert(SmmuDetach(&Domain)==CR_BUSY&&Writes==W);
  assert(SmmuReleaseToPhysicalAtExit(&Domain)==CR_BUSY&&Writes==W);
  assert(SmmuReleaseToCoherentPhysicalAtExit(&Domain)==CR_BUSY&&Writes==W);
  SmmuDomain Stale=Domain;Stale.InheritedStreamOwned=FALSE;
  assert(SmmuDetach(&Stale)==CR_BUSY&&Writes==W);
  assert(SmmuReleaseToPhysicalAtExit(&Stale)==CR_BUSY&&Writes==W);
  for(UINTN I=0;I<9;I++) {
    Start();
    switch(I) {
      case 0: R[SMR(3)/4]=0x800100e0;break;
      case 1: R[SMR(4)/4]=0x800000e0;break;
      case 2: R[S2CR(4)/4]=3;break;
      case 3: R[S2CR(3)/4]=0x10003;break;
      case 4: R[S2CR(3)/4]=74;break;
      case 5: Device.ReservedBanks[3]=TRUE;break;
      case 6: R[(0x1000+3*4)/4]=0;break;
      case 7: FailCheck=1;break;
      case 8: FailCheck=2;break;
    }
    memcpy(Before,R,sizeof(R));assert(Acquire()!=CR_SUCCESS&&Writes==0);
    assert(!memcmp(Before,R,sizeof(R))&&!Domain.Failed);
  }
  Start();FailCheck=3;assert(Acquire()==CR_BUSY&&Domain.Failed);
  assert(R[S2CR(3)/4]==3&&Device.ReservedBanks[11]&&Device.ReservedStreams[3]);Preserved();
  Start();Timeout=TRUE;assert(Acquire()==CR_TIMEOUT&&Domain.Failed&&Device.SyncFailed&&R[S2CR(3)/4]==3);Preserved();
  static CONST UINTN Rejected[]={0x1800+11*4,0x1000+11*4,CB(11)+0x10,CB(11)+0x30,CB(11)+0x20,CB(11)+0x24,CB(11)+0x28,CB(11)+0x2c,CB(11)+0x38,CB(11)};
  for(UINTN I=0;I<ARRAY_SIZE(Rejected);I++) {
    Start();Reject=BASE+Rejected[I];
    /* TTBR1 low/MAIR1 zero are already zero, so a dropped zero write is
       indistinguishable from success; seed it nonzero for rejection. */
    R[Rejected[I]/4]=0xa5a5a5a4;assert(Acquire()==CR_DEVICE_ERROR&&Domain.Failed&&R[S2CR(3)/4]==3);
    Preserved();
  }
  Start();Reject=BASE+S2CR(3);assert(Acquire()==CR_DEVICE_ERROR&&Domain.Failed&&R[S2CR(3)/4]==3);Preserved();
  Start();FailCheck=4;assert(Acquire()==CR_BUSY&&Domain.Failed&&R[S2CR(3)/4]==11);
  assert(Device.ReservedBanks[11]&&Device.InheritedStreams[3]);Preserved();
  Start();CorruptS2cr=TRUE;assert(Acquire()==CR_DEVICE_ERROR&&Domain.Failed&&R[S2CR(3)/4]==12);
  assert(Device.ReservedBanks[11]&&Device.InheritedStreams[3]);Preserved();
  for(UINTN I=0;I<4;I++) {
    static CONST UINT32 RaceOffsets[]={0,S2CR(0),CB(3)+0x30,CB(11)};
    Start();RaceCheck=2;RaceOffset=RaceOffsets[I];
    assert(Acquire()==CR_BUSY&&!Writes&&!Domain.Failed);
    Start();RaceCheck=3;RaceOffset=RaceOffsets[I];
    assert(Acquire()==CR_BUSY&&Domain.Failed&&R[S2CR(3)/4]==3&&Device.ReservedBanks[11]);
  }
  Start();FailCheck=1;StopStatus=1;
  assert(Acquire()==CR_NOT_READY&&!Writes&&!Domain.Failed);
  Start();FailCheck=4;StopStatus=1;
  assert(Acquire()==CR_NOT_READY&&Domain.Failed&&R[S2CR(3)/4]==11);
#else
  (void)Preserved;
#endif
  free(Table);for(UINTN I=0;I<6;I++)free(PciTables[I]);
  puts("Stopped UFS: observed exact collision, continuous route, preserved foreign owners and retained failures PASS");return 0;
}
