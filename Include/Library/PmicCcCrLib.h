/* PM8150B CC observation, no ADSP/PD/VBUS-control dependency.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/SpmiSharedCrLib.h>
#include <ucsi/TypeC.h>
typedef struct { UINT32 Board; BOOLEAN Batteryless; } CR_PMIC_CC_TARGET;
#define CR_PMIC_CC_HDK8150 8150U
#define CR_PMIC_CC_HDK8245 8245U
typedef struct {
 CR_SPMI_BUS *Bus; CR_PMIC_CC_TARGET Target; CR_SPMI_IRQ Irqs[8];
 VOID (*Wake)(VOID *); VOID *WakeContext;
 CR_USB_TYPEC_STATE State; UINT8 Revision[6],Raw[8];
 CR_STATUS LastStatus; BOOLEAN Initialized,Stopping,Dirty,Valid;
} CR_PMIC_CC;
/* Caller serializes all operations, including IRQ Wake, through its shared
   arbiter adapter. No sleep, polling timer or ADSP transport in this core. */
CR_STATUS CrPmicCcStart(CR_PMIC_CC *,CONST CR_PMIC_CC_TARGET *,CR_SPMI_BUS *,VOID (*)(VOID *),VOID *);
CR_STATUS CrPmicCcService(CR_PMIC_CC *);
CR_STATUS CrPmicCcGet(CR_PMIC_CC *,CR_USB_TYPEC_STATE *);
CR_STATUS CrPmicCcStop(CR_PMIC_CC *);
