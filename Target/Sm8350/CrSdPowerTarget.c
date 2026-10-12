/* HDK8350 PM8350C SD rail policy. SPDX-License-Identifier: MIT
 * Both actual HDK trees permit BOB3008..3960mV. PMIC5 BOB selector10 is
 * 3000+10*32=3320mV; this chosen parent policy leaves360mV over SD2960mV.
 * This is a bounded source-based policy, not a measured dropout guarantee.
 * Parent ownership is retained by RpmhRailLib across shared consumers and
 * failed withdrawal; no untracked direct SPMI/regulator writes are needed.
 */
#include "CrStorageTarget.h"
STATIC CONST CHAR8 *CONST mNames[]={"ldob1","ldob2","ldoc1","bobc1","ldoc6","ldoc9","ldob5","ldob6","ldob7","ldob9","smpb12","smpe2","smpb11","smpb10","smpc1","ldoe7","rfclka5"};
STATIC CONST CR_DAL_RPMH_RAILS mRails={ARRAY_SIZE(mNames),mNames};
STATIC CONST CR_DAL_RPMH_PARENT mParents[]={
  /* Use the existing HDK PCIe WLAN/WiGig shared input contract. S1C
   * 1880mV is in both supplied Android and mainline HDK ranges; 1904mV
   * exceeds mainline's 1880mV maximum. The PM8350 SMPS AUTO request (6)
   * also agrees with the already qualified Android PCIe consumers. */
  {"ldob1","smpb11",952,6},{"ldob2","bobc1",3320,6},
  {"ldoc1","smpc1",1880,6},
  {"ldoc6","bobc1",3320,6},{"ldoc9","bobc1",3320,6},
  /* HDK mainline vdd-l3-l5 and vdd-l2-l7 are BOB supplied.
   * L6/L9 ancestry is not inferred from Android vccq-parent metadata. */
  {"ldob5","bobc1",3320,6},{"ldob7","bobc1",3320,6},
};
STATIC CONST CR_DAL_RPMH_TOPOLOGY mTopology={ARRAY_SIZE(mParents),mParents};
CONST CR_DAL_RPMH_RAILS *CrSm8350GetStorageRails(VOID) {return &mRails;}
CONST CR_DAL_RPMH_TOPOLOGY *CrSm8350GetStorageTopology(VOID) {return &mTopology;}
