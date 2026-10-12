/* Observable reset/DMA model for the actual shared retirement core. GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/MdssRetireCrLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *Buffer,UINTN Bytes,UINT8 Value) {return memset(Buffer,Value,Bytes);}
#endif
typedef struct {
  UINT32 R[CrMdssRetireWindows][0x90000/4];
  UINT32 Calls,Fail,Resets,HaltEdges,Version,Soc,RejectOffset,ForceReadOnly,HaltBlocked;
  CR_MDSS_RETIRE_WINDOW RejectWindow;
  BOOLEAN Sealed,ResetOn,ExternalOn,DropReset,StaleAck;
} HW;
static CR_STATUS Step(HW *H) { return ++H->Calls==H->Fail?CR_DEVICE_ERROR:CR_SUCCESS; }
static CR_STATUS Ready(VOID *C,CR_MDSS_RETIRE_WINDOW W) { assert(W<CrMdssRetireWindows);return Step(C); }
static CR_STATUS Seal(VOID *C) {HW *H=C;H->Sealed=TRUE;return Step(H);}
static CR_STATUS Read(VOID *C,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 *V) {
  HW *H=C;assert(W<CrMdssRetireWindows&&!(O&3)&&O<0x90000);
  if(W==CrMdssRetireDpu){
    /* Public DP catalog ends at 0x280. Reading a DSI-only tear register
       beyond that range cannot be used as a stopped-route witness. */
    UINT32 First=H->Soc==CR_MDSS_SOC_SM8250?0x6a000:0x34000;
    UINT32 Stride=H->Soc==CR_MDSS_SOC_SM8250?0x800:0x1000;
    for(UINT32 I=0;I<4;I+=3)
      assert(O!=First+I*Stride+0x284&&O!=First+I*Stride+0x2b4);
    if(H->ResetOn)return CR_NOT_READY;
  }
  CR_STATUS S=Step(H);if(S==CR_SUCCESS)*V=H->R[W][O/4];return S;
}
static CR_STATUS Write(VOID *C,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 V) {
  HW *H=C;assert(H->Sealed&&W<CrMdssRetireWindows&&!(O&3)&&O<0x90000);
  CR_STATUS S=Step(H);if(S!=CR_SUCCESS)return S;
  if(H->RejectOffset==O&&H->RejectWindow==W)return CR_SUCCESS;
  H->R[W][O/4]=V;
  if(W==CrMdssRetireDpu&&O==0x2bc)H->R[W][O/4]&=~H->ForceReadOnly;
  if(W==CrMdssRetireDsi0||W==CrMdssRetireDsi1)
    if(O==0x118&&V){H->R[W][4/4]&=~7U;H->R[W][8/4]&=~6U;}
  if(W==CrMdssRetireVbif&&O==0x200&&!H->StaleAck){
    H->R[W][0x204/4]=V&~H->HaltBlocked;if(V&0x33f3)++H->HaltEdges;
  }
  return CR_SUCCESS;
}
static CR_STATUS Fence(VOID *C){return Step(C);}
static CR_STATUS Delay(VOID *C,UINT32 Us){assert(Us&&Us<=20000);return Step(C);}
static CR_STATUS ResetCore(VOID *C,BOOLEAN On) {
  HW *H=C;assert(H->Sealed);CR_STATUS S=Step(H);if(S!=CR_SUCCESS)return S;
  if(H->DropReset)return CR_DEVICE_ERROR;
  H->ResetOn=On;++H->Resets;
  if(On){memset(H->R[CrMdssRetireDpu],0,sizeof(H->R[0]));H->R[CrMdssRetireDpu][0]=H->Version;}
  return CR_SUCCESS;
}
static CR_STATUS Released(VOID *C){HW *H=C;CR_STATUS S=Step(H);return S==CR_SUCCESS&&H->ResetOn?CR_NOT_READY:S;}
static CR_STATUS ClockOff(VOID *C){HW *H=C;CR_STATUS S=Step(H);if(S==CR_SUCCESS)H->ExternalOn=FALSE;return S;}
static CR_STATUS ClockCheck(VOID *C){HW *H=C;CR_STATUS S=Step(H);return S==CR_SUCCESS&&H->ExternalOn?CR_NOT_READY:S;}
static VOID Setup(HW *H,CR_MDSS_RETIRE *R,UINT32 Soc) {
  memset(H,0,sizeof(*H));memset(R,0,sizeof(*R));H->ExternalOn=TRUE;
  H->Soc=Soc;
  H->Version=Soc==CR_MDSS_SOC_SM8250?0x60000000:0x70000000;H->R[CrMdssRetireDpu][0]=H->Version;
  for(UINT32 I=CrMdssRetireDsi0;I<=CrMdssRetireDsi1;++I){H->R[I][1]=7;H->R[I][2]=6;}
  for(UINT32 I=CrMdssRetirePhy0;I<=CrMdssRetirePhy1;++I){
    H->R[I][0x3c/4]=1;H->R[I][0x24/4]=0x7f;H->R[I][0x14/4]=0x20;H->R[I][0x1c/4]=0xff;
  }
  CR_MDSS_RETIRE_IO Io={H,Soc,Ready,Seal,Read,Write,Fence,Delay,ResetCore,Released,ClockOff,ClockCheck};
  assert(CrMdssRetireInit(R,&Io)==CR_SUCCESS&&H->Calls==0);
}
int main(VOID) {
  HW *H=calloc(1,sizeof(*H));assert(H);CR_MDSS_RETIRE R;UINT32 Cases=0;
  const UINT32 Socs[]={CR_MDSS_SOC_SM8250,CR_MDSS_SOC_SM8350};
  for(UINT32 I=0;I<2;++I){
    Setup(H,&R,Socs[I]);H->ForceReadOnly=0x100;
    assert((CrMdssRetireStart(&R)==CR_SUCCESS)&&"force request readback is not the VBIF halt completion");
    assert(R.Retired&&H->HaltEdges==1);
    assert(CrMdssRetireResume(&R)==CR_SUCCESS);
    Setup(H,&R,Socs[I]);H->ForceReadOnly=0x100;H->HaltBlocked=1U<<9;
    assert(CrMdssRetireStart(&R)==CR_TIMEOUT&&!R.Retired&&R.Stage==4&&H->ExternalOn);
    assert(R.ReadbackFailed&&R.FailureWindow==CrMdssRetireVbif&&R.FailureOffset==0x204);
    H->HaltBlocked=0;assert(CrMdssRetireStart(&R)==CR_SUCCESS);
    /* Real hardware may refuse a force/request bit despite successful bus
       writes. The exact refusal must retain the owner and be retryable. */
    const UINT32 Offsets[]={0x2ac,0x2b4,0x2bc,0x2c4,0x200};
    const UINT32 Masks[]={0x101,0x101,0x110101,0x101,0x33f3};
    for(UINT32 J=4;J<5;++J){
      Setup(H,&R,Socs[I]);H->RejectOffset=Offsets[J];
      H->RejectWindow=J==4?CrMdssRetireVbif:CrMdssRetireDpu;
      assert(CrMdssRetireStart(&R)==CR_DEVICE_ERROR&&R.Stage==4&&!R.Retired&&R.ForceSaved);
      assert(R.ReadbackFailed&&R.FailureWindow==H->RejectWindow&&
        R.FailureOffset==Offsets[J]&&R.FailureMask==Masks[J]&&
        R.FailureExpected==Masks[J]&&R.FailureActual==0);
      assert(H->ExternalOn&&CrMdssRetireResume(&R)==CR_NOT_READY);
      H->RejectOffset=0;
      assert(CrMdssRetireStart(&R)==CR_SUCCESS&&!R.ReadbackFailed);
      assert(CrMdssRetireResume(&R)==CR_SUCCESS&&!R.ReadbackFailed);
    }
    Setup(H,&R,Socs[I]);assert(CrMdssRetireStart(&R)==CR_SUCCESS);
    assert(H->Sealed&&H->Resets==2&&H->HaltEdges==1&&!H->ExternalOn&&R.Retired);
    UINT32 Calls=H->Calls;assert(CrMdssRetireCheck(&R)==CR_SUCCESS);UINT32 CheckCalls=H->Calls-Calls;
    H->R[CrMdssRetireDsi1][1]=1;assert(CrMdssRetireCheck(&R)==CR_NOT_READY);
    H->R[CrMdssRetireDsi1][1]=0;assert(CrMdssRetireCheck(&R)==CR_SUCCESS);
    UINT32 Ctl=Socs[I]==CR_MDSS_SOC_SM8250?0x1000:0x15000;
    H->R[CrMdssRetireDpu][(Ctl+0x1c)/4]=1;
    assert(CrMdssRetireCheck(&R)==CR_NOT_READY);
    H->R[CrMdssRetireDpu][(Ctl+0x1c)/4]=0;
    assert(CrMdssRetireCheck(&R)==CR_SUCCESS);
    assert(CrMdssRetireResume(&R)==CR_SUCCESS&&R.Resumed);
    assert(!(H->R[CrMdssRetireVbif][0x200/4]&0x33f3));
    assert(CrMdssRetireCheck(&R)==CR_NOT_READY);
    UINT32 WritesBefore=H->Resets;assert(CrMdssRetireStart(&R)==CR_BUSY&&H->Resets==WritesBefore);
    /* Failure after clearing the halt must retain a separate resume journal.
       Requiring the old halted condition again strands the caller forever. */
    Setup(H,&R,Socs[I]);assert(CrMdssRetireStart(&R)==CR_SUCCESS);
    UINT32 ResumeBefore=H->Calls;
    assert(CrMdssRetireResume(&R)==CR_SUCCESS);
    UINT32 ResumeCalls=H->Calls-ResumeBefore;
    for(UINT32 Fail=1;Fail<=ResumeCalls;++Fail){
      Setup(H,&R,Socs[I]);assert(CrMdssRetireStart(&R)==CR_SUCCESS);
      H->Fail=H->Calls+Fail;
      assert(CrMdssRetireResume(&R)!=CR_SUCCESS&&!R.Resumed);
      H->Fail=0;
      assert((CrMdssRetireResume(&R)==CR_SUCCESS&&R.Resumed)&&"resume retry");
      ++Cases;
    }
    for(UINT32 Fail=1;Fail<=Calls;++Fail){Setup(H,&R,Socs[I]);H->Fail=Fail;
      assert(CrMdssRetireStart(&R)!=CR_SUCCESS&&!R.Retired);
      H->Fail=0;assert(CrMdssRetireStart(&R)==CR_SUCCESS);assert(CrMdssRetireCheck(&R)==CR_SUCCESS);++Cases;
    }
    for(UINT32 Fail=1;Fail<=CheckCalls;++Fail){Setup(H,&R,Socs[I]);assert(CrMdssRetireStart(&R)==CR_SUCCESS);
      H->Fail=H->Calls+Fail;assert(CrMdssRetireCheck(&R)!=CR_SUCCESS);++Cases;
    }
    Setup(H,&R,Socs[I]);H->StaleAck=TRUE;H->R[CrMdssRetireVbif][0x204/4]=0x33f3;
    assert(CrMdssRetireStart(&R)==CR_TIMEOUT&&!R.Retired&&H->ExternalOn);
    assert(R.ReadbackFailed&&R.FailureWindow==CrMdssRetireVbif&&
      R.FailureOffset==0x204&&R.FailureMask==0x33f3&&
      R.FailureExpected==0&&R.FailureActual==0x33f3);
    Setup(H,&R,Socs[I]);H->DropReset=TRUE;
    assert(CrMdssRetireStart(&R)!=CR_SUCCESS&&!R.Retired&&H->ExternalOn);
  }
  free(H);printf("Shared MDSS reset/host/PHY/DMA retirement: two SoCs, stale ACK rejection, %u retained failure boundaries PASS\n",Cases);return 0;
}
