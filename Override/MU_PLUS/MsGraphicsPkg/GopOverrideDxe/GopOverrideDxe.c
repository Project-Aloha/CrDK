/** @file

   Graphics (GOP) Override implementation

Copyright (C) Microsoft Corporation. All rights reserved.
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <PiDxe.h>

#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

//
// ****** Global variables ******
//

EFI_GUID  *mMsGopOverrideProtocolGuid;
// EFI_GRAPHICS_OUTPUT_PROTOCOL        *mGop;
EFI_EVENT  mGopRegisterEvent;
VOID       *mGopRegistration;

/**
  GOP registration notification callback

  @param[in] Event      Event that signalled the callback.
  @param[in] Context    Pointer to an optional event contxt.

  @retval None.

**/
VOID
EFIAPI
GopRegisteredCallback (
  IN  EFI_EVENT  Event,
  IN  VOID       *Context
  )
{
  EFI_STATUS                    Status = EFI_SUCCESS;
  EFI_HANDLE                    *Handles = NULL;
  EFI_HANDLE                    Controller = NULL;
  UINTN                         HandleCount = 0;
  UINTN                         Index;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *pGop;
  EFI_DEVICE_PATH_PROTOCOL      *DevicePath;

  //
  // ConSplitter also publishes GOP. Only a display controller has a device path.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid,
                                 (VOID **)&DevicePath);
    if (EFI_ERROR (Status) || (DevicePath == NULL)) {
      continue;
    }
    // A platform with multiple framebuffers can select its rendering-engine
    // parent explicitly. Other GOPs remain available to GraphicsConsole.
    CONST EFI_GUID *Selected = PcdGetPtr (PcdMsGopOverrideControllerGuid);
    CONST EFI_GUID Empty = {0};
    if (!CompareGuid (Selected, &Empty)) {
      CONST EFI_DEVICE_PATH_PROTOCOL *End = (CONST VOID *)((CONST UINT8 *)DevicePath + sizeof (VENDOR_DEVICE_PATH));
      if (DevicePath->Type != HARDWARE_DEVICE_PATH || DevicePath->SubType != HW_VENDOR_DP ||
          (DevicePath->Length[0] | (DevicePath->Length[1] << 8)) != sizeof (VENDOR_DEVICE_PATH) ||
          !CompareGuid (&((CONST VENDOR_DEVICE_PATH *)DevicePath)->Guid, Selected) ||
          End->Type != END_DEVICE_PATH_TYPE || End->SubType != END_ENTIRE_DEVICE_PATH_SUBTYPE ||
          (End->Length[0] | (End->Length[1] << 8)) != sizeof (*End)) {
        continue;
      }
    }
    if (Controller != NULL) {
      DEBUG ((DEBUG_ERROR, "ERROR [GOP]: Multiple display controllers found\n"));
      Status = EFI_UNSUPPORTED;
      goto Exit;
    }

    Controller = Handles[Index];
  }
  if (Controller == NULL) {
    goto Exit;
  }

  Status = gBS->HandleProtocol (
                  Controller,
                  &gEfiGraphicsOutputProtocolGuid,
                  (VOID **)&pGop
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [GOP]: Unable to get %g protocol - code=%r\n", &gEfiGraphicsOutputProtocolGuid, Status));
    goto Exit;
  }

  //
  // Now, install Graphics Output Override Protocol on this handle.
  //
  Status = gBS->InstallProtocolInterface (
                  &Controller,
                  mMsGopOverrideProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  (VOID *)pGop
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [GOP]: Unable to install %g protocol - code=%r\n", mMsGopOverrideProtocolGuid, Status));
    goto Exit;
  }

  //
  // Uninstall Graphics Output Protocol on this handle.
  //
  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Controller,
                  &gEfiGraphicsOutputProtocolGuid,
                  (VOID *)pGop,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [GOP]: Unable to uninstall %g protocol - code=%r\n", &gEfiGraphicsOutputProtocolGuid, Status));
    gBS->UninstallMultipleProtocolInterfaces (Controller,
                                             mMsGopOverrideProtocolGuid,
                                             (VOID *)pGop, NULL);
    goto Exit;
  }
  DEBUG ((DEBUG_INIT, "GOP Override: native display selected from %u GOP handles\n", (UINT32)HandleCount));

  //
  // On success, close the Graphics Output Protocol registration notification event.
  //
  if (mGopRegisterEvent != NULL) {
    Status = gBS->CloseEvent (mGopRegisterEvent);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "ERROR [GOP]: Unable to close GOP Override event - code=%r\n", Status));
      goto Exit;
    }
    mGopRegisterEvent = NULL;
  }

Exit:
  if (Handles != NULL) {
    FreePool (Handles);
  }

  DEBUG ((DEBUG_INFO, "INFO [GOP]: GopRegisteredCallback exit - code=%r\n", Status));

  return;
}

/**
  Main entry point for this driver.

  @param[in] ImageHandle    Image handle this driver.
  @param[in] SystemTable    Pointer to SystemTable.

  @retval EFI_SUCCESS           This function always complete successfully.
  @retval EFI_OUT_OF_RESOURCES  Insufficient resources to initialize.

**/
EFI_STATUS
EFIAPI
DriverInit (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;

  mMsGopOverrideProtocolGuid = PcdGetPtr (PcdMsGopOverrideProtocolGuid);

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  GopRegisteredCallback,
                  NULL,
                  &mGopRegisterEvent
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [GOP]: Failed to create GOP registration event (%r).\r\n", Status));
    goto Exit;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiGraphicsOutputProtocolGuid,
                  mGopRegisterEvent,
                  &mGopRegistration
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ERROR [GOP]: Failed to register for GOP registration notifications (%r).\r\n", Status));
    goto Exit;
  }

  // Registration covers future GOPs; signal once to inspect existing ones.
  Status = gBS->SignalEvent (mGopRegisterEvent);

Exit:
  if (EFI_ERROR (Status) && (mGopRegisterEvent != NULL)) {
    gBS->CloseEvent (mGopRegisterEvent);
    mGopRegisterEvent = NULL;
  }
  DEBUG ((DEBUG_INFO, "INFO [GOP]: DriverInit exit - code=%r\n", Status));

  return Status;
}

/**
  Driver Clean-Up

  @retval EFI_SUCCESS       This function always complete successfully.

**/
static EFI_STATUS
EFIAPI
DriverCleanUp (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;

  //
  // Close the Graphics Output Protocol registration notification event.
  //
  if (mGopRegisterEvent != NULL) {
    Status = gBS->CloseEvent (mGopRegisterEvent);
    if (!EFI_ERROR (Status)) {
      mGopRegisterEvent = NULL;
    }
  }

  return Status;
}

/**
  Driver unload handler.

  @param[in] ImageHandle    Image handle this driver.

  @retval EFI_SUCCESS       This function always complete successfully.

**/
EFI_STATUS
EFIAPI
DriverUnload (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;

  Status = DriverCleanUp (ImageHandle);

  return Status;
}
