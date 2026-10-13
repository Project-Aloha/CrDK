/* Private unchanged PAS helper declarations. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/PilSmcccLib.h>
CR_STATUS CrScmPrivatePasInitialize(CR_PIL_SMCCC *,CONST CR_PIL_TARGET *);
CR_STATUS CrScmPrivatePasCall(VOID *,CR_PIL_SMC_ARGS *);
CR_STATUS CrScmPrivatePasUninitialize(CR_PIL_SMCCC *);
CR_STATUS CrScmPrivatePasProbe(CR_PIL_SMCCC *);
VOID CrScmPasWire(CR_PIL_SMC_ARGS *);
