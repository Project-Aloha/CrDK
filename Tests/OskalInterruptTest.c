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
  *Interface = &mMock;
  return EFI_SUCCESS;
}

STATIC EFI_BOOT_SERVICES mBootServices = { .LocateProtocol = MockLocate };

STATIC VOID Handler(IN VOID *Parameter)
{
  assert(Parameter == &mHandled);
  mHandled++;
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
  assert(CrRegisterInterrupt(NULL) == CR_INVALID_PARAMETER);
  assert(CrUnregisterInterrupt(NULL) == CR_INVALID_PARAMETER);
  assert(CrRegisterInterrupt(&Config) == CR_SUCCESS);
  mHardwareHandler[40](40, SystemContext);
  assert(mHandled == 1 && mEois == 1);
  assert(CrRegisterInterrupt(&Config) == CR_BUSY);
  assert(CrUnregisterInterrupt(&Config) == CR_SUCCESS);
  assert(mHardwareHandler[40] == NULL && FindInterruptEntry(40) == NULL);
  assert(CrRegisterInterrupt(&Config) == CR_SUCCESS);

  mFailDisable = TRUE;
  assert(CrUnregisterInterrupt(&Config) == EFI_DEVICE_ERROR);
  assert(mHardwareHandler[40] != NULL && FindInterruptEntry(40) != NULL);
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
  mHardwareHandler[40](40, SystemContext);
  assert(mHandled == 1);
  mFailUnregister = FALSE;
  assert(CrUnregisterInterrupt(&Config) == CR_SUCCESS);
  assert(mHardwareHandler[40] == NULL && FindInterruptEntry(40) == NULL);

  ResetMock();
  mHardwareHandler[40] = ForeignHandler;
  assert(CrRegisterInterrupt(&Config) == EFI_ALREADY_STARTED);
  assert(mHardwareHandler[40] == ForeignHandler && FindInterruptEntry(40) == NULL);
  assert(mUnregisters == 0);

  puts("OSKAL IRQ: registration rollback, real unregister and retained callbacks passed");
  return 0;
}
