/* Native adapter for the shared PAS lifecycle. SPDX-License-Identifier: MIT */
#include <Library/PILCrUefiLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define SMEM(P) ((EFI_SMEM_CR_PROTOCOL *)(P)->NativeAdapter[0])
#define CMDDB(P) ((EFI_CMD_DB_PROTOCOL *)(P)->NativeAdapter[1])
#define RPMH(P) ((EFI_RPMH_CR_PROTOCOL *)(P)->NativeAdapter[2])
#define ICB(P) ((EFI_INTERCONNECT_CR_PROTOCOL *)(P)->NativeAdapter[3])

STATIC CR_STATUS SecureCall(VOID *Context, CR_PIL_SMC_ARGS *Frame) {
  ARM_SMC_ARGS Native;
  CR_PIL_CONTEXT *Pil = Context;
  UINTN CallerEl = ArmReadCurrentEL() >> 2;
  ZeroMem(&Native,sizeof(Native));
  Native.Arg0=(UINTN)Frame->X[0]; Native.Arg1=(UINTN)Frame->X[1];
  Native.Arg2=(UINTN)Frame->X[2]; Native.Arg3=(UINTN)Frame->X[3];
  Native.Arg4=(UINTN)Frame->X[4]; Native.Arg5=(UINTN)Frame->X[5];
  Native.Arg6=(UINTN)Frame->X[6]; Native.Arg7=(UINTN)Frame->X[7];
  ArmCallSmc(&Native);
  if (Pil->PasOwned && Pil->StartGeneration != 0 &&
      (Pil->SmcOriginal.X[0] == 0x42000206U || Pil->SmcOriginal.X[0] == 0x02000206U) &&
      Native.Arg0 == 0 && Native.Arg1 == 0 && (CallerEl == 1 || CallerEl == 2)) {
    Pil->NativeShutdownGeneration = Pil->StartGeneration;
    Pil->NativeShutdownEl = (UINT32)CallerEl;
  }
  Frame->X[0]=Native.Arg0; Frame->X[1]=Native.Arg1;
  Frame->X[2]=Native.Arg2; Frame->X[3]=Native.Arg3;
  Frame->X[4]=Native.Arg4; Frame->X[5]=Native.Arg5;
  Frame->X[6]=Native.Arg6; Frame->X[7]=Native.Arg7;
  return CR_SUCCESS;
}
STATIC UINT32 Read32(VOID *Context,UINT32 Address) {
  (VOID)Context;return MmioRead32(Address);
}
STATIC VOID Write32(VOID *Context,UINT32 Address,UINT32 Value) {
  (VOID)Context;MmioWrite32(Address,Value);
}
STATIC VOID Fence(VOID *Context) {(VOID)Context;ArmDataSynchronizationBarrier();}
STATIC VOID Delay(VOID *Context,UINT32 Us) {(VOID)Context;MicroSecondDelay(Us);}
STATIC CR_STATUS Get(VOID *Context,UINT32 Host,UINT32 Item,UINT32 *Size,VOID **Address) {
  CR_PIL_CONTEXT *Pil=Context;return SMEM(Pil)->GetItem(SMEM(Pil),Host,Item,Size,Address);
}
STATIC CR_STATUS Allocate(VOID *Context,UINT32 Host,UINT32 Item,UINT32 Size,VOID **Address) {
  CR_PIL_CONTEXT *Pil=Context;return SMEM(Pil)->AllocateItem(SMEM(Pil),Host,Item,Size,Address);
}
STATIC CR_STATUS Address(VOID *Context,CONST CHAR8 *Name,UINT32 *Value) {
  CR_PIL_CONTEXT *Pil=Context;return CMDDB(Pil)->GetEntryAddressByName(CMDDB(Pil),Name,Value);
}
STATIC CR_STATUS Aux(VOID *Context,CONST CHAR8 *Name,VOID *Data,UINT32 *Size) {
  CR_PIL_CONTEXT *Pil=Context;return CMDDB(Pil)->GetAuxDataByName(CMDDB(Pil),Name,Data,Size);
}
STATIC CR_STATUS Proxy(VOID *Context,UINT32 Address,UINT32 Value) {
  CR_PIL_CONTEXT *Pil=Context;
  return RPMH(Pil)->VoteArc(RPMH(Pil),Pil,Address,Value);
}
STATIC CR_STATUS CryptoAcquire(VOID *Context,CONST CHAR8 *Name,UINT32 Source,UINT32 Destination,UINT64 *Handle) {
  CR_PIL_CONTEXT *Pil=Context;
  INTERCONNECT_PATH_HANDLE Native=0;
  CR_STATUS Status=ICB(Pil)->AcquirePath(ICB(Pil),Name,Source,Destination,&Native);
  *Handle=(UINT64)Native;return Status;
}
STATIC CR_STATUS CryptoSet(VOID *Context,UINT64 Handle,UINT64 Average,UINT64 Peak) {
  CR_PIL_CONTEXT *Pil=Context;
  return ICB(Pil)->SetBandwidth(ICB(Pil),(INTERCONNECT_PATH_HANDLE)Handle,Average,Peak);
}
STATIC CR_STATUS CryptoRelease(VOID *Context,UINT64 Handle) {
  CR_PIL_CONTEXT *Pil=Context;
  return ICB(Pil)->ReleasePath(ICB(Pil),(INTERCONNECT_PATH_HANDLE)Handle);
}
CR_STATUS CrPilInitialize(CR_PIL_CONTEXT *Pil,CONST CR_PIL_TARGET *Target,
                           EFI_SMEM_CR_PROTOCOL *Smem,EFI_CMD_DB_PROTOCOL *CmdDb,
                           EFI_RPMH_CR_PROTOCOL *Rpmh,EFI_INTERCONNECT_CR_PROTOCOL *Interconnect) {
  if (Smem==NULL || CmdDb==NULL || Rpmh==NULL || Interconnect==NULL ||
      Smem->GetItem==NULL || Smem->AllocateItem==NULL ||
      CmdDb->GetEntryAddressByName==NULL || CmdDb->GetAuxDataByName==NULL ||
      Rpmh->Revision<EFI_RPMH_CR_PROTOCOL_REVISION || Rpmh->VoteArc==NULL || Interconnect->AcquirePath==NULL ||
      Interconnect->SetBandwidth==NULL || Interconnect->ReleasePath==NULL)
    return CR_INVALID_PARAMETER;
  CR_PIL_IO Io={Pil,SecureCall,Read32,Write32,Fence,Delay,Get,Allocate,Address,Aux,
                Proxy,CryptoAcquire,CryptoSet,CryptoRelease};
  CR_STATUS Status=CrPilCoreInitialize(Pil,Target,&Io);
  if (CR_ERROR(Status)) return Status;
  Pil->NativeAdapter[0]=Smem;Pil->NativeAdapter[1]=CmdDb;
  Pil->NativeAdapter[2]=Rpmh;Pil->NativeAdapter[3]=Interconnect;
  return Status;
}
