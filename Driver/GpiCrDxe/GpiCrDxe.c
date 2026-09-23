/** @file
 *  GPI DMA target-resource protocol producer.
 *
 *  MU's GpiDxe owns the DMA engine.  This driver only publishes the Linux
 *  SM8450 base/IRQ/channel/SID metadata and refuses to start without QUP
 *  readiness, so it cannot accidentally become a second DMA owner.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Library/CrDalLib.h>
#include <Protocol/EFIGpiCrProtocol.h>
#include <Protocol/EFIMuBusProtocol.h>
#include <Protocol/EFIQupCrProtocol.h>

STATIC CONST CR_TARGET_QUP_CONTEXT *mGpiContext;
STATIC MU_GPI_PROTOCOL               *mMuGpi;

STATIC EFI_STATUS
EFIAPI
GpiGetController (
  IN  EFI_GPI_CR_PROTOCOL               *This,
  IN  UINT8                              ControllerId,
  OUT CONST CR_TARGET_GPI_CONTROLLER **Controller
  )
{
  UINTN Index;

  (VOID)This;
  if ((Controller == NULL) || (mGpiContext == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Controller = NULL;
  for (Index = 0; Index < mGpiContext->GpiCount; ++Index) {
    if (mGpiContext->Gpi[Index].Id == ControllerId) {
      *Controller = &mGpiContext->Gpi[Index];
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS
EFIAPI
GpiGetContext (
  IN  EFI_GPI_CR_PROTOCOL       *This,
  OUT CONST CR_TARGET_QUP_CONTEXT **Context
  )
{
  (VOID)This;
  if ((Context == NULL) || (mGpiContext == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *Context = mGpiContext;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
GpiGetMuProtocol (
  IN  EFI_GPI_CR_PROTOCOL *This,
  OUT MU_GPI_PROTOCOL     **Protocol
  )
{
  (VOID)This;
  if (Protocol == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Protocol = NULL;
  if (mMuGpi == NULL) {
    return EFI_NOT_READY;
  }
  *Protocol = mMuGpi;
  return EFI_SUCCESS;
}

STATIC EFI_GPI_CR_PROTOCOL mGpiProtocol = {
  .Revision       = EFI_GPI_CR_PROTOCOL_REVISION,
  .GetController  = GpiGetController,
  .GetContext     = GpiGetContext,
  .GetMuProtocol  = GpiGetMuProtocol,
  .MuProtocol     = NULL,
};

EFI_STATUS
EFIAPI
GpiCrEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_QUP_CR_PROTOCOL   *Qup;

  (VOID)SystemTable;
  Status = gBS->LocateProtocol (
      &gEfiQupCrProtocolGuid, NULL, (VOID **)&Qup);
  if (EFI_ERROR (Status) || (Qup == NULL)) {
    return EFI_NOT_READY;
  }

  Status = gBS->LocateProtocol (
      &gQcomGpiInitProtocolGuid, NULL, (VOID **)&mMuGpi);
  if (EFI_ERROR (Status) || (mMuGpi == NULL) ||
      (mMuGpi->Revision < MU_GPI_PROTOCOL_REVISION) ||
      (mMuGpi->IfaceReg == NULL) || (mMuGpi->IfaceDereg == NULL) ||
      (mMuGpi->ProcessTre == NULL)) {
    return EFI_NOT_READY;
  }

  mGpiContext = CrDalGetQupContext ();
  if ((mGpiContext == NULL) || (mGpiContext->Gpi == NULL) ||
      (mGpiContext->GpiCount == 0)) {
    return EFI_NOT_FOUND;
  }

  mGpiProtocol.MuProtocol = mMuGpi;

  return gBS->InstallMultipleProtocolInterfaces (
      &ImageHandle, &gEfiGpiCrProtocolGuid, &mGpiProtocol, NULL);
}
