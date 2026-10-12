/* Actual EFI adapter, pointer semantics and native I2C packets. MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Driver/FocalTechTouchCrDxe/FocalTechTouchCrDxe.c"
EFI_GUID gEfiAbsolutePointerProtocolGuid,gEfiDevicePathProtocolGuid,gEfiLoadedImageProtocolGuid;
EFI_GUID gEfiI2cCrProtocolGuid,gEfiGpioCrProtocolGuid,gEfiRpmhCrProtocolGuid,gEfiEventExitBootServicesGuid;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
static BOOLEAN ReenterAllocation;
static EFI_STATUS RecursiveEntryStatus;
VOID *EFIAPI AllocateZeroPool(UINTN N){if(ReenterAllocation){ReenterAllocation=FALSE;RecursiveEntryStatus=FocalTechTouchCrEntry((EFI_HANDLE)1,NULL);}return calloc(1,N);}
VOID EFIAPI FreePool(VOID *P){free(P);}
static UINT32 DiagnosticIds,DiagnosticReports,DiagnosticLines,DiagnosticQualifications,DiagnosticValidations;
static UINTN DiagnosticMask;
static EFI_STATUS DiagnosticQualification,DiagnosticValidation;
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *Format,...){
 if(!(L&DiagnosticMask))return;
 if(strstr(Format," ID reg="))++DiagnosticIds;
 if(strstr(Format,"first report reg="))++DiagnosticReports;
 if(strstr(Format,"report +"))++DiagnosticLines;
 va_list A;va_start(A,Format);
 if(strstr(Format,"firmware ID qualification=")){++DiagnosticQualifications;DiagnosticQualification=va_arg(A,EFI_STATUS);}
 if(strstr(Format,"first report validation=")){++DiagnosticValidations;DiagnosticValidation=va_arg(A,EFI_STATUS);}
 va_end(A);
}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){return (L&DiagnosticMask)!=0;}
UINTN EFIAPI MicroSecondDelay(UINTN N){assert(N==1000||N==100000||N==200000);return N;}
static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static EFI_I2C_CR_PROTOCOL Bus;
static EFI_GPIO_CR_PROTOCOL Gpio;
static EFI_RPMH_CR_PROTOCOL Rpmh;
static UINT8 ReportData[33];
static struct {UINTN Reads,Signals,Creates,Closes,Open,CloseBus,Installs,Uninstalls,FailCreate,Raises;BOOLEAN Pins[2],Rails[2],Published,FailInstall,PartialInstall,FailRailRelease,FailUninstall,FailTimer,FailClose,FailCancel,BadId,BadRate,ExitAtFrequency,ExitAtRequest,ExitAtInstall,UnloadAtRequest,EdgePending,FailArm;EFI_STATUS ReenterStatus,IdReadStatus,ReportReadStatus,EdgeStatus;EFI_TPL Tpl;} F;
typedef struct {EFI_EVENT_NOTIFY Notify;VOID *Context;EFI_TPL Tpl;BOOLEAN Armed;} TEST_EVENT;
static EFI_STATUS EFIAPI Frequency(EFI_I2C_CR_PROTOCOL *T,UINTN *Hz){assert(T==&Bus&&*Hz==100000);if(F.BadRate)*Hz=400000;if(F.ExitAtFrequency){F.ExitAtFrequency=FALSE;ExitBoot(mFts->Exit,mFts);}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Request(EFI_I2C_CR_PROTOCOL *T,UINTN A,EFI_I2C_REQUEST_PACKET *P,EFI_EVENT E,EFI_STATUS *S){
 assert(T==&Bus&&A==0x38&&P->OperationCount==2&&!E&&!S&&P->Operation[0].LengthInBytes==1&&P->Operation[0].Flags==0&&P->Operation[1].Flags==I2C_FLAG_READ&&F.Tpl<=TPL_CALLBACK);
 ++F.Reads;if(F.UnloadAtRequest){F.UnloadAtRequest=FALSE;F.ReenterStatus=FocalTechTouchCrUnload((EFI_HANDLE)1);assert(F.ReenterStatus==EFI_ACCESS_DENIED&&mFts&&F.Pins[0]&&F.Rails[0]);}UINT8 R=*P->Operation[0].Buffer;UINTN N=P->Operation[1].LengthInBytes;
 if(F.ExitAtRequest){F.ExitAtRequest=FALSE;ExitBoot(mFts->Exit,mFts);assert(F.Pins[0]&&F.Rails[0]);}
 if(R==0xa3||R==0x9f){assert(N==1);if(F.IdReadStatus!=EFI_SUCCESS)return F.IdReadStatus;*P->Operation[1].Buffer=F.BadId?0:(R==0xa3?0x54:0x52);}else{assert(R==1&&N==32);if(F.ReportReadStatus!=EFI_SUCCESS)return F.ReportReadStatus;memcpy(P->Operation[1].Buffer,ReportData+1,32);}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI PinAcquire(EFI_GPIO_CR_PROTOCOL *T,VOID *O,CONST GpioConfigParams *D,BOOLEAN *Owned){assert(T==&Gpio&&O&&(D->PinNumber==38||D->PinNumber==39));F.Pins[D->PinNumber-38]=TRUE;*Owned=TRUE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI PinSet(EFI_GPIO_CR_PROTOCOL *T,VOID *O,UINT16 N,UINT8 V){assert(T==&Gpio&&O&&N==38&&V<2&&F.Pins[0]);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI ArmEdge(EFI_GPIO_CR_PROTOCOL *T,VOID *O,UINT16 N){assert(T==&Gpio&&O==&mFts->Touch&&N==39&&F.Pins[1]);F.EdgePending=FALSE;return F.FailArm?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI TakeEdge(EFI_GPIO_CR_PROTOCOL *T,VOID *O,UINT16 N,BOOLEAN *Pending){assert(T==&Gpio&&O==&mFts->Touch&&N==39&&F.Pins[1]);*Pending=F.EdgePending;F.EdgePending=FALSE;return F.EdgeStatus;}
static EFI_STATUS EFIAPI PinRelease(EFI_GPIO_CR_PROTOCOL *T,VOID *O,UINT16 N){assert(T==&Gpio&&O&&F.Pins[N-38]);F.Pins[N-38]=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI RailAcquire(EFI_RPMH_CR_PROTOCOL *T,CONST CHAR8 *N,UINT32 M,UINT8 Mode,BOOLEAN *Owned){assert(T==&Rpmh&&N&&Mode==7&&(M==3000||M==1800));F.Rails[M==3000?0:1]=TRUE;*Owned=TRUE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI RailRelease(EFI_RPMH_CR_PROTOCOL *T,CONST CHAR8 *N,BOOLEAN V){assert(T==&Rpmh&&!V&&N);if(F.FailRailRelease)return EFI_DEVICE_ERROR;UINT32 I=!strcmp(N,"ldoa13")?0:1;assert(F.Rails[I]);F.Rails[I]=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **I){assert(!R);if(G==&gEfiGpioCrProtocolGuid)*I=&Gpio;else{assert(G==&gEfiRpmhCrProtocolGuid);*I=&Rpmh;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **H){assert(T==ByProtocol&&G==&gEfiI2cCrProtocolGuid&&!K);*N=1;*H=AllocateZeroPool(sizeof(**H));(*H)[0]=(EFI_HANDLE)2;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **I){if(G==&gEfiI2cCrProtocolGuid){assert(H==(EFI_HANDLE)2);*I=&Bus;}else{assert(H==(EFI_HANDLE)1&&G==&gEfiLoadedImageProtocolGuid);*I=&Loaded;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **I,EFI_HANDLE A,EFI_HANDLE C,UINT32 Attr){assert(H==(EFI_HANDLE)2&&G==&gEfiI2cCrProtocolGuid&&A==(EFI_HANDLE)1&&C==H&&Attr==EFI_OPEN_PROTOCOL_BY_DRIVER);*I=&Bus;++F.Open;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI CloseBus(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE A,EFI_HANDLE C){assert(H==(EFI_HANDLE)2&&G==&gEfiI2cCrProtocolGuid&&A==(EFI_HANDLE)1&&C==H);++F.CloseBus;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY N,VOID *C,EFI_EVENT *Out){assert(N&&C);if(F.FailCreate==F.Creates+1){F.FailCreate=0;return EFI_OUT_OF_RESOURCES;}TEST_EVENT *E=calloc(1,sizeof(*E));E->Notify=N;E->Context=C;E->Tpl=T;*Out=E;++F.Creates;if(Type&EVT_TIMER)assert(T==TPL_CALLBACK);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI EventEx(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){assert(G==&gEfiEventExitBootServicesGuid);return Event(Type,T,N,(VOID *)C,Out);}
static EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY T,UINT64 N){TEST_EVENT *V=E;assert(V);if(T==TimerPeriodic){assert(N==160000);if(F.FailTimer)return EFI_DEVICE_ERROR;V->Armed=TRUE;}else{assert(T==TimerCancel&&N==0);if(F.FailCancel)return EFI_DEVICE_ERROR;V->Armed=FALSE;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Signal(EFI_EVENT E){assert(E);++F.Signals;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(E);if(F.FailClose)return EFI_DEVICE_ERROR;++F.Closes;free(E);return EFI_SUCCESS;}
static EFI_TPL EFIAPI Raise(EFI_TPL T){++F.Raises;EFI_TPL Old=F.Tpl;F.Tpl=T;return Old;}
static VOID EFIAPI Restore(EFI_TPL T){F.Tpl=T;}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){++F.Installs;if(F.FailInstall)return EFI_DEVICE_ERROR;va_list A;va_start(A,H);assert(va_arg(A,EFI_GUID *)==&gEfiAbsolutePointerProtocolGuid);assert(va_arg(A,VOID *)==&mFts->Pointer);va_end(A);*H=(EFI_HANDLE)3;F.Published=TRUE;if(F.ExitAtInstall)ExitBoot(mFts->Exit,mFts);return F.PartialInstall?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...){assert(H==(EFI_HANDLE)3&&F.Published);++F.Uninstalls;if(F.FailUninstall)return EFI_ACCESS_DENIED;F.Published=FALSE;return EFI_SUCCESS;}
static EFI_BOOT_SERVICES Boot={.LocateProtocol=Locate,.LocateHandleBuffer=Handles,.HandleProtocol=Handle,.OpenProtocol=Open,.CloseProtocol=CloseBus,.CreateEvent=Event,.CreateEventEx=EventEx,.SetTimer=Timer,.SignalEvent=Signal,.CloseEvent=Close,.RaiseTPL=Raise,.RestoreTPL=Restore,.InstallMultipleProtocolInterfaces=Install,.UninstallMultipleProtocolInterfaces=Uninstall};
EFI_BOOT_SERVICES *gBS=&Boot;
static VOID Setup(void){assert(!mFts);DiagnosticIds=DiagnosticReports=DiagnosticLines=DiagnosticQualifications=DiagnosticValidations=0;DiagnosticQualification=DiagnosticValidation=EFI_SUCCESS;DiagnosticMask=DEBUG_WARN|DEBUG_ERROR;memset(&F,0,sizeof(F));memset(&Loaded,0,sizeof(Loaded));F.Tpl=TPL_APPLICATION;memset(ReportData,0xff,33);ReportData[0]=ReportData[1]=ReportData[2]=0;
 Bus=(EFI_I2C_CR_PROTOCOL){.Revision=EFI_I2C_CR_PROTOCOL_REVISION,.Instance=14,.BusFrequencyKHz=100,.SetBusFrequency=Frequency,.StartRequest=Request};
 Gpio=(EFI_GPIO_CR_PROTOCOL){.Revision=0x10003,.AcquirePin=PinAcquire,.SetPinValue=PinSet,.ReleasePin=PinRelease,.ArmPinFallingEdge=ArmEdge,.TakePinEdge=TakeEdge};
 Rpmh=(EFI_RPMH_CR_PROTOCOL){.Revision=EFI_RPMH_CR_PROTOCOL_REVISION,.AcquireVreg=RailAcquire,.RpmhEnableVreg=RailRelease};}
static VOID Tick(BOOLEAN Edge){F.EdgePending|=Edge;assert(mFts&&mFts->Poll);TEST_EVENT *E=mFts->Poll;EFI_TPL Old=F.Tpl;F.Tpl=E->Tpl;E->Notify(E,E->Context);F.Tpl=Old;}
static VOID VerifyClean(void){assert(!mFts&&!F.Published&&F.Closes==F.Creates&&F.Open==F.CloseBus);for(UINT32 I=0;I<2;++I)assert(!F.Pins[I]&&!F.Rails[I]);}
static VOID TestDisposeFrozen(void){UINTN Raises=F.Raises;EFI_ABSOLUTE_POINTER_STATE S;assert(mFts&&mFts->Exiting&&FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_NOT_READY);
 assert(GetState(&mFts->Pointer,&S)==EFI_NOT_READY&&Reset(&mFts->Pointer,FALSE)==EFI_NOT_READY&&F.Raises==Raises);
 /* Test harness resumes its modeled boot-services environment solely to
  * reclaim test allocations. Production code has no unfreeze operation. */
 mFts->Exiting=mFts->Frozen=FALSE;assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();}
int main(void){EFI_ABSOLUTE_POINTER_STATE S;
 /* ID qualification cannot require a touch interrupt at driver load. The
    captured device returns FF before a report is pending. */
 Setup();memset(ReportData,0xff,sizeof(ReportData));
 assert((FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS)&&"idle firmware must publish without a forced report");
 assert(F.Reads==2&&F.Published);
 Tick(FALSE);assert(F.Reads==2&&GetState(&mFts->Pointer,&S)==EFI_NOT_READY);
 Tick(TRUE);assert(F.Reads==3&&GetState(&mFts->Pointer,&S)==EFI_COMPROMISED_DATA);
 F.EdgeStatus=EFI_DEVICE_ERROR;Tick(FALSE);assert(F.Reads==3&&GetState(&mFts->Pointer,&S)==EFI_DEVICE_ERROR);
 assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();Gpio.Revision=0x10001;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_UNSUPPORTED&&!F.Reads&&!F.Installs);VerifyClean();
 Setup();Bus.Instance=2;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_NOT_FOUND&&!F.Reads&&!F.Installs);VerifyClean();
 /* Real firmware masks exclude INFO. These failures must distinguish ID
  * transport, supported-ID qualification, report transport and report parse
  * without publishing input or displaying bytes from a failed read. */
 Setup();F.IdReadStatus=EFI_COMPROMISED_DATA;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_COMPROMISED_DATA&&!F.Installs&&F.Reads==10);
 assert(DiagnosticIds==10&&DiagnosticQualifications==1&&DiagnosticQualification==EFI_COMPROMISED_DATA&&!DiagnosticReports&&!DiagnosticLines&&!DiagnosticValidations);VerifyClean();
 Setup();F.BadId=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_UNSUPPORTED&&!F.Installs&&F.Reads==20);
 assert(DiagnosticIds==20&&DiagnosticQualifications==1&&DiagnosticQualification==EFI_UNSUPPORTED&&!DiagnosticReports&&!DiagnosticValidations);VerifyClean();
 Setup();F.ReportReadStatus=EFI_COMPROMISED_DATA;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS&&F.Reads==2);Tick(TRUE);assert(GetState(&mFts->Pointer,&S)==EFI_COMPROMISED_DATA&&F.Reads==3);
 assert(DiagnosticIds==2&&DiagnosticQualifications==1&&DiagnosticQualification==EFI_SUCCESS&&DiagnosticReports==1&&!DiagnosticLines&&DiagnosticValidations==1&&DiagnosticValidation==EFI_COMPROMISED_DATA);assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();memset(ReportData,0,sizeof(ReportData));assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS&&F.Reads==2);Tick(TRUE);assert(GetState(&mFts->Pointer,&S)==EFI_COMPROMISED_DATA&&F.Reads==3);
 assert(DiagnosticIds==2&&DiagnosticQualifications==1&&DiagnosticQualification==EFI_SUCCESS&&DiagnosticReports==1&&DiagnosticLines==4&&DiagnosticValidations==1&&DiagnosticValidation==EFI_COMPROMISED_DATA);assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();F.FailArm=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_DEVICE_ERROR&&!F.Installs&&F.Reads==2);VerifyClean();
 Setup();F.BadRate=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_PROTOCOL_ERROR&&!F.Reads&&!F.Installs);VerifyClean();
 Setup();F.FailTimer=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_DEVICE_ERROR&&!F.Installs);VerifyClean();
 Setup();F.FailInstall=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_DEVICE_ERROR);VerifyClean();
 for(UINTN I=1;I<=3;++I){Setup();F.FailCreate=I;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_OUT_OF_RESOURCES);VerifyClean();}
 Setup();ReenterAllocation=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS&&RecursiveEntryStatus==EFI_ALREADY_STARTED);assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();F.FailInstall=F.FailClose=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS&&mFts->ResidentFailed&&mFts->Failure==EFI_DEVICE_ERROR&&!F.Published&&mFts->Poll);Tick(TRUE);F.FailClose=FALSE;assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();F.FailInstall=F.FailCancel=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS&&mFts->ResidentFailed&&!F.Published);F.FailCancel=FALSE;assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();F.PartialInstall=F.FailUninstall=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS&&mFts->ResidentFailed&&F.Published);assert(GetState(&mFts->Pointer,&S)==EFI_DEVICE_ERROR);F.FailUninstall=FALSE;assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS&&mFts&&F.Published&&Loaded.Unload);assert(mFts->Mode.AbsoluteMaxX==1080&&mFts->Mode.AbsoluteMaxY==2340);assert(GetState(&mFts->Pointer,&S)==EFI_NOT_READY);
 assert(DiagnosticIds==2 && !DiagnosticReports && !DiagnosticLines);ReportData[0]=0x40;ReportData[2]=1;ReportData[3]=0;ReportData[4]=120;ReportData[5]=0;ReportData[6]=240;Tick(TRUE);assert(GetState(&mFts->Pointer,&S)==EFI_SUCCESS&&S.ActiveButtons==EFI_ABSP_TouchActive&&S.CurrentX==120&&S.CurrentY==240);
 ReportData[2]=0;ReportData[3]=0x40;Tick(TRUE);assert(GetState(&mFts->Pointer,&S)==EFI_SUCCESS&&!S.ActiveButtons&&S.CurrentX==120&&S.CurrentY==240);
 assert(DiagnosticIds==2 && DiagnosticReports==1 && DiagnosticLines==4);F.FailUninstall=TRUE;assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED&&mFts&&!mFts->Armed&&F.Published);F.FailUninstall=FALSE;
 F.FailRailRelease=TRUE;assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_DEVICE_ERROR&&mFts&&!F.Published&&!mFts->Poll&&F.Rails[1]);F.FailRailRelease=FALSE;assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS);UINTN Reads=F.Reads;ExitBoot(mFts->Exit,mFts);Tick(TRUE);assert(F.Reads==Reads&&GetState(&mFts->Pointer,&S)==EFI_NOT_READY);TestDisposeFrozen();
 Setup();assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS);F.UnloadAtRequest=TRUE;Tick(TRUE);assert(mFts&&F.ReenterStatus==EFI_ACCESS_DENIED);assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 Setup();assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS);Reads=F.Reads;F.ExitAtFrequency=TRUE;Tick(TRUE);assert(F.Reads==Reads&&mFts->Exiting&&F.Pins[0]&&F.Rails[0]);TestDisposeFrozen();
 Setup();assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS);F.ExitAtRequest=TRUE;Tick(TRUE);assert(mFts->Exiting&&F.Pins[0]&&F.Rails[0]);TestDisposeFrozen();
 Setup();F.ExitAtInstall=TRUE;assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS&&mFts->ResidentFailed&&mFts->Failure==EFI_ABORTED&&mFts->Exiting);assert(GetState(&mFts->Pointer,&S)==EFI_NOT_READY);TestDisposeFrozen();
 Setup();assert(FocalTechTouchCrEntry((EFI_HANDLE)1,NULL)==EFI_SUCCESS);Reads=F.Reads;F.Tpl=TPL_NOTIFY;assert(Reset(&mFts->Pointer,TRUE)==EFI_ACCESS_DENIED&&F.Reads==Reads);F.Tpl=TPL_HIGH_LEVEL;assert(GetState(&mFts->Pointer,&S)==EFI_ACCESS_DENIED&&F.Reads==Reads);F.Tpl=TPL_APPLICATION;assert(Reset(&mFts->Pointer,TRUE)==EFI_SUCCESS&&F.Reads==Reads+2);Tick(FALSE);assert(F.Reads==Reads+2);assert(FocalTechTouchCrUnload((EFI_HANDLE)1)==EFI_SUCCESS);VerifyClean();
 puts("FTS actual EFI adapter: native repeated-start packets, GPIO/RPMh before publish, pointer/release, event rollback, retained unload, no I2C after EBS PASS");return 0;
}
