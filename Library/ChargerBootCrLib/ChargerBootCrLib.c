/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/EFICrFdtProtocol.h>
#include <Library/AndroidBootCrLib.h>
#include <Library/BaseLib.h>
#include <Library/ChargerBootLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

extern EFI_GUID gChargerCrFileGuid;

VOID EFIAPI ChargerBootLaunch (VOID)
{
  CR_ANDROID_BOOT_INFO Info;
  EFI_CR_FDT_PROTOCOL *Fdt;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_HANDLE Image;
  EFI_STATUS Status;
  EFI_LOADED_IMAGE_PROTOCOL *Parent;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH Node;

  Fdt = NULL;
  Status = gBS->LocateProtocol (&gEfiCrFdtProtocolGuid, NULL, (VOID **)&Fdt);
  if (Status != EFI_SUCCESS || Fdt == NULL ||
      Fdt->Revision != EFI_CR_FDT_PROTOCOL_REVISION ||
      Fdt->Size < sizeof (*Fdt) || Fdt->GetBootInfo == NULL) {
    return;
  }
  Info = (CR_ANDROID_BOOT_INFO){0};
  Status = Fdt->GetBootInfo (Fdt, &Info);
  if (Status != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "ChargerBootCr: DAL boot-mode data unavailable: %r\n", Status));
    return;
  }
  if (Info.Version != CR_ANDROID_BOOT_VERSION || Info.Mode != CR_ANDROID_BOOT_CHARGER) {
    return;
  }
  DEBUG ((DEBUG_ERROR, "ChargerBootCr: ABL requested charger mode; launching ChargerCr\n"));
  Parent = NULL;
  Path = NULL;
  Image = NULL;
  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Parent);
  if (Status == EFI_SUCCESS && Parent != NULL) {
    EfiInitializeFwVolDevicepathNode (&Node, &gChargerCrFileGuid);
    Path = AppendDevicePathNode (DevicePathFromHandle (Parent->DeviceHandle), &Node.Header);
    if (Path == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    }
  } else if (!EFI_ERROR (Status)) {
    Status = EFI_NOT_READY;
  }
  if (Path != NULL) {
    Status = gBS->LoadImage (FALSE, gImageHandle, Path, NULL, 0, &Image);
    FreePool (Path);
    if (Status == EFI_SUCCESS && Image != NULL) {
      Status = gBS->StartImage (Image, NULL, NULL);
      gBS->UnloadImage (Image);
    } else if (!EFI_ERROR (Status)) {
      Status = EFI_NOT_READY;
    }
  }
  if (Status != EFI_SUCCESS && !EFI_ERROR (Status)) {
    Status = EFI_NOT_READY;
  }
  /* A charging-only request must not fall through into the installed OS if
     the charging application fails or unexpectedly returns. */
  DEBUG ((DEBUG_ERROR, "ChargerBootCr: application returned %r; shutting down\n", Status));
  gRT->ResetSystem (EfiResetShutdown, Status, 0, NULL);
  CpuDeadLoop ();
}
