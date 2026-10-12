/* Explicit pass-through only; no GPU admission or retirement authority.
 * SPDX-License-Identifier: MIT */
#include <Library/AdrenoBootUefiLib.h>

static BOOLEAN Overlaps(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn) {
  UINTN Ap=(UINTN)A,Bp=(UINTN)B;
  if (An>MAX_UINTN-Ap || Bn>MAX_UINTN-Bp) return TRUE;
  return Ap<Bp+Bn && Bp<Ap+An;
}

EFI_STATUS CrAdrenoBootInstallIort(EFI_ACPI_TABLE_PROTOCOL *Acpi,
  CONST VOID *Table,UINTN Bytes,BOOLEAN *Handled,UINTN *TableKey) {
  if (Acpi==NULL || Table==NULL || Bytes<8 || Handled==NULL || TableKey==NULL ||
      Overlaps(Handled,sizeof(*Handled),Table,Bytes) ||
      Overlaps(Handled,sizeof(*Handled),Acpi,sizeof(*Acpi)) ||
      Overlaps(Handled,sizeof(*Handled),TableKey,sizeof(*TableKey)))
    return EFI_INVALID_PARAMETER;
  *Handled=FALSE;
  return EFI_SUCCESS;
}
