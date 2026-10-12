/* Private DXE boot-data importer. SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Protocol/EFICrFdtProtocol.h>
EFI_STATUS CrBootFdtImport(VOID);
BOOLEAN CrBootFdtHobPresent(VOID);
EFI_CR_FDT_PROTOCOL *CrBootFdtProtocol(VOID);
/* Only before successful or possibly-partial protocol publication. */
VOID CrBootFdtDiscard(VOID);
