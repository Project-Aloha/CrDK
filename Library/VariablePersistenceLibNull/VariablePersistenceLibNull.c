/** @file
  Default platform: retain the existing emulated variable behavior.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Library/VariablePersistenceLib.h>
EFI_STATUS EFIAPI VariablePersistenceLoad (VOID *Store, UINTN Size) {
  (VOID)Store; (VOID)Size; return EFI_SUCCESS;
}
EFI_STATUS EFIAPI VariablePersistenceBegin (VOID) { return EFI_SUCCESS; }
EFI_STATUS EFIAPI VariablePersistenceCommit (CONST VOID *Store, UINTN Size) {
  (VOID)Store; (VOID)Size; return EFI_SUCCESS;
}
VOID EFIAPI VariablePersistenceEnd (VOID) { }
