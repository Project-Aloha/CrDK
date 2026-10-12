/* Native SM8350 ADSP contract from supplied public Android lahaina DTS.
 * PAS1, fixed reserved carveout, ADSP SMP2P and AOSS QMP. Firmware discovery
 * uses the existing UEFI UFS-FAT image directory policy; the files remain
 * PAS-authenticated board firmware and are never embedded by this target.
 * SPDX-License-Identifier: MIT */
#include <Library/CrTargetPilLib.h>
STATIC CONST CR_PIL_TARGET mTarget={
  .PasId=1,.MemoryBase=0x86100000,.MemorySize=0x2500000,
  .SmemHost=2,.SmemIn=443,.SmemOut=429,.IpccSend=0x40800c,.IpccClient=3,
  .QmpBase=0xc300000,.QmpSize=0x400,.ImagePath=L"\\image\\adsp",
  .InterconnectProvider="qcom,sm8350-pil",.CryptoSource=0x7000,.CryptoDestination=0x7001,
  .ProxyMinimumLevel=384,
};
CONST CR_PIL_TARGET *CrSm8350GetPilTarget(VOID){return &mTarget;}
