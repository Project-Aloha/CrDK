/* Native-only protocol adapter; excluded from CRDK export. MIT */
#pragma once
#include <Library/PILCrLib.h>
#include <Protocol/EFISmemCrProtocol.h>
#include <Protocol/EFICmdDBCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
CR_STATUS CrPilInitialize(CR_PIL_CONTEXT *Pil, CONST CR_PIL_TARGET *Target,
                           EFI_SMEM_CR_PROTOCOL *Smem,
                           EFI_CMD_DB_PROTOCOL *CmdDb,
                           EFI_RPMH_CR_PROTOCOL *Rpmh,
                           EFI_INTERCONNECT_CR_PROTOCOL *Interconnect);
