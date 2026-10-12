/** Native async battery observer using the existing sole PMIC GLink owner.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/BatteryChargerCrLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIBatteryChargerCrProtocol.h>
#include <Protocol/EFIPmicGlinkCrProtocol.h>
#include <Guid/EventGroup.h>

STATIC CR_BATTERY_CHARGER mBattery;
STATIC EFI_BATTERY_CHARGER_CR_PROTOCOL mProtocol;
STATIC EFI_PMIC_GLINK_CR_PROTOCOL *mTransport;
STATIC EFI_EVENT mTimer, mExit;
STATIC BOOLEAN mStopped=TRUE, mRegistered, mPublished;
STATIC UINT32 mLoggedTimeouts, mLoggedMalformed;
STATIC UINT64 Now(VOID)
{ return GetTimeInNanoSecond(GetPerformanceCounter())/1000ULL; }
STATIC CR_STATUS Send(VOID *Context, CONST VOID *Data, UINTN Length)
{
  (VOID)Context;
  if(mStopped || !mRegistered || mTransport==NULL) return EFI_NOT_READY;
  return mTransport->Send(mTransport,CR_BATTERY_OWNER,&mBattery,Data,Length);
}
/* Provider invokes both pure callbacks at TPL_NOTIFY: no RPC/events/BootSvc. */
STATIC VOID EFIAPI Receive(VOID *Context, CONST UINT8 *Data, UINTN Length)
{ (VOID)Context; if(!mStopped) CrBatteryChargerReceive(&mBattery,Data,Length,Now()); }
STATIC VOID EFIAPI State(VOID *Context, CONST EFI_PMIC_GLINK_CR_STATE *Transport)
{
  (VOID)Context;
  CrBatteryChargerOnline(&mBattery,!mStopped && Transport->Online,
                        Transport->AdspGeneration,Transport->PeerGeneration);
}
STATIC EFI_STATUS EFIAPI Snapshot(EFI_BATTERY_CHARGER_CR_PROTOCOL *This,
                                  CR_BATTERY_CHARGER_SNAPSHOT *Out)
{
  EFI_TPL Tpl; EFI_STATUS Status;
  if(This!=&mProtocol || Out==NULL) return EFI_INVALID_PARAMETER;
  Tpl=gBS->RaiseTPL(TPL_NOTIFY);
  Status=CrBatteryChargerSnapshot(&mBattery,Now(),Out);
  gBS->RestoreTPL(Tpl); return Status;
}
STATIC VOID EFIAPI Tick(EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context;
  if(mStopped || !mRegistered) return;
  (VOID)CrBatteryChargerTick(&mBattery,Now());
  if(mLoggedTimeouts!=mBattery.State.Timeouts || mLoggedMalformed!=mBattery.State.Malformed) {
    DEBUG((DEBUG_WARN,"BatteryCr: battery RPC timeout=%u malformed=%u valid=0x%x pending=%u status=%r\n",
      mBattery.State.Timeouts,mBattery.State.Malformed,mBattery.State.ValidMask,mBattery.State.Pending,mBattery.State.LastError));
    mLoggedTimeouts=mBattery.State.Timeouts; mLoggedMalformed=mBattery.State.Malformed;
  }
}
STATIC EFI_STATUS Stop(VOID)
{
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_NOTIFY); EFI_STATUS Status;
  mStopped=TRUE;
  CrBatteryChargerOnline(&mBattery,FALSE,mBattery.AdspGeneration,mBattery.PeerGeneration);
  gBS->RestoreTPL(Tpl);
  if(mTimer!=NULL) {
    Status=gBS->SetTimer(mTimer,TimerCancel,0);
    if(Status!=EFI_SUCCESS) return Status;
  }
  if(mRegistered) {
    Status=mTransport->Unregister(mTransport,CR_BATTERY_OWNER,&mBattery);
    if(Status!=EFI_SUCCESS) return Status;
    mRegistered=FALSE; mTransport=NULL;
  }
  return EFI_SUCCESS;
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  (VOID)Event; (VOID)Context;
  Status=Stop();
  if(Status!=EFI_SUCCESS) DEBUG((DEBUG_ERROR,"BatteryCr: exit callback retention %r\n",Status));
}
STATIC EFI_STATUS Cleanup(VOID)
{
  EFI_STATUS Status=Stop();
  if(Status!=EFI_SUCCESS) return Status;
  if(mTimer!=NULL) {
    Status=gBS->CloseEvent(mTimer); if(Status!=EFI_SUCCESS) return Status;
    mTimer=NULL;
  }
  if(mExit!=NULL) {
    Status=gBS->CloseEvent(mExit); if(Status!=EFI_SUCCESS) return Status;
    mExit=NULL;
  }
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI BatteryChargerCrUnload(EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status=Stop();
  if(Status!=EFI_SUCCESS) return Status;
  if(mPublished) {
    Status=gBS->UninstallMultipleProtocolInterfaces(ImageHandle,
      &gEfiBatteryChargerCrProtocolGuid,&mProtocol,NULL);
    if(Status!=EFI_SUCCESS) return Status;
    mPublished=FALSE;
  }
  return Cleanup();
}
EFI_STATUS EFIAPI BatteryChargerCrEntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status, CleanupStatus;
  (VOID)SystemTable;
  if(mRegistered || mPublished || mTimer!=NULL || mExit!=NULL) return EFI_ALREADY_STARTED;
  mStopped=TRUE; mLoggedTimeouts=mLoggedMalformed=0;
  Status=CrBatteryChargerInit(&mBattery,Send,NULL); if(Status!=EFI_SUCCESS) return Status;
  Status=gBS->LocateProtocol(&gEfiPmicGlinkCrProtocolGuid,NULL,(VOID **)&mTransport);
  if(Status!=EFI_SUCCESS) return Status;
  if(mTransport==NULL || mTransport->Revision!=EFI_PMIC_GLINK_CR_PROTOCOL_REVISION ||
     mTransport->Register==NULL || mTransport->Unregister==NULL || mTransport->Send==NULL || mTransport->GetState==NULL)
    return EFI_UNSUPPORTED;
  Status=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Tick,NULL,&mTimer);
  if(Status!=EFI_SUCCESS) return Status;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,
                           &gEfiEventExitBootServicesGuid,&mExit);
  if(Status!=EFI_SUCCESS) goto Rollback;
  /* Open admission only after exit invalidation exists. Initial callback is
     synchronous; protocol publication never asserts ADSP/sample readiness. */
  mStopped=FALSE;
  Status=mTransport->Register(mTransport,CR_BATTERY_OWNER,&mBattery,Receive,State,NULL);
  if(Status!=EFI_SUCCESS) goto Rollback;
  mRegistered=TRUE;
  mProtocol.Revision=EFI_BATTERY_CHARGER_CR_PROTOCOL_REVISION; mProtocol.GetSnapshot=Snapshot;
  Status=gBS->InstallMultipleProtocolInterfaces(&ImageHandle,
    &gEfiBatteryChargerCrProtocolGuid,&mProtocol,NULL);
  if(Status!=EFI_SUCCESS) goto Rollback;
  mPublished=TRUE;
  Status=gBS->SetTimer(mTimer,TimerPeriodic,1000000ULL); /* 100 ms; one async RPC at a time */
  if(Status!=EFI_SUCCESS) {
    EFI_STATUS Uninstall;
    /* Close sampling/subscriber admission before a publication rollback can
       synchronously reconnect consumers. Failed cleanup pins this image. */
    if(Stop()!=EFI_SUCCESS) return EFI_SUCCESS;
    Uninstall=gBS->UninstallMultipleProtocolInterfaces(ImageHandle,
      &gEfiBatteryChargerCrProtocolGuid,&mProtocol,NULL);
    if(Uninstall!=EFI_SUCCESS) return EFI_SUCCESS;
    mPublished=FALSE; goto Rollback;
  }
  DEBUG((DEBUG_INIT,"BatteryCr: async SM8450 battery owner=%u observer armed; samples await ADSP\n",CR_BATTERY_OWNER));
  return EFI_SUCCESS;
Rollback:
  CleanupStatus=Cleanup();
  if(CleanupStatus!=EFI_SUCCESS) {
    DEBUG((DEBUG_ERROR,"BatteryCr: startup=%r cleanup=%r; stopped callback image retained\n",Status,CleanupStatus));
    return EFI_SUCCESS;
  }
  return Status;
}
