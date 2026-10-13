/* Preserve the existing MIT PAS implementation and all of its notices.
   This private translation unit prevents an SDK-wide link from defining its
   public helper twice. Only the resident SCM provider publishes its owner.
   SPDX-License-Identifier: MIT */
#include "ScmPas.h"
#define CrPilSmcccInitialize CrScmPrivatePasInitialize
#define CrPilSmcccCall CrScmPrivatePasCall
#define CrPilSmcccUninitialize CrScmPrivatePasUninitialize
#define CrPilSmcccProbe CrScmPrivatePasProbe
#define CrPilArm64Smccc CrScmPasWire
#include "../PILCrLib/PilSmccc.c"
