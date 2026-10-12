/* Native panel suppliers owned by the common MDSS frontend.
 * Target data identifies resources; it does not grant PMIC voltage authority.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
#define MDP_PANEL_POWER_CONFIG_REVISION 1U
#define MDP_PANEL_POWER_MAX_RAILS 4U
#define MDP_PANEL_POWER_RAIL_NAME 16U
typedef enum {
  /* Keep the provider's inherited voltage/mode and permanent enable floor.
   * Nominal values below are panel intent, never a measured voltage or a
   * headroom grant. This policy never calls AcquireVreg or a legacy setter. */
  MdpPanelRetainInheritedEnable=1,
  /* Explicit board-qualified fixed supplier contract. NominalMillivolts and
   * NominalMode are the exact positive vote, shared with other consumers.
   * It remains asserted for the lifetime of the retained panel owner. */
  MdpPanelRetainFixedVote=2
} MDP_PANEL_RAIL_POLICY;
typedef enum {
  /* Selected SWIRE supplies have no qualified physical OFF witness. Keep
   * the complete native owner and all provider prerequisites resident. */
  MdpPanelSwirePermanentRetention=1
} MDP_PANEL_BIAS_POLICY;
typedef struct {
  CHAR8 Name[MDP_PANEL_POWER_RAIL_NAME];
  UINT32 Policy,NominalMillivolts,PostOnDelayUs;
  UINT8 NominalMode;
} MDP_PANEL_POWER_RAIL;
typedef struct {
  UINT32 Revision,Size,RailCount;
  MDP_PANEL_POWER_RAIL Rails[MDP_PANEL_POWER_MAX_RAILS];
  GpioConfigParams Reset,Te;
  BOOLEAN HasTe;
  UINT32 BiasPolicy,PostBiasDelayUs;
  /* Public target identity only. No SPMI role, register read/write or physical
   * AB/IBB ON/OFF observation follows from these three address fields. */
  UINT8 BiasSid;
  UINT16 AbBase,IbbBase;
} MDP_PANEL_POWER_CONFIG;
typedef EFI_STATUS (*MDP_PANEL_POWER_READY)(VOID *);
typedef EFI_STATUS (*MDP_PANEL_POWER_DELAY)(VOID *,UINT32);
typedef struct {
  VOID *Self,*NativeContext;
  EFI_RPMH_CR_PROTOCOL *Rpmh;
  EFI_GPIO_CR_PROTOCOL *Gpio;
  MDP_PANEL_POWER_READY NativeReady;
  MDP_PANEL_POWER_DELAY DelayUs;
  EFI_RPMH_CR_ENSURE_RETAINED_VREG_ENABLED RetainEnable;
  EFI_RPMH_CR_ENSURE_RETAINED_VREG RetainFixed;
  EFI_GPIO_CR_ACQUIRE_PIN AcquirePin;
  EFI_GPIO_CR_SET_PIN_VALUE SetPin;
  EFI_GPIO_CR_READ_PIN_VALUE ReadPin;
  MDP_PANEL_POWER_CONFIG Config;
  BOOLEAN Initialized,PermanentHold,Closing,PowerReady;
  BOOLEAN PinOwned[2],PinReady[2],RailAttempted[MDP_PANEL_POWER_MAX_RAILS];
  BOOLEAN RailReady[MDP_PANEL_POWER_MAX_RAILS],RailSettled[MDP_PANEL_POWER_MAX_RAILS],BiasSettled;
} MDP_PANEL_POWER;
/* Pure constructor. Stable zeroed native storage and external serialization.
 * NativeReady checks the real retained access/clock/provider incarnation.
 * Its context and all providers remain alive until exact successful Close.
 * A failed post-effect operation retains the same storage and prerequisites. */
EFI_STATUS MdpPanelPowerInit(MDP_PANEL_POWER *,EFI_RPMH_CR_PROTOCOL *,EFI_GPIO_CR_PROTOCOL *,
    CONST MDP_PANEL_POWER_CONFIG *,VOID *,MDP_PANEL_POWER_READY,MDP_PANEL_POWER_DELAY);
/* Bind before inherited reset retirement when that route needs this GPIO
 * owner. Partial AcquirePin ownership is retained and retried idempotently. */
EFI_STATUS MdpPanelPowerBindPins(MDP_PANEL_POWER *,BOOLEAN *Owned);
EFI_STATUS MdpPanelPowerReady(VOID *);
EFI_STATUS MdpPanelPowerSet(VOID *,BOOLEAN On,BOOLEAN *Owned);
EFI_STATUS MdpPanelPowerReset(VOID *,BOOLEAN High);
/* Once a pin or permanent rail floor may have been touched, FALSE and Close
 * refuse withdrawal. Reset LOW is not an AB/IBB OFF witness. Successful EBS
 * retains this owner; normal unload must retain it after the refusal too. */
EFI_STATUS MdpPanelPowerClose(MDP_PANEL_POWER *);

/* Exact supplied R66451 board resources; selected explicitly by the frontend. */
EFI_STATUS MdpPanelPowerR66451Config(MDP_PANEL_POWER_CONFIG *);
