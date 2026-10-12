/* Generated SM8350 ARC facts. SPDX-License-Identifier: MIT
 * Android R11 13ca08fdf0979fdd61d5e8991661874bb2d19150 final HDK DTB;
 * Linux a53261b60834ce617cc4f3c7723dda8285c59789 rpmhpd/clk-rpmh.
 * Register-independent facts, no Linux algorithm copied. See
 * LahainaCrPkg/Tests/sm8350-arc-facts.json for exact sources and resolved edges.
 * Highest qualified CmdDB corner is retained until reboot; MinLevel only
 * rejects an insufficient table. This is a new boot demand, not ABL readback.
 */
#include <CrDalDevice.h>
#include <oskal/common.h>
STATIC CONST CR_DAL_RPMH_ARC_ENTRY mSm8350ArcEntries[] = {
  {"mxc.lvl",CR_DAL_RPMH_ARC_POWER,384,CR_DAL_RPMH_ARC_NO_PARENT},
  {"mx.lvl",CR_DAL_RPMH_ARC_POWER,384,0},
  {"cx.lvl",CR_DAL_RPMH_ARC_POWER,384,1},
  {"lmx.lvl",CR_DAL_RPMH_ARC_POWER,16,CR_DAL_RPMH_ARC_NO_PARENT},
  {"lcx.lvl",CR_DAL_RPMH_ARC_POWER,16,CR_DAL_RPMH_ARC_NO_PARENT},
  {"mmcx.lvl",CR_DAL_RPMH_ARC_POWER,384,1},
  {"xo.lvl",CR_DAL_RPMH_ARC_XO,0,CR_DAL_RPMH_ARC_NO_PARENT},
};
STATIC CONST CR_DAL_RPMH_ARC_INVENTORY mSm8350ArcInventory = {
  CR_DAL_RPMH_ARC_REVISION,sizeof(CR_DAL_RPMH_ARC_INVENTORY),
  ARRAY_SIZE(mSm8350ArcEntries),mSm8350ArcEntries
};
CONST CR_DAL_RPMH_ARC_INVENTORY *CrSm8350GetArcInventory(VOID) { return &mSm8350ArcInventory; }
