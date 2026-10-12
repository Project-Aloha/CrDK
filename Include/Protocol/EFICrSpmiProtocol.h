/* Native shared SPMI owner. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/SpmiSharedCrLib.h>
#include <Library/PmicCcCrLib.h>
#define EFI_CR_SPMI_PROTOCOL_REVISION 1U
#define EFI_CR_SPMI_PROTOCOL_GUID {0x4d18a49b,0x6078,0x4d12,{0xa1,0x9d,0x3a,0x9c,0x8e,0x79,0xde,0x50}}
extern EFI_GUID gEfiCrSpmiProtocolGuid;
typedef enum { CrSpmiClientButtons=1,CrSpmiClientTypeC=2 } EFI_CR_SPMI_ROLE;
typedef struct _EFI_CR_SPMI_PROTOCOL EFI_CR_SPMI_PROTOCOL;
/* Consumers enter OSKAL interrupt guard around Acquire/Release and EVERY bus
   operation. No direct arbiter/PDC address or dispatch ownership is exposed.
   Each exact lease and bus storage remains resident until successful Release. */
struct _EFI_CR_SPMI_PROTOCOL {
 UINT32 Revision,Size;
 EFI_STATUS (EFIAPI *Acquire)(EFI_CR_SPMI_PROTOCOL *,EFI_CR_SPMI_ROLE,VOID *,CR_SPMI_CLIENT *,CR_SPMI_BUS *);
 EFI_STATUS (EFIAPI *Release)(EFI_CR_SPMI_PROTOCOL *,CR_SPMI_CLIENT *);
 EFI_STATUS (EFIAPI *GetTypeCTarget)(EFI_CR_SPMI_PROTOCOL *,CR_PMIC_CC_TARGET *);
};
