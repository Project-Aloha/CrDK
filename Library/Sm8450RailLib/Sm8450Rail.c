/* PM8350 regulator inputs on HDK8450, from native target and Linux HDK DTS.
   SPDX-License-Identifier: MIT */
#include <Library/Sm8450RailLib.h>
#include <oskal/cr_string.h>
#include <oskal/common.h>
typedef struct { CONST CHAR8 *Name; INT32 Parent; UINT32 Mv; UINT8 Mode; } RAIL;
/* SMPS/BOB AUTO=6. LDO output contracts belong to each caller. S11/S12
   values round the reference minimum up to their 8mV hardware selector;
   BOB3296 is the qualified native/vendor input value, not an inferred vote. */
STATIC CONST RAIL mRails[] = {
  {"ldob1",11,968,6}, {"ldob2",8,3296,6}, {"ldob5",8,3296,6},
  {"ldob6",12,1352,6}, {"ldob7",8,3296,6}, {"ldob9",12,1352,6},
  {"ldoc1",8,3296,6}, {"ldoh2",8,3296,6}, {"bobc1",-1,0,0},
  {"smpb10",-1,0,0}, {"smpe2",-1,0,0}, {"smpb11",-1,0,0},
  {"smpb12",-1,0,0}, {"smpc1",-1,0,0},
  /* HDK vdd-l6-l9-l11-supply=&vreg_bob. Retain the qualified shared
     3296mV/AUTO input contract; SD callers request 3008mV/HPM outputs. */
  {"ldoc6",8,3296,6}, {"ldoc9",8,3296,6}
};
CR_STATUS CrSm8450HdkRailsInit(CR_RPMH_RAIL *Rails,UINT32 Count) {
  if (Rails==NULL || Count!=ARRAY_SIZE(mRails) || Count!=CR_SM8450_HDK_RAIL_COUNT)
    return CR_INVALID_PARAMETER;
  /* Never reset a live graph, including a partially committed legacy vote. */
  for (UINT32 I=0;I<Count;++I)
    if (Rails[I].ReferenceCount || Rails[I].ParentOwned || Rails[I].VoltageRequested ||
        Rails[I].ModeRequested || Rails[I].EnableRequested) return CR_BUSY;
  cr_memset(Rails,0,sizeof(*Rails)*Count);
  for (UINT32 I=0;I<Count;++I) {
    Rails[I].Name=mRails[I].Name;Rails[I].ParentMv=mRails[I].Mv;Rails[I].ParentMode=mRails[I].Mode;
    if (mRails[I].Parent>=0) Rails[I].Parent=&Rails[mRails[I].Parent];
  }
  return CR_SUCCESS;
}
