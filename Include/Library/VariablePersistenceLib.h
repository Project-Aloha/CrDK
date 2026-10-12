/** @file
  Optional platform persistence for the emulated authenticated variable store.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef VARIABLE_PERSISTENCE_LIB_H_
#define VARIABLE_PERSISTENCE_LIB_H_
#include <Uefi.h>

/** Restore a fully validated store before variable parsing and authentication
    initialization. The caller supplies a freshly formatted default store.
    Missing backing storage must leave that default intact and return success. */
EFI_STATUS EFIAPI VariablePersistenceLoad (IN OUT VOID *Store, IN UINTN Size);

/** Admit an outer SetVariable operation before any variable mutation. A
    backend may reject unsafe TPL or recursive writes. Every successful Begin
    must be paired with End, even if the variable operation fails. */
EFI_STATUS EFIAPI VariablePersistenceBegin (VOID);

/** Persist the completed store after releasing the variable service lock.
    This must never invoke boot filesystem services after ExitBootServices.
    A successful call with no backing store intentionally drops persistence. */
EFI_STATUS EFIAPI VariablePersistenceCommit (IN CONST VOID *Store, IN UINTN Size);
VOID EFIAPI VariablePersistenceEnd (VOID);
#endif
