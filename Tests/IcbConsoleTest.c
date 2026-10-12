/* Actual native ICC producer/core and SM8250 graph, modeled DAL/CmdDB/RPMh.
 * A transient QUP consumer must not retire the active debug UART's demand.
 * SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifndef CR_ICB_DXE_SOURCE
#define CR_ICB_DXE_SOURCE "../Driver/ICBCrDxe/ICBCrDxe.c"
#endif
#include CR_ICB_DXE_SOURCE
EFI_GUID gEfiCmdDBCrProtocolGuid;
EFI_GUID gEfiInterconnectCrProtocolGuid;
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
static EFI_TPL CurrentTpl=TPL_APPLICATION;
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return CurrentTpl;}
static EFI_TPL EFIAPI Raise(EFI_TPL T){assert(T>=CurrentTpl);EFI_TPL Old=CurrentTpl;CurrentTpl=T;return Old;}
static VOID EFIAPI Restore(EFI_TPL T){assert(T<=CurrentTpl);CurrentTpl=T;}
static CONST InterconnectTargetContext *Target;
static InterconnectTargetContext GenericTarget;
static CrDebugUartContext Uart={.BaseAddress=0xa90000,.IsDebugUart=TRUE,.Type=DEBUG_UART_TYPE_GENI,.BaudRate=115200};
static RpmhDeviceContext Rpmh;
static CR_DAL_DEVICE_INFO RpmhInfo;
static UINT32 Hardware[INTERCONNECT_MAX_BCMS];
static UINT32 Calls,FailCall,Published;
static BOOLEAN FailPublish,Generic;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L){*L=1;return L;}
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L){assert(*L==1);*L=2;return L;}
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L){assert(*L==2);*L=1;return L;}
static UINT16 Bcm(CONST CHAR8 *N){for(UINT16 I=0;I<Target->BcmCount;++I)if(!strcmp(Target->Bcms[I].Name,N))return I;assert(0);return 0;}
static EFI_STATUS EFIAPI Address(EFI_CMD_DB_PROTOCOL *P,CONST CHAR8 *N,UINT32 *A){(VOID)P;*A=0x100+4U*Bcm(N);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Aux(EFI_CMD_DB_PROTOCOL *P,CONST CHAR8 *N,UINT8 *D,UINT32 *S){(VOID)P;assert(*S>=8);UINT16 I=Bcm(N);memset(D,0,8);if(!Target->Bcms[I].EnableMask){D[0]=0xe8;D[1]=3;D[4]=1;}D[6]=(UINT8)(I%4);*S=8;return EFI_SUCCESS;}
static EFI_CMD_DB_PROTOCOL CmdDb={.GetEntryAddressByName=Address,.GetAuxDataByName=Aux};
CR_STATUS RpmhLibInit(RpmhDeviceContext **C){assert(*C==&Rpmh);Rpmh.NumCmdsPerTcs=4;return CR_SUCCESS;}
CR_STATUS RpmhWrite(RpmhDeviceContext *C,RpmhTcsCmd *Cmd,UINT32 N){assert(C==&Rpmh&&N&&N<=4);++Calls;if(Calls==FailCall)return CR_DEVICE_ERROR;for(UINT32 I=0;I<N;++I){UINT32 B=(Cmd[I].addr-0x100)/4;assert(B<Target->BcmCount);Hardware[B]=Cmd[I].data&0x0fffffff;}return CR_SUCCESS;}
EFI_STATUS CrDalGetDeviceInfo(CR_DAL_DEVICE_TYPE T,UINT32 I,UINT32 R,UINTN S,CONST CR_DAL_DEVICE_INFO **Out){assert(T==CrDalDeviceRpmh&&!I&&R==CR_DAL_DATA_REVISION_1&&S==sizeof(Rpmh));RpmhInfo.Data=&Rpmh;*Out=&RpmhInfo;return EFI_SUCCESS;}
CONST InterconnectTargetContext *CrDalGetInterconnectContext(VOID){return Generic?&GenericTarget:Target;}
CrDebugUartContext *CrDalGetDebugUartContext(VOID){return &Uart;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *K,VOID **Out){assert(G==&gEfiCmdDBCrProtocolGuid&&!K);*Out=&CmdDb;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Publish(EFI_HANDLE *H,...){(VOID)H;++Published;return FailPublish?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Unpublish(EFI_HANDLE H,...){(VOID)H;return EFI_SUCCESS;}
int main(int Argc,char **Argv){
 Target=CrTargetGetInterconnectContext();GenericTarget=*Target;GenericTarget.ProviderCount=1;
 Services.LocateProtocol=Locate;Services.InstallMultipleProtocolInterfaces=Publish;Services.UninstallMultipleProtocolInterfaces=Unpublish;
 Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;
 const char *Mode=Argc>1?Argv[1]:"normal";
 if(!strcmp(Mode,"generic"))Generic=TRUE;
 else if(!strcmp(Mode,"bad-uart"))Uart.BaseAddress=0x984000;
 else if(!strcmp(Mode,"publish-error"))FailPublish=TRUE;
 else assert(!strcmp(Mode,"normal"));
 EFI_STATUS S=InterconnectEntryPoint((EFI_HANDLE)1,NULL);
 if(!strcmp(Mode,"bad-uart")){assert(S==EFI_UNSUPPORTED&&!Published);puts("ICC console refuses mismatched UART identity before publication PASS");return 0;}
 if(Generic){assert(S==EFI_SUCCESS&&InterconnectUnload((EFI_HANDLE)1)==EFI_SUCCESS);puts("ICC unrelated target behavior preserved PASS");return 0;}
 assert(S==EFI_SUCCESS&&mInterconnect);
 UINT16 Q=Bcm("QUP0");
 if(FailPublish){assert(!mInitialized&&InterconnectUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED);assert(Hardware[Q]!=0);puts("ICC failed publication retains live console ownership PASS");return 0;}
 INTERCONNECT_PATH_HANDLE P=0;
 assert(mInterconnectProtocol.AcquirePath(&mInterconnectProtocol,"qcom,sm8250-qup1",0x5300,0x5301,&P)==EFI_SUCCESS);
 assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,P,2000,2000)==EFI_SUCCESS);
 assert(mInterconnectProtocol.ReleasePath(&mInterconnectProtocol,P)==EFI_SUCCESS);
 /* This assertion reproduces the UART resource gap in the old producer:
  * it withdraws the last transient SE vote while UART12 still uses QUP1. */
 assert(Hardware[Q]!=0&&mInterconnect->Bcm[Q].ProgrammedY!=0);
 UINT32 Old=Hardware[Q];
 assert(mInterconnectProtocol.AcquirePath(&mInterconnectProtocol,"qcom,sm8250-qup1",0x5300,0x5301,&P)==EFI_SUCCESS);
 assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,P,4000,4000)==EFI_SUCCESS);
 FailCall=Calls+1;assert(mInterconnectProtocol.ReleasePath(&mInterconnectProtocol,P)==EFI_DEVICE_ERROR);
 FailCall=0;assert(mInterconnectProtocol.ReleasePath(&mInterconnectProtocol,P)==EFI_SUCCESS&&Hardware[Q]==Old);
 assert(InterconnectUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED);
 puts("ICC actual producer: UART12 core/config retained across successful/failed transient QUP1 release PASS");return 0;
}
