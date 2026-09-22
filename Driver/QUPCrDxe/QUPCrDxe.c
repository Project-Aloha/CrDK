/** @file
 *  QUPv3 target-resource protocol producer.
 *
 *  This module deliberately does not touch QUP registers.  The MU QUP/GENI
 *  stack remains the sole hardware owner; Crane consumers use the published
 *  Linux-derived resource contract to sequence their dependencies.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Library/CrTargetQupLib.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFIQupCrProtocol.h>

STATIC CONST CR_TARGET_QUP_CONTEXT *mQupContext;

STATIC EFI_STATUS
EFIAPI
QupGetWrapper (
  IN  EFI_QUP_CR_PROTOCOL          *This,
  IN  UINT8                         WrapperId,
  OUT CONST CR_TARGET_QUP_WRAPPER **Wrapper
  )
{
  UINTN Index;

  (VOID)This;
  if ((Wrapper == NULL) || (mQupContext == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Wrapper = NULL;
  for (Index = 0; Index < mQupContext->QupCount; ++Index) {
    if (mQupContext->Qup[Index].Id == WrapperId) {
      *Wrapper = &mQupContext->Qup[Index];
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS
EFIAPI
QupGetContext (
  IN  EFI_QUP_CR_PROTOCOL       *This,
  OUT CONST CR_TARGET_QUP_CONTEXT **Context
  )
{
  (VOID)This;
  if ((Context == NULL) || (mQupContext == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *Context = mQupContext;
  return EFI_SUCCESS;
}

STATIC EFI_QUP_CR_PROTOCOL mQupProtocol = {
  .Revision   = EFI_QUP_CR_PROTOCOL_REVISION,
  .GetWrapper = QupGetWrapper,
  .GetContext = QupGetContext,
};

EFI_STATUS
EFIAPI
QupCrEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)SystemTable;
  mQupContext = CrTargetGetQupContext ();
  if ((mQupContext == NULL) || (mQupContext->Qup == NULL) ||
      (mQupContext->QupCount == 0)) {
    return EFI_NOT_FOUND;
  }

  /* The clock and ICB protocols are dependencies of this contract.  Their
   * presence proves that consumers can perform the ordered resource setup;
   * no private MU ownership is inferred from it. */
  {
    EFI_CLOCK_CR_PROTOCOL       *Clock;
    EFI_INTERCONNECT_CR_PROTOCOL *Icb;

    Status = gBS->LocateProtocol (
        &gEfiClockCrProtocolGuid, NULL, (VOID **)&Clock);
    if (EFI_ERROR (Status) || (Clock == NULL)) {
      return EFI_NOT_READY;
    }
    Status = gBS->LocateProtocol (
        &gEfiInterconnectCrProtocolGuid, NULL, (VOID **)&Icb);
    if (EFI_ERROR (Status) || (Icb == NULL)) {
      return EFI_NOT_READY;
    }
  }

  return gBS->InstallMultipleProtocolInterfaces (
      &ImageHandle, &gEfiQupCrProtocolGuid, &mQupProtocol, NULL);
}
