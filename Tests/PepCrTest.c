/* Test actual Kodiak LPMD programs and failed provider transactions.
 * SPDX-License-Identifier: MIT */
#include <Library/PepCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PepFixture.h"
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI SetMem(VOID *P, UINTN N, UINT8 V) { return memset(P, V, N); }
#endif

typedef struct {
  UINT32 Validated, Applied, Begun, Committed, RolledBack, Balance, Counts[5];
  UINT32 RejectAt, FailAt;
  BOOLEAN FailCommit, FailRollback;
} IO_TEST;
STATIC CR_STATUS Validate(VOID *C, VOID *Owner, CONST CR_PEP_RESOURCE_OP *Op) {
  IO_TEST *T = C;
  assert(Owner != NULL && Op->Name.Length != 0);
  ++T->Validated;
  return T->RejectAt == T->Validated ? CR_UNSUPPORTED : CR_SUCCESS;
}
STATIC CR_STATUS Begin(VOID *C, VOID *Owner) {
  IO_TEST *T = C; assert(Owner != NULL); ++T->Begun; return CR_SUCCESS;
}
STATIC CR_STATUS Apply(VOID *C, VOID *Owner, CONST CR_PEP_RESOURCE_OP *Op) {
  IO_TEST *T = C; assert(Owner != NULL); ++T->Applied;
  if (T->FailAt == T->Applied) return CR_TIMEOUT;
  ++T->Balance; ++T->Counts[Op->Kind];
  return CR_SUCCESS;
}
STATIC CR_STATUS Commit(VOID *C, VOID *Owner) {
  IO_TEST *T = C; assert(Owner != NULL); ++T->Committed;
  return T->FailCommit ? CR_DEVICE_ERROR : CR_SUCCESS;
}
STATIC CR_STATUS Rollback(VOID *C, VOID *Owner) {
  IO_TEST *T = C; assert(Owner != NULL); ++T->RolledBack;
  if (T->FailRollback) return CR_NOT_READY;
  T->Balance = 0; return CR_SUCCESS;
}
STATIC VOID IdentityTests(VOID) {
  CR_PEP_VALUE Names[2][2] = {
    {{.Type=CrPepString,.String={"DEVICE",6}}, {.Type=CrPepString}},
    {{.Type=CrPepString,.String={"DEVICE",6}}, {.Type=CrPepString}}
  };
  CR_PEP_VALUE Devices[2] = {
    {.Type=CrPepPackage,.Package={Names[0],2}},
    {.Type=CrPepPackage,.Package={Names[1],2}}
  };
  CR_PEP_VALUE Root = {.Type=CrPepPackage,.Package={Devices,1}};
  CR_PEP_POLICY Policy; UINT32 Index;
  CONST CHAR8 *Aliases[] = {"\\_SB.A", "\\_SB_.A_", "\\_sB.a__", "\\_sb_.A___"};
  for(UINTN A=0;A<4;++A) {
    Names[0][1].String=(CR_PEP_TEXT){Aliases[A],strlen(Aliases[A])};
    Root.Package.Count=1;assert(CrPepPolicyInit(&Policy,&Root)==CR_SUCCESS);
    for(UINTN B=0;B<4;++B) {
      Index=99;
      assert(CrPepFindDevice(&Policy,(CR_PEP_TEXT){Aliases[B],strlen(Aliases[B])},&Index)==CR_SUCCESS&&Index==0);
      Names[1][1].String=(CR_PEP_TEXT){Aliases[B],strlen(Aliases[B])};
      Root.Package.Count=2;assert(CrPepPolicyInit(&Policy,&Root)==CR_INVALID_PARAMETER&&Policy.Root==NULL);
      Root.Package.Count=1;assert(CrPepPolicyInit(&Policy,&Root)==CR_SUCCESS);
    }
  }
  CONST CHAR8 *Different[] = {"\\_SB.AB", "\\_SB.B", "\\_SB.A.B", "\\_SB.A_B", "\\_SB.A1"};
  for(UINTN I=0;I<sizeof(Different)/sizeof(Different[0]);++I) {
    Index=99;assert(CrPepFindDevice(&Policy,(CR_PEP_TEXT){Different[I],strlen(Different[I])},&Index)==CR_NOT_FOUND&&Index==99);
  }
  CONST CHAR8 *Invalid[] = {"\\", "\\.A", "\\_SB.", "\\_SB..A", "\\_SB.AAAAA", "\\_SB.1A", "\\_SB.A-", "\\_SB.^A", "_SB.A", "\\_SB.A\x80"};
  for(UINTN I=0;I<sizeof(Invalid)/sizeof(Invalid[0]);++I) {
    Names[0][1].String=(CR_PEP_TEXT){Invalid[I],strlen(Invalid[I])};
    assert(CrPepPolicyInit(&Policy,&Root)==CR_INVALID_PARAMETER&&Policy.Root==NULL);
  }
  /* The two views have no trailing NUL. Comparison may not read beyond either. */
  CONST CHAR8 Short[]={'\\','A'}, Padded[]={'\\','A','_','_','_'};
  Names[0][1].String=(CR_PEP_TEXT){Short,sizeof(Short)};
  assert(CrPepPolicyInit(&Policy,&Root)==CR_SUCCESS);
  assert(CrPepFindDevice(&Policy,(CR_PEP_TEXT){Padded,sizeof(Padded)},&Index)==CR_SUCCESS);
}
int main(void) {
  IdentityTests();
  // Kodiak USB omits FOOTSWITCH mode, while display declares it. The parser
  // preserves either shape; only the qualified hardware provider chooses mode.
  CR_PEP_VALUE FsArgs[]={{.Type=CrPepString,.String={"gdsc",4}},
    {.Type=CrPepInteger,.Integer=1},{.Type=CrPepInteger,.Integer=1}};
  CR_PEP_VALUE FsRecord[]={{.Type=CrPepString,.String={"FOOTSWITCH",10}},
    {.Type=CrPepPackage,.Package={FsArgs,3}}};
  CR_PEP_VALUE Fs={.Type=CrPepPackage,.Package={FsRecord,2}};
  CR_PEP_RESOURCE_OP FsOp;
  assert(CrPepDecodeResource(&Fs,&FsOp)==CR_SUCCESS && FsOp.Kind==CrPepFootswitch &&
         FsOp.ArgumentCount==2 && FsOp.Arguments[1]==1);
  FsRecord[1].Package.Count=2;
  assert(CrPepDecodeResource(&Fs,&FsOp)==CR_SUCCESS && FsOp.ArgumentCount==1);
  FsRecord[1].Package.Count=3;FsArgs[2].Type=CrPepString;
  assert(CrPepDecodeResource(&Fs,&FsOp)==CR_INVALID_PARAMETER);
  CR_PEP_VALUE VregArgs[]={{.Type=CrPepString,.String={"PPP_RESOURCE_ID_LDO6_B",22}},
    {.Type=CrPepInteger,.Integer=1},{.Type=CrPepInteger,.Integer=1200000},
    {.Type=CrPepInteger,.Integer=1},{.Type=CrPepInteger,.Integer=7},{.Type=CrPepInteger,.Integer=0},
    {.Type=CrPepString,.String={"HLOS_DRV",8}},{.Type=CrPepString,.String={"REQUIRED",8}}};
  CR_PEP_VALUE VregRecord[]={{.Type=CrPepString,.String={"PMICVREGVOTE",12}},
    {.Type=CrPepPackage,.Package={VregArgs,8}}};
  CR_PEP_VALUE Vreg={.Type=CrPepPackage,.Package={VregRecord,2}};
  assert(CrPepDecodeResource(&Vreg,&FsOp)==CR_SUCCESS && FsOp.Kind==CrPepVreg &&
         FsOp.ArgumentCount==5 && FsOp.Arguments[1]==1200000 && FsOp.DriverName.Length==8 && FsOp.VoteType.Length==8);
  VregArgs[7].Type=CrPepInteger;assert(CrPepDecodeResource(&Vreg,&FsOp)==CR_INVALID_PARAMETER);
  CR_PEP_POLICY Policy;
  CR_PEP_RESULT Result;
  IO_TEST Test = {0};
  int Owner;
  UINT32 Index = 99, Abandon;
  CR_PEP_RESOURCE_IO Io = {&Test, Validate, Begin, Apply, Commit, Rollback};
  assert(CrPepPolicyInit(&Policy, &KodiakRoot) == CR_SUCCESS);
  assert(CrPepFindDevice(&Policy, (CR_PEP_TEXT){"\\_sb.urs0.usb0", 14}, &Index) == CR_SUCCESS);
  assert(CrPepFindDevice(&Policy, (CR_PEP_TEXT){"\\_SB_.URS0.USB0", 15}, &Index) == CR_SUCCESS);
  assert(Index == 1);
  assert(CrPepGetAbandonState(&Policy, Index, &Abandon) == CR_SUCCESS && Abandon == 3);
  assert(CrPepGetAbandonState(&Policy, 0, &Abandon) == CR_NOT_FOUND);
  for (UINT32 D = 0; D < KodiakRoot.Package.Count; ++D) {
    for (UINT32 S = 0; S < 4; ++S)
      assert(CrPepApplyState(&Policy, D, CrPepDState, 0, S, &Owner, &Io, &Result) == CR_SUCCESS);
    assert(CrPepApplyState(&Policy, D, CrPepFState, 0, 0, &Owner, &Io, &Result) == CR_SUCCESS);
  }
  assert(Test.Counts[CrPepClock] == 112 && Test.Counts[CrPepFootswitch] == 15);
  assert(Test.Counts[CrPepVreg] == 67 && Test.Counts[CrPepNpa] == 15);
  assert(Test.Counts[CrPepBusArb] == 30 && Test.Applied == 239);
  assert(Test.Validated == Test.Applied && Test.RolledBack == 0);
  CR_PEP_DEVICE_INFO DeviceInfo;
  CR_PEP_COMPONENT_INFO ComponentInfo;
  assert(CrPepGetDeviceInfo(&Policy, 1, &DeviceInfo) == CR_SUCCESS);
  assert(DeviceInfo.DStateMask == 15 && DeviceInfo.ComponentMask == 1 &&
         DeviceInfo.HasAbandonState && DeviceInfo.AbandonState == 3);
  assert(CrPepGetComponentInfo(&Policy, 1, 0, &ComponentInfo) == CR_SUCCESS &&
         ComponentInfo.FStateMask == 1 && ComponentInfo.PStateMask == 1);
  assert(CrPepGetComponentInfo(&Policy, 1, 1, &ComponentInfo) == CR_NOT_FOUND);
  assert(CrPepGetComponentInfo(&Policy, 1, 32, &ComponentInfo) == CR_INVALID_PARAMETER);
  assert(CrPepGetDeviceInfo(&Policy, 256, &DeviceInfo) == CR_INVALID_PARAMETER);
  memset(&Test, 0, sizeof(Test));
  for (UINT32 D = 0; D < KodiakRoot.Package.Count; ++D)
    assert(CrPepValidateDeviceResources(&Policy, D, &Owner, &Io) == CR_SUCCESS);
  assert(Test.Validated == 239 && Test.Begun == 0 && Test.Applied == 0);
  // Give F0 real operations: failure in its first operation must roll back
  // the already-applied D0 operations within the SAME transaction.
  CONST CR_PEP_VALUE *D0 = NULL;
  CONST CR_PEP_VALUE *Original = &KodiakRoot.Package.Values[1];
  for (UINTN I = 2; I < Original->Package.Count; ++I) {
    CONST CR_PEP_VALUE *V = &Original->Package.Values[I];
    if (V->Package.Values[0].String.Length == 6 &&
        !memcmp(V->Package.Values[0].String.Data, "DSTATE", 6) &&
        V->Package.Values[1].Integer == 0) D0 = V;
  }
  assert(D0 != NULL && D0->Package.Count > 2);
  CR_PEP_VALUE FValues[258];
  memcpy(FValues, D0->Package.Values, D0->Package.Count * sizeof(*FValues));
  FValues[0].String = (CR_PEP_TEXT){"FSTATE", 6};
  CR_PEP_VALUE CV[] = {
    {.Type=CrPepString,.String={"COMPONENT",9}}, {.Type=CrPepInteger,.Integer=0},
    {.Type=CrPepPackage,.Package={FValues,D0->Package.Count}}
  };
  CR_PEP_VALUE DV[] = {Original->Package.Values[0], Original->Package.Values[1], *D0,
    {.Type=CrPepPackage,.Package={CV,3}}};
  CR_PEP_VALUE Dev = {.Type=CrPepPackage,.Package={DV,4}};
  CR_PEP_VALUE Root = {.Type=CrPepPackage,.Package={&Dev,1}};
  CR_PEP_POLICY BatchPolicy;
  CR_PEP_TRANSITION Batch[] = {{CrPepDState,0,0}, {CrPepFState,0,0}};
  UINT32 Operations = (UINT32)D0->Package.Count - 2;
  assert(CrPepPolicyInit(&BatchPolicy, &Root) == CR_SUCCESS);
  memset(&Test, 0, sizeof(Test)); Test.FailAt = Operations + 1;
  assert(CrPepApplyStates(&BatchPolicy,0,Batch,2,&Owner,&Io,&Result) == CR_TIMEOUT);
  assert(Test.Validated == 2 * Operations && Test.Begun == 1 &&
         Result.Applied == Operations && Test.Committed == 0 && Test.RolledBack == 1 && Test.Balance == 0);
  memset(&Test, 0, sizeof(Test)); Test.RejectAt = Operations + 1;
  assert(CrPepApplyStates(&BatchPolicy,0,Batch,2,&Owner,&Io,&Result) == CR_UNSUPPORTED);
  assert(!Test.Begun && !Test.Applied && !Test.RolledBack);
  memset(&Test, 0, sizeof(Test));
  assert(CrPepApplyStates(&BatchPolicy,0,Batch,2,&Owner,&Io,&Result) == CR_SUCCESS &&
         Test.Begun == 1 && Test.Committed == 1 && Result.Applied == 2 * Operations);
  memset(&Test, 0, sizeof(Test)); Batch[1].State=1;
  assert(CrPepApplyStates(&BatchPolicy,0,Batch,2,&Owner,&Io,&Result) == CR_NOT_FOUND && !Test.Validated && !Test.Begun);
  Batch[1].State=0;Batch[1].Kind=(CR_PEP_STATE_KIND)-1;
  assert(CrPepValidateStates(&BatchPolicy,0,Batch,2,&Owner,&Io) == CR_INVALID_PARAMETER && !Test.Validated);
  assert(CrPepApplyStates(&BatchPolicy,0,Batch,0,&Owner,&Io,&Result) == CR_INVALID_PARAMETER);
  assert(CrPepApplyStates(&BatchPolicy,0,Batch,66,&Owner,&Io,&Result) == CR_INVALID_PARAMETER);
  CR_PEP_TRANSITION Maximum[65];
  for(UINT32 I=0;I<65;++I) Maximum[I]=(CR_PEP_TRANSITION){CrPepDState,0,0};
  memset(&Test,0,sizeof(Test));
  assert(CrPepApplyStates(&BatchPolicy,0,Maximum,65,&Owner,&Io,&Result) == CR_SUCCESS &&
         Test.Begun == 1 && Test.Committed == 1 && Result.Applied == 65 * Operations);
  for (UINT32 Version = 0; Version < 2; ++Version) {
    CONST UINT8 *Data = Version == 0 ? KodiakWireV1 : KodiakWireV2;
    UINTN Length = Version == 0 ? sizeof(KodiakWireV1) : sizeof(KodiakWireV2);
    UINTN Required = 0;
    CONST CR_PEP_VALUE *Output = NULL;
    assert(CrPepDecodeAcpiOutput(Data, Length, NULL, 0, &Required, &Output) == CR_BUFFER_TOO_SMALL);
    assert(Required > 500 && Output == NULL);
    CR_PEP_VALUE *Arena = calloc(Required, sizeof(*Arena)); assert(Arena != NULL);
    Arena[0].Type = CrPepInteger; Arena[0].Integer = 0xfeed;
    assert(CrPepDecodeAcpiOutput(Data, Length, Arena, Required - 1, &Required, &Output) == CR_BUFFER_TOO_SMALL);
    assert(Arena[0].Type == CrPepInteger && Arena[0].Integer == 0xfeed && Output == NULL);
    assert(CrPepDecodeAcpiOutput(Data, Length, Arena, Required, &Required, &Output) == CR_SUCCESS);
    assert(Output->Package.Count == 1);
    CR_PEP_POLICY WirePolicy;
    assert(CrPepPolicyInit(&WirePolicy, &Output->Package.Values[0]) == CR_SUCCESS);
    memset(&Test, 0, sizeof(Test));
    for (UINT32 D = 0; D < 5; ++D)
      for (UINT32 S = 0; S < 4; ++S)
        assert(CrPepApplyState(&WirePolicy, D, CrPepDState, 0, S, &Owner, &Io, &Result) == CR_SUCCESS);
    assert(Test.Applied == 239);
    // Every truncation must reject before publishing a partially decoded tree.
    for (UINTN Size = 0; Size < Length; ++Size) {
      assert(CR_ERROR(CrPepDecodeAcpiOutput(Data, Size, Arena, Required, &Required, &Output)));
      assert(Output == NULL);
    }
    UINT8 *Bad = malloc(Length); assert(Bad != NULL);
    memcpy(Bad, Data, Length); Bad[0] = 0;
    assert(CrPepDecodeAcpiOutput(Bad, Length, NULL, 0, &Required, &Output) == CR_DEVICE_ERROR);
    memcpy(Bad, Data, Length); memset(Bad + 8, 0, 4);
    assert(CrPepDecodeAcpiOutput(Bad, Length, NULL, 0, &Required, &Output) == CR_DEVICE_ERROR);
    memcpy(Bad, Data, Length);
    memset(Bad + 12 + (Version == 0 ? 2 : 4), 0xff, Version == 0 ? 2 : 4);
    assert(CrPepDecodeAcpiOutput(Bad, Length, NULL, 0, &Required, &Output) == CR_DEVICE_ERROR);
    free(Bad); free(Arena);
  }
  UINTN Needed = 0;
  CONST CR_PEP_VALUE *TooDeep = NULL;
  assert(CrPepDecodeAcpiOutput(DeepWire, sizeof(DeepWire), NULL, 0, &Needed, &TooDeep) == CR_DEVICE_ERROR);
  assert(TooDeep == NULL);
  for (UINT32 V=0; V<2; ++V) {
    CONST UINT8 *Data = V == 0 ? BufferWireV1 : BufferWireV2;
    UINTN Length = V == 0 ? sizeof(BufferWireV1) : sizeof(BufferWireV2), Required;
    CR_PEP_VALUE Arena[2]; CONST CR_PEP_VALUE *Output;
    assert(CrPepDecodeAcpiOutput(Data,Length,Arena,2,&Required,&Output) == CR_SUCCESS);
    assert(Required == 2 && Output->Package.Count == 1);
    CONST CR_PEP_VALUE *Buffer = Output->Package.Values;
    assert(Buffer->Type == CrPepBuffer && Buffer->Buffer.Length == 9);
    assert(Buffer->Buffer.Data >= Data && Buffer->Buffer.Data + 9 <= Data + Length);
    assert(Buffer->Buffer.Data[0] == 2 && Buffer->Buffer.Data[5] == 3);
    for (UINTN N=0; N<Length; ++N) {
      Arena[0].Type=CrPepInteger;Arena[0].Integer=0xfeed;
      assert(CR_ERROR(CrPepDecodeAcpiOutput(Data,N,Arena,2,&Required,&Output)));
      assert(Output == NULL && Arena[0].Integer == 0xfeed);
    }
    Data = V == 0 ? EmptyBufferWireV1 : EmptyBufferWireV2;
    Length = V == 0 ? sizeof(EmptyBufferWireV1) : sizeof(EmptyBufferWireV2);
    assert(CrPepDecodeAcpiOutput(Data,Length,Arena,2,&Required,&Output) == CR_SUCCESS);
    assert(Output->Package.Values->Type == CrPepBuffer && Output->Package.Values->Buffer.Length == 0);
  }
  memset(&Test, 0, sizeof(Test)); Test.RejectAt = 8;
  assert(CrPepApplyState(&Policy, 1, CrPepDState, 0, 0, &Owner, &Io, &Result) == CR_UNSUPPORTED);
  assert(Test.Begun == 0 && Test.Applied == 0 && Test.RolledBack == 0);
  memset(&Test, 0, sizeof(Test)); Test.FailAt = 4;
  assert(CrPepApplyState(&Policy, 1, CrPepDState, 0, 0, &Owner, &Io, &Result) == CR_TIMEOUT);
  assert(Result.Applied == 3 && Result.RollbackStatus == CR_SUCCESS);
  assert(Test.RolledBack == 1 && Test.Balance == 0 && Test.Committed == 0);
  memset(&Test, 0, sizeof(Test)); Test.FailAt = 4; Test.FailRollback = TRUE;
  assert(CrPepApplyState(&Policy, 1, CrPepDState, 0, 0, &Owner, &Io, &Result) == CR_TIMEOUT);
  assert(Result.RollbackStatus == CR_NOT_READY && Test.Balance == 3);
  memset(&Test, 0, sizeof(Test)); Test.FailCommit = TRUE;
  assert(CrPepApplyState(&Policy, 1, CrPepDState, 0, 0, &Owner, &Io, &Result) == CR_DEVICE_ERROR);
  assert(Test.Committed == 1 && Test.RolledBack == 1 && Test.Balance == 0);
  assert(CrPepApplyState(&Policy, 1, CrPepDState, 0, 4, &Owner, &Io, &Result) == CR_NOT_FOUND);
  assert(CrPepApplyState(&Policy, 1, CrPepFState, 9, 0, &Owner, &Io, &Result) == CR_NOT_FOUND);
  assert(CrPepApplyState(&Policy, 999, CrPepDState, 0, 0, &Owner, &Io, &Result) == CR_INVALID_PARAMETER);
  CR_PEP_VALUE Invalid = {.Type = CrPepPackage, .Package = {NULL, 257}};
  assert(CrPepPolicyInit(&Policy, &Invalid) == CR_INVALID_PARAMETER && Policy.Root == NULL);
  Invalid.Package.Count = 0;
  assert(CrPepPolicyInit(&Policy, &Invalid) == CR_SUCCESS);
  assert(CrPepFindDevice(&Policy, (CR_PEP_TEXT){"\\_SB.MISSING", 12}, &Index) == CR_NOT_FOUND);
  // Duplicate devices must be rejected before a policy can be published.
  CR_PEP_VALUE Duplicates[2] = {KodiakRoot.Package.Values[0], KodiakRoot.Package.Values[0]};
  Invalid.Package.Values = Duplicates; Invalid.Package.Count = 2;
  assert(CrPepPolicyInit(&Policy, &Invalid) == CR_INVALID_PARAMETER && Policy.Root == NULL);
  // Preserve 64-bit votes and raw Qualcomm opcodes/units at the ABI boundary.
  CR_PEP_VALUE Arguments[] = {
    {.Type = CrPepInteger, .Integer = 3},
    {.Type = CrPepString, .String = {"master", 6}},
    {.Type = CrPepString, .String = {"slave", 5}},
    {.Type = CrPepInteger, .Integer = 0x100000000ULL},
    {.Type = CrPepInteger, .Integer = 0x200000000ULL}
  };
  CR_PEP_VALUE Record[] = {
    {.Type = CrPepString, .String = {"BUSARB", 6}},
    {.Type = CrPepPackage, .Package = {Arguments, 5}}
  };
  CR_PEP_VALUE Resource = {.Type = CrPepPackage, .Package = {Record, 2}};
  CR_PEP_RESOURCE_OP Op;
  assert(CrPepDecodeResource(&Resource, &Op) == CR_SUCCESS);
  assert(Op.ArgumentCount == 3 && Op.Arguments[0] == 3);
  assert(Op.Arguments[1] == 0x100000000ULL && Op.Arguments[2] == 0x200000000ULL);
  Record[0].String = (CR_PEP_TEXT){"UNKNOWN", 7};
  assert(CrPepDecodeResource(&Resource, &Op) == CR_UNSUPPORTED);
  Record[0].String = (CR_PEP_TEXT){"CLOCK", 5};
  Record[1].Package.Count = 4;
  Arguments[0].Type = CrPepString;
  Arguments[0].String = (CR_PEP_TEXT){"gcc_ref", 7};
  Arguments[1].Type = CrPepInteger; Arguments[1].Integer = 3;
  Arguments[2].Type = CrPepInteger; Arguments[2].Integer = 9600;
  Arguments[3].Integer = 5;
  assert(CrPepDecodeResource(&Resource, &Op) == CR_SUCCESS);
  assert(Op.Arguments[0] == 3 && Op.Arguments[1] == 9600 && Op.Arguments[2] == 5);
  Record[1].Package.Count = 2;
  Arguments[1].Integer = 0;
  assert(CrPepDecodeResource(&Resource, &Op) == CR_UNSUPPORTED);
  Record[1].Package.Values = NULL;
  assert(CrPepDecodeResource(&Resource, &Op) == CR_INVALID_PARAMETER);
  puts("PEP: all five Kodiak devices/239 operations, preflight, commit failure and rollback retention passed");
  return 0;
}
