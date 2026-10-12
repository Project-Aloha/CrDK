/* Native GPIO composition needs the same validated GIC/TPL prerequisite as
 * the real provider. Only Boot Services/GIC are modeled; OSKAL remains real.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Protocol/HardwareInterrupt2.h>
STATIC EFI_LOCATE_PROTOCOL mGpioPreviousLocate;
STATIC EFI_TPL mGpioFixtureTpl;
STATIC EFI_STATUS EFIAPI NoRegister(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,HARDWARE_INTERRUPT_SOURCE S,HARDWARE_INTERRUPT_HANDLER H) {
  (VOID)P;(VOID)S;(VOID)H;assert(!"UFS GPIO must not register an interrupt");return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI NoInterrupt(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,HARDWARE_INTERRUPT_SOURCE S) {
  (VOID)P;(VOID)S;assert(!"UFS GPIO must not change a GIC source");return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI NoTrigger(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,HARDWARE_INTERRUPT_SOURCE S,EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE T) {
  (VOID)P;(VOID)S;(VOID)T;assert(!"UFS GPIO must not change a GIC trigger");return EFI_UNSUPPORTED;
}
STATIC EFI_HARDWARE_INTERRUPT2_PROTOCOL mGpioFixtureGic={
 .RegisterInterruptSource=NoRegister,.EnableInterruptSource=NoInterrupt,
 .DisableInterruptSource=NoInterrupt,.EndOfInterrupt=NoInterrupt,.SetTriggerType=NoTrigger};
STATIC EFI_TPL EFIAPI GpioRaise(EFI_TPL T) {
 EFI_TPL Old=mGpioFixtureTpl;assert(T>=Old);mGpioFixtureTpl=T;return Old;
}
STATIC VOID EFIAPI GpioRestore(EFI_TPL T) {assert(T<=mGpioFixtureTpl);mGpioFixtureTpl=T;}
STATIC EFI_STATUS EFIAPI GpioLocate(EFI_GUID *G,VOID *R,VOID **P) {
 if(G==&gHardwareInterrupt2ProtocolGuid){assert(mGpioFixtureTpl<=TPL_NOTIFY);*P=&mGpioFixtureGic;return EFI_SUCCESS;}
 return mGpioPreviousLocate(G,R,P);
}
STATIC VOID GpioGuardFixtureInit(VOID) {
 assert(gBS&&gBS->LocateProtocol);mGpioPreviousLocate=gBS->LocateProtocol;
 gBS->LocateProtocol=GpioLocate;gBS->RaiseTPL=GpioRaise;gBS->RestoreTPL=GpioRestore;
 mGpioFixtureTpl=TPL_APPLICATION;
}
