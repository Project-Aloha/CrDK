/* Public sm8350-hdk.dts and supplied Android lahaina HDK:
 * IPCC@408000 GIC_SPI229, LPASS3/GLINKsignal0 and GPIO81 orientation.
 * Android ucsi_glink/pmic_glink binds PMIC_RTR_ADSP_APPS and charger_pd.
 * No AP-direct PD rail consumer is described. SPDX-License-Identifier: MIT */
#include <Library/UcsiTargetLib.h>
STATIC CONST CR_UCSI_TARGET mTarget={0x8350,8350,0x408000,261,81,TRUE,TRUE,NULL,0,0};
CONST CR_UCSI_TARGET *CrUcsiGetTarget(VOID){return &mTarget;}
