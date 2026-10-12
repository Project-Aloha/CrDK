/* SM8350 UFS and PCIe SMMU data. SPDX-License-Identifier: MIT.
 * Exact PCIe SID pairs from the board DT and public sm8350.dtsi.
 * Android msm-arm-smmu has no matching ACTLR override. Preserve inherited
 * global/unowned state and the existing UFS cache policy; PCIe is coherent.
 */
#include <Library/CrTargetSmmuLib.h>
STATIC CONST UINT16 mSids[]={0xe0,0x1c00,0x1c01,0x1c80,0x1c81,0x0,0x20};
STATIC CONST UINT16 mCoherent[]={0x1c00,0x1c01,0x1c80,0x1c81};
STATIC CONST UINT16 mUfsSids[]={0xe0};
STATIC CONST CrTargetSmmuUsbDevice mUsbDevices[]={{0xa600000,0xcd00,0},{0xa800000,0xcd00,0x20}};
STATIC CrTargetSmmuContext mSmmu={
  .Base=0x15000000ULL,.Size=0x100000,.SegmentCount=2,.MaximumDomains=11,
  .DefaultStreamIds=mSids,.DefaultStreamCount=ARRAY_SIZE(mSids),
  .ExternalIoMmuOwnsPcieStreams=FALSE,
  /* HDK8350 PCIe110 lahaina.txt: both RCs read SCTLR=0x27 after 0x1027.
   * Public arm-smmu requests S1_ASIDPNE without requiring its readback.
   * Accept only bit12 RAZ; translation and all fault controls stay checked. */
  .SctlrReadbackIgnoreMask=(1U<<12),
  .CoherentStreamIds=mCoherent,.CoherentStreamCount=ARRAY_SIZE(mCoherent),
  .UfsHciBase=0x1d84000,.UfsStreamIds=mUfsSids,.UfsStreamCount=ARRAY_SIZE(mUfsSids),
  .DisplayReservedBase=0xe1000000,.DisplayReservedBytes=0x2300000,
  .UsbSoc=0x8350,.UsbDevices=mUsbDevices,.UsbDeviceCount=ARRAY_SIZE(mUsbDevices),
};
CrTargetSmmuContext *CrTargetGetSmmuContext(VOID) { return &mSmmu; }
