/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include "../Library/OskInterruptLib/oskinterrupt.c"

EFI_GUID gHardwareInterrupt2ProtocolGuid;
EFI_BOOT_SERVICES *gBS;

STATIC HARDWARE_INTERRUPT_HANDLER mHardwareHandler[64];
STATIC BOOLEAN mFailRegisterAfterInstall;
STATIC BOOLEAN mFailSetTrigger;
STATIC BOOLEAN mFailEnable;
STATIC BOOLEAN mFailDisable;
STATIC BOOLEAN mFailUnregister;
STATIC UINTN mHandled;
STATIC UINTN mUnregisters;
STATIC UINTN mEois;
STATIC EFI_TPL mTpl;
STATIC UINTN mExpectedEoi, mDeferredCalls;
STATIC BOOLEAN mWorkQueued;
STATIC UINTN mLocateCalls;
STATIC BOOLEAN mFailLocate, mInvalidProvider;

STATIC EFI_TPL EFIAPI MockRaise(EFI_TPL Tpl) {
  EFI_TPL Old = mTpl;
  assert(Tpl == TPL_HIGH_LEVEL && Tpl >= Old);
  mTpl = Tpl;
  return Old;
}
STATIC VOID EFIAPI MockRestore(EFI_TPL Tpl) {
  assert(mTpl == TPL_HIGH_LEVEL && mEois == mExpectedEoi);
  mTpl = Tpl;
  if (mWorkQueued && mTpl < TPL_CALLBACK) {
    mWorkQueued = FALSE; ++mDeferredCalls;
  }
}
STATIC EFI_STATUS EFIAPI MockSignal(EFI_EVENT Event) {
  assert(Event == (EFI_EVENT)1 && mTpl == TPL_HIGH_LEVEL);
  mWorkQueued = TRUE;
  return EFI_SUCCESS;
}

VOID EFIAPI
DebugPrint(IN UINTN Level, IN CONST CHAR8 *Format, ...)
{
  (VOID)Level;
  (VOID)Format;
}

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(IN UINTN Level)
{
  (VOID)Level;
  return FALSE;
}

STATIC EFI_STATUS EFIAPI
MockRegister(
    IN EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,
    IN HARDWARE_INTERRUPT_SOURCE Source,
    IN HARDWARE_INTERRUPT_HANDLER Handler)
{
  (VOID)This;
  assert(Source < ARRAY_SIZE(mHardwareHandler));
  assert(mTpl == TPL_HIGH_LEVEL);
  if (Handler == NULL) {
    mUnregisters++;
    if (mFailUnregister) {
      return EFI_DEVICE_ERROR;
    }
    assert(mHardwareHandler[Source] != NULL);
    mHardwareHandler[Source] = NULL;
    return EFI_SUCCESS;
  }
  if (mHardwareHandler[Source] != NULL) {
    return EFI_ALREADY_STARTED;
  }
  mHardwareHandler[Source] = Handler;
  return mFailRegisterAfterInstall ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockEnable(IN EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,
           IN HARDWARE_INTERRUPT_SOURCE Source)
{
  (VOID)This;
  (VOID)Source;
  return mFailEnable ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockDisable(IN EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,
            IN HARDWARE_INTERRUPT_SOURCE Source)
{
  (VOID)This;
  (VOID)Source;
  return mFailDisable ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockEoi(IN EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,
        IN HARDWARE_INTERRUPT_SOURCE Source)
{
  (VOID)This;
  (VOID)Source;
  assert(mTpl == TPL_HIGH_LEVEL && mDeferredCalls < mHandled);
  mEois++;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockSetTrigger(IN EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,
               IN HARDWARE_INTERRUPT_SOURCE Source,
               IN EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE Trigger)
{
  (VOID)This;
  (VOID)Source;
  (VOID)Trigger;
  return mFailSetTrigger ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_HARDWARE_INTERRUPT2_PROTOCOL mMock = {
  .RegisterInterruptSource = MockRegister,
  .EnableInterruptSource = MockEnable,
  .DisableInterruptSource = MockDisable,
  .EndOfInterrupt = MockEoi,
  .SetTriggerType = MockSetTrigger
};

STATIC EFI_STATUS EFIAPI
MockLocate(IN EFI_GUID *Protocol, IN VOID *Registration, OUT VOID **Interface)
{
  (VOID)Protocol;
  (VOID)Registration;
  /* Actual DXE protocol database acquires its TPL_NOTIFY lock. */
  assert(mTpl <= TPL_NOTIFY);
  ++mLocateCalls;
  if (mFailLocate) return EFI_NOT_FOUND;
  *Interface = mInvalidProvider ? NULL : &mMock;
  return EFI_SUCCESS;
}

STATIC EFI_BOOT_SERVICES mBootServices = { .LocateProtocol = MockLocate,
  .RaiseTPL = MockRaise, .RestoreTPL = MockRestore, .SignalEvent = MockSignal };

STATIC VOID Handler(IN VOID *Parameter)
{
  assert(Parameter == &mHandled);
  assert(mTpl == TPL_HIGH_LEVEL);
  CR_INTERRUPT_CONFIG Active={.InterruptNumber=40};
  assert(CrUnregisterInterrupt(&Active)==CR_BUSY);
  mHandled++;
  assert(gBS->SignalEvent((EFI_EVENT)1) == EFI_SUCCESS);
}

STATIC VOID EFIAPI
ForeignHandler(IN HARDWARE_INTERRUPT_SOURCE Source,
               IN EFI_SYSTEM_CONTEXT Context)
{
  (VOID)Source;
  (VOID)Context;
}

STATIC VOID ResetMock(VOID)
{
  memset(mCrIntTable, 0, sizeof(mCrIntTable));
  memset(mHardwareHandler, 0, sizeof(mHardwareHandler));
  mHwInterrupt2 = NULL;
  gBS = &mBootServices;
  mFailRegisterAfterInstall = FALSE;
  mFailSetTrigger = FALSE;
  mFailEnable = FALSE;
  mFailDisable = FALSE;
  mFailUnregister = FALSE;
  mHandled = 0;
  mUnregisters = 0;
  mEois = 0;
  mTpl = TPL_APPLICATION; mExpectedEoi = mDeferredCalls = 0;
  mWorkQueued = FALSE;
  mLocateCalls = 0; mFailLocate = mInvalidProvider = FALSE;
}

int main(void)
{
  CR_INTERRUPT_CONFIG Config = {
    .Handler = Handler,
    .Param = &mHandled,
    .InterruptNumber = 40,
    .TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH
  };
  EFI_SYSTEM_CONTEXT SystemContext = {0};

  ResetMock();
  /* A cold call from HIGH must decline without illegal LocateProtocol or
     leaving the caller's priority changed. Cache is untouched on failure. */
  mTpl = TPL_HIGH_LEVEL;
  assert(CrRegisterInterrupt(&Config) == CR_NOT_READY);
  assert(mLocateCalls == 0 && mTpl == TPL_HIGH_LEVEL && mHwInterrupt2 == NULL);
  mTpl = TPL_APPLICATION; mFailLocate = TRUE;
  assert(CrRegisterInterrupt(&Config) == CR_NOT_FOUND);
  assert(mTpl == TPL_APPLICATION && mHwInterrupt2 == NULL && !CrInterruptIsRegistered(&Config));
  mFailLocate = FALSE; mInvalidProvider = TRUE;
  assert(CrRegisterInterrupt(&Config) == CR_UNSUPPORTED);
  assert(mTpl == TPL_APPLICATION && mHwInterrupt2 == NULL);
  ResetMock();
  assert(CrRegisterInterrupt(NULL) == CR_INVALID_PARAMETER);
  assert(CrUnregisterInterrupt(NULL) == CR_INVALID_PARAMETER);
  assert(!CrInterruptIsRegistered(NULL) && !CrInterruptIsRegistered(&Config));
  assert(CrRegisterInterrupt(&Config) == CR_SUCCESS);
  assert(CrInterruptIsRegistered(&Config));
  assert(mLocateCalls == 1 && mTpl == TPL_APPLICATION);
  /* Cached nested setup from HIGH performs table/controller IO only. */
  CR_INTERRUPT_GUARD Outer, Inner;
  assert(CrInterruptGuardAcquire(&Outer) == CR_SUCCESS);
  assert(CrInterruptGuardAcquire(&Inner) == CR_SUCCESS);
  CrInterruptGuardRelease(&Inner); CrInterruptGuardRelease(&Outer);
  assert(mLocateCalls == 1 && mTpl == TPL_APPLICATION);
  CR_INTERRUPT_CONFIG Foreign = Config;
  Foreign.Param = NULL;
  assert(!CrInterruptIsRegistered(&Foreign));
  mExpectedEoi=mEois+1;
  mHardwareHandler[40](40, SystemContext);
  assert(mHandled == 1 && mEois == 1 && mDeferredCalls == 1 && mTpl == TPL_APPLICATION);
  assert(CrRegisterInterrupt(&Config) == CR_BUSY);
  assert(CrUnregisterInterrupt(&Config) == CR_SUCCESS);
  assert(mHardwareHandler[40] == NULL && FindInterruptEntry(40) == NULL);
  assert(!CrInterruptIsRegistered(&Config));
  assert(CrRegisterInterrupt(&Config) == CR_SUCCESS);

  mFailDisable = TRUE;
  assert(CrUnregisterInterrupt(&Config) == EFI_DEVICE_ERROR);
  assert(mHardwareHandler[40] != NULL && FindInterruptEntry(40) != NULL);
  assert(CrInterruptIsRegistered(&Config));
  mFailDisable = FALSE;
  mFailUnregister = TRUE;
  assert(CrUnregisterInterrupt(&Config) == EFI_DEVICE_ERROR);
  assert(mHardwareHandler[40] != NULL && FindInterruptEntry(40) != NULL);
  mFailUnregister = FALSE;
  assert(CrUnregisterInterrupt(&Config) == CR_SUCCESS);

  ResetMock();
  mFailSetTrigger = TRUE;
  assert(CrRegisterInterrupt(&Config) == EFI_DEVICE_ERROR);
  assert(mHardwareHandler[40] == NULL && FindInterruptEntry(40) == NULL);
  mFailSetTrigger = FALSE;
  mFailEnable = TRUE;
  assert(CrRegisterInterrupt(&Config) == EFI_DEVICE_ERROR);
  assert(mHardwareHandler[40] == NULL && FindInterruptEntry(40) == NULL);

  ResetMock();
  mFailRegisterAfterInstall = TRUE;
  mFailUnregister = TRUE;
  assert(CrRegisterInterrupt(&Config) == EFI_DEVICE_ERROR);
  assert(mHardwareHandler[40] != NULL && FindInterruptEntry(40) != NULL);
  mExpectedEoi=mEois+1;
  mHardwareHandler[40](40, SystemContext);
  assert(mHandled == 1);
  mFailUnregister = FALSE;
  assert(CrUnregisterInterrupt(&Config) == CR_SUCCESS);
  assert(mHardwareHandler[40] == NULL && FindInterruptEntry(40) == NULL);

  ResetMock();
  mHardwareHandler[40] = ForeignHandler;
  assert(CrRegisterInterrupt(&Config) == EFI_ALREADY_STARTED);
  assert(mHardwareHandler[40] == ForeignHandler && FindInterruptEntry(40) == NULL);
  assert(!CrInterruptIsRegistered(&Config));
  assert(mUnregisters == 0);

  puts("OSKAL IRQ: registration rollback, real unregister and retained callbacks passed");
  return 0;
}
