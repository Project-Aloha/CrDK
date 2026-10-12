/** @file
  Optional persistence around a completed standard variable transaction.
  Authentication, policy, append/delete and attribute checks remain in the
  shared variable engine. Filesystem work runs outside its TPL_NOTIFY lock.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "Variable.h"
#include <Library/VariablePersistenceLib.h>

EFI_STATUS
EFIAPI
VariableServiceSetVariableWithPersistence (
  IN CHAR16 *VariableName,
  IN EFI_GUID *VendorGuid,
  IN UINT32 Attributes,
  IN UINTN DataSize,
  IN VOID *Data
  )
{
  EFI_STATUS Status;
  Status = VariablePersistenceBegin ();
  if (Status != EFI_SUCCESS) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }
  Status = VariableServiceSetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  if (Status == EFI_SUCCESS) {
    Status = VariablePersistenceCommit (mNvVariableCache, mNvVariableCache->Size);
    if (!EFI_ERROR (Status) && Status != EFI_SUCCESS) {
      Status = EFI_DEVICE_ERROR;
    }
  }
  VariablePersistenceEnd ();
  return Status;
}

EFI_STATUS
VariablePersistenceSaveCurrentStore (VOID)
{
  EFI_STATUS Status;
  Status = VariablePersistenceBegin ();
  if (Status != EFI_SUCCESS) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }
  Status = VariablePersistenceCommit (mNvVariableCache, mNvVariableCache->Size);
  VariablePersistenceEnd ();
  return Status == EFI_SUCCESS || EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
}
