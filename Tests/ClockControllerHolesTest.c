/* Actual ClockLib sparse controller admission, native and NT. MIT */
#define main ClockPriorOwnershipRegression
#include "ClockOwnershipTest.c"
#undef main
static UINTN Maps,Unmaps;
#ifndef _KERNEL_MODE
CR_STATUS MapDeviceIORegion(UINTN Address,UINTN Size) {
  assert(Address && Size);++Maps;return CR_SUCCESS;
}
CR_STATUS UnMapMemRegion(UINTN Address,UINTN Size) {
  assert(Address && Size);++Unmaps;return CR_SUCCESS;
}
#endif
static VOID Sparse(VOID) {
  ClockController C[CC_TYPE_DISP_CC+1]={0};
  C[0]=(ClockController){.Address=0x100000,.Size=0x1f0000};
  C[CC_TYPE_DISP_CC]=(ClockController){.Address=0xaf00000,.Size=0x20000};
#ifdef _KERNEL_MODE
  C[0].MemMapped=C[CC_TYPE_DISP_CC].MemMapped=TRUE;
#endif
  ClockDriverContext Context={.ClockControllers=C,.ClockControllerCount=ARRAY_SIZE(C)};
  ClockDriverContext *Core=&Context;UINTN R=Reads,W=Writes;
  assert(ClockLibInit(&Core)==CR_SUCCESS&&Context.LockInitialized&&Reads==R&&Writes==W);
  for(UINTN I=1;I<CC_TYPE_DISP_CC;++I)assert(!C[I].MemMapped);
  assert(ClockDeinit(Core)==CR_SUCCESS&&Reads==R&&Writes==W);
#ifdef _KERNEL_MODE
  assert(Maps==0&&Unmaps==0&&C[0].MemMapped&&C[CC_TYPE_DISP_CC].MemMapped);
#else
  assert(Maps==2&&Unmaps==2&&!C[0].MemMapped&&!C[CC_TYPE_DISP_CC].MemMapped);
#endif
}
static VOID Partial(VOID) {
  for(UINT32 Kind=0;Kind<7;++Kind) {
    ClockController C[CC_TYPE_DISP_CC+1]={0};ClockNode *Nodes[]={NULL};
    C[0]=(ClockController){.Address=0x100000,.Size=0x1f0000,.MemMapped=TRUE};
    switch(Kind) {
      case 0:C[2].Address=0x200000;break;
      case 1:C[2].Size=0x1000;break;
      case 2:C[2].MemMapped=TRUE;break;
      case 3:C[2].Clks=Nodes;break;
      case 4:C[2].ClkCount=1;break;
      case 5:C[2].Gdscs=Nodes;break;
      default:C[2].GdscCount=1;break;
    }
    ClockDriverContext Context={.ClockControllers=C,.ClockControllerCount=ARRAY_SIZE(C)};
    ClockDriverContext *Core=&Context;UINTN M=Maps,U=Unmaps,R=Reads,W=Writes;
    assert(ClockLibInit(&Core)==CR_NOT_READY&&!Context.LockInitialized);
    assert(Maps==M&&Unmaps==U&&Reads==R&&Writes==W);
    assert(ClockDeinit(Core)==CR_NOT_READY&&Maps==M&&Unmaps==U);
  }
}
int main(void) {
  Sparse();Partial();
  puts("Clock controller holes: two real windows, no hole mapping, partial descriptors rejected before mapping/teardown PASS");
  return 0;
}
