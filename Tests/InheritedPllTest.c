/* Shared inherited direct PLL observation, without analogue/vote effects. MIT */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#endif
#include <Library/FixedPllVoteLib.h>
static UINT32 Values[5];
static UINTN Reads,Writes;
static INT32 Drift=-1;
#ifndef _KERNEL_MODE
static UINT32 DiagnosticMask,DiagnosticCalls,DriftDiagnostics;
#endif
static UINT32 Read(UINTN A) {
  CONST UINT32 Offsets[]={0,4,0x40,0x0c,0x38};
  for(UINT32 I=0;I<5;++I)if(A==0x100000U+Offsets[I]) {
    ++Reads;return Values[I]^((INT32)I==Drift && Reads>5?BIT(10):0);
  }
  assert(0);return 0;
}
#ifdef _KERNEL_MODE
UINT32 CrMmioRead32(UINTN A){return Read(A);}
UINT32 CrMmioWrite32(UINTN A,UINT32 V){(void)A;++Writes;return V;}
UINT32 CrMmioUpdateBits32(UINTN A,UINT32 M,UINT32 V){(void)A;(void)M;++Writes;return V;}
#else
UINT32 EFIAPI MmioRead32(UINTN A){return Read(A);}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){(void)A;++Writes;return V;}
VOID EFIAPI MemoryFence(VOID){}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){
  (void)Level;
  if(strstr(Format,"Inherited PLL base=")){
    va_list Args;va_start(Args,Format);
    assert(va_arg(Args,UINT64)==0x100000);
    assert(va_arg(Args,UINT64)==600000000&&va_arg(Args,UINT64)==600000000);
    DiagnosticMask=va_arg(Args,UINT32);assert(va_arg(Args,UINT32)<=1);
    for(UINT32 I=0;I<5;++I)assert(va_arg(Args,UINT32)==Values[I]);
    assert(va_arg(Args,UINT32)==(Values[3]&1U));
    va_end(Args);++DiagnosticCalls;
  }else if(strstr(Format,"Inherited PLL second snapshot"))++DriftDiagnostics;
}
#endif
static VOID Reset(VOID) {
  Values[0]=BIT(31)|BIT(2)|BIT(0);Values[1]=31;Values[2]=0x4000;Values[3]=7;Values[4]=1;
  Reads=Writes=0;Drift=-1;
#ifndef _KERNEL_MODE
  DiagnosticMask=DiagnosticCalls=DriftDiagnostics=0;
#endif
}
int main(void) {
  for(UINT32 Soc=0;Soc<2;++Soc) {
    CR_INHERITED_PLL_DESCRIPTION D={.Base=0x100000,.Size=0x44,.ExpectedL=31,.ExpectedAlpha=0x4000,
      .ParentHz=19200000,.FrequencyHz=600000000,.FsmInModeRegister=Soc==0};
    Reset();assert(CrInheritedPllCheck(&D,600000000)==CR_SUCCESS&&Reads==10&&!Writes);
    for(UINT32 Outputs=1;Outputs<8;Outputs+=2){
      Reset();Values[3]=Outputs|BIT(17);
      assert(CrInheritedPllCheck(&D,600000000)==CR_SUCCESS&&Reads==10&&!Writes);
    }
    Reset();Values[3]=6;assert(CrInheritedPllCheck(&D,600000000)==CR_NOT_READY&&!Writes);
#ifndef _KERNEL_MODE
    assert(DiagnosticCalls==1&&DiagnosticMask==BIT(4)&&!DriftDiagnostics);
#endif
    Reset();assert(CrInheritedPllCheck(&D,300000000)==CR_UNSUPPORTED&&!Reads&&!Writes);
    for(UINT32 I=0;I<5;++I) {
      Reset();Values[I]=0;assert(CrInheritedPllCheck(&D,600000000)==CR_NOT_READY&&!Writes);
      Reset();Drift=I;assert(CrInheritedPllCheck(&D,600000000)==CR_NOT_READY&&Reads==10&&!Writes);
#ifndef _KERNEL_MODE
      assert(DiagnosticCalls==1&&DiagnosticMask==BIT(6)&&DriftDiagnostics==1);
#endif
    }
    Reset();Values[Soc?3:0]|=Soc?BIT(21):BIT(20);
    assert(CrInheritedPllCheck(&D,600000000)==CR_NOT_READY&&!Writes);
    Reset();D.FrequencyHz=599999999;assert(CrInheritedPllCheck(&D,600000000)==CR_INVALID_PARAMETER&&!Reads&&!Writes);
    D.FrequencyHz=600000000;D.Size=0x43;
    assert(CrInheritedPllCheck(&D,600000000)==CR_INVALID_PARAMETER&&!Reads&&!Writes);
  }
  puts("Inherited direct PLL: stable two-snapshot authority, all register drift, FSM/config/rate/window refusal and no writes PASS");
  return 0;
}
