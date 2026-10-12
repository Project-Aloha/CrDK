/** @file
  Crane target-data publication driver.

  SPDX-License-Identifier: MIT
**/

#include <PiDxe.h>

#include <Library/CrDalRegistryLib.h>
#include <Library/CrTargetLib.h>
#include <Library/DebugLib.h>
#include "BootFdt.h"
#include <Library/UfsPowerFdtCrLib.h>
#include <Library/UfsPowerPolicyCrLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFICrDalProtocol.h>

STATIC CONST CR_DAL_DEVICE_MANIFEST *mManifest;
STATIC EFI_HANDLE                    mCrDalHandle;
/* Provider-owned storage becomes immutable at protocol publication. Keep
 * unaffected devices even when boot data cannot qualify the UFS supplies. */
STATIC CR_DAL_DEVICE_INFO             mPublishedDevices[128];
STATIC CR_DAL_DEVICE_MANIFEST         mPublishedManifest;
STATIC CR_TARGET_UFS_CONTEXT          mPublishedUfs;
STATIC CR_UFS_POWER_RESOLVED           mUfsPower;

STATIC EFI_STATUS PrepareUfs (CONST CR_DAL_DEVICE_INFO *Device)
{
  CONST CR_TARGET_UFS_CONTEXT *Target;
  CONST VOID *Blob;
  UINTN BlobSize;
  CR_UFS_POWER_FDT Power;
  EFI_STATUS Status;
  if (Device->Instance!=0 || Device->DataRevision!=CR_DAL_DATA_REVISION_1 ||
      Device->DataSize!=sizeof(*Target) ||
      !(Device->Attributes&CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE)) return EFI_UNSUPPORTED;
  Target=Device->Data;
  if (!CrTargetUfsValid(Target) || !Target->PowerPolicy) return EFI_COMPROMISED_DATA;
  if ((Target->PowerPolicy->Revision!=1 && Target->PowerPolicy->Revision!=2) ||
      Target->PowerPolicy->Size!=sizeof(*Target->PowerPolicy) ||
      Target->PowerPolicy->RequireHob>1 || !Target->PowerPolicy->Rules ||
      !Target->PowerPolicy->RuleCount || Target->PowerPolicy->RuleCount>CR_TARGET_UFS_MAX_RAILS)
    return EFI_COMPROMISED_DATA;
  if (Target->PowerPolicy->Revision==2 && !Target->PowerPolicy->RequireHob)
    return EFI_COMPROMISED_DATA;
  if (!CrBootFdtHobPresent()) {
    if (Target->PowerPolicy->RequireHob) return EFI_NOT_FOUND;
    mPublishedUfs=*Target;
    DEBUG ((DEBUG_ERROR,"CrDAL: UFS using explicit legacy board power profile; no raw FDT HOB\n"));
    return EFI_SUCCESS;
  }
  EFI_CR_FDT_PROTOCOL *Fdt=CrBootFdtProtocol();
  Status=Fdt->GetBlob(Fdt,&Blob,&BlobSize);
  if (Status!=EFI_SUCCESS) return Status;
  Status=CrUfsPowerFdtParse(Blob,BlobSize,&Power);
  if (Status!=EFI_SUCCESS) return Status;
  Status=CrUfsPowerResolve(Target,&Power,&mUfsPower);
  if (Status!=EFI_SUCCESS) return Status;
  mPublishedUfs=*Target;
  mPublishedUfs.Rails=mUfsPower.Rails;
  mPublishedUfs.RailCount=mUfsPower.Count;
  mPublishedUfs.RetainedRailMask=mUfsPower.RetainedRailMask;
  mPublishedUfs.RetainedEnableSupply=mUfsPower.RetainedEnableSupply;
  if (!CrTargetUfsValid(&mPublishedUfs)) return EFI_COMPROMISED_DATA;
  for (UINT32 I=0; I<mUfsPower.Count; ++I)
    DEBUG ((DEBUG_ERROR,"CrDAL: UFS FDT rail %a vote=%u mV source=%a\n",
            mUfsPower.Rails[I].Name,mUfsPower.Rails[I].VoltageMv,
            mUfsPower.Sources[I]==1 ? "DT" : "qualified board preference"));
  DEBUG ((DEBUG_ERROR,"CrDAL: UFS FDT inherited/fixed supply roles=0x%x\n",mUfsPower.InheritedRoles));
  if (mPublishedUfs.RetainedEnableSupply)
    DEBUG ((DEBUG_ERROR,"CrDAL: UFS FDT supplier %a: retained enable, inherited voltage/mode\n",
            mPublishedUfs.RetainedEnableSupply));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS PrepareManifest (VOID)
{
  if (mManifest->DeviceCount>ARRAY_SIZE(mPublishedDevices)) return EFI_OUT_OF_RESOURCES;
  mPublishedManifest=*mManifest;
  mPublishedManifest.Devices=mPublishedDevices;
  mPublishedManifest.DeviceCount=0;
  for (UINTN I=0; I<mManifest->DeviceCount; ++I) {
    CONST CR_DAL_DEVICE_INFO *Device=&mManifest->Devices[I];
    if (Device->Type==CrDalDeviceUfs) {
      EFI_STATUS Status=PrepareUfs(Device);
      if (Status!=EFI_SUCCESS) {
        DEBUG ((DEBUG_ERROR,"CrDAL: UFS omitted; boot power configuration rejected: %r\n",Status));
        continue;
      }
    }
    mPublishedDevices[mPublishedManifest.DeviceCount]=*Device;
    if (Device->Type==CrDalDeviceUfs)
      mPublishedDevices[mPublishedManifest.DeviceCount].Data=&mPublishedUfs;
    ++mPublishedManifest.DeviceCount;
  }
  mManifest=&mPublishedManifest;
  return CrDalRegistryValidate(mManifest);
}

STATIC
EFI_STATUS
EFIAPI
CrDalGetDeviceInfo (
  IN  EFI_CR_DAL_PROTOCOL      *This,
  IN  CR_DAL_DEVICE_TYPE        Type,
  IN  UINT32                    Instance,
  IN  UINT32                    MinimumDataRevision,
  IN  UINTN                     MinimumDataSize,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return CrDalRegistryGetDeviceInfo (
           mManifest,
           Type,
           Instance,
           MinimumDataRevision,
           MinimumDataSize,
           DeviceInfo
           );
}

STATIC
EFI_STATUS
EFIAPI
CrDalGetDeviceCount (
  IN  EFI_CR_DAL_PROTOCOL *This,
  OUT UINTN               *DeviceCount
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return CrDalRegistryGetDeviceCount (mManifest, DeviceCount);
}

STATIC
EFI_STATUS
EFIAPI
CrDalGetDeviceByIndex (
  IN  EFI_CR_DAL_PROTOCOL      *This,
  IN  UINTN                     Index,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return CrDalRegistryGetDeviceByIndex (mManifest, Index, DeviceInfo);
}

STATIC EFI_CR_DAL_PROTOCOL mCrDalProtocol = {
  .Revision         = EFI_CR_DAL_PROTOCOL_REVISION,
  .Size             = sizeof (EFI_CR_DAL_PROTOCOL),
  .Reserved         = 0,
  .GetDeviceInfo    = CrDalGetDeviceInfo,
  .GetDeviceCount   = CrDalGetDeviceCount,
  .GetDeviceByIndex = CrDalGetDeviceByIndex,
};

EFI_STATUS
EFIAPI
CrDalDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  if (mCrDalHandle!=NULL) return EFI_ALREADY_STARTED;

  Status = CrTargetGetDeviceManifest (&mManifest);
  if (Status!=EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "CrDAL: target manifest unavailable: %r\n", Status));
    return Status;
  }

  Status = CrDalRegistryValidate (mManifest);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CrDAL: invalid target manifest: %r\n", Status));
    mManifest = NULL;
    return Status;
  }

  Status=CrBootFdtImport();
  DEBUG ((DEBUG_ERROR,"CrDAL: DXE boot FDT import: %r\n",Status));
  CR_ANDROID_BOOT_INFO BootInfo;
  EFI_CR_FDT_PROTOCOL *Fdt=CrBootFdtProtocol();
  EFI_STATUS BootStatus=Fdt->GetBootInfo(Fdt,&BootInfo);
  DEBUG ((DEBUG_ERROR,"CrDAL: DXE Android boot parse: %r mode=%u source=%u\n",
          BootStatus,BootInfo.Mode,BootInfo.Source));
  (VOID)BootStatus;
  Status=PrepareManifest();
  if (Status!=EFI_SUCCESS) { mManifest=NULL; CrBootFdtDiscard(); return Status; }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mCrDalHandle,
                  &gEfiCrDalProtocolGuid,
                  &mCrDalProtocol,
                  &gEfiCrFdtProtocolGuid,
                  CrBootFdtProtocol(),
                  NULL
                  );
  if (Status!=EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "CrDAL: protocol installation failed: %r\n", Status));
    /* A partial installation may retain a callback into this image. */
    if (mCrDalHandle!=NULL) return EFI_SUCCESS;
    mManifest = NULL;
    CrBootFdtDiscard();
  }

  return Status;
}
