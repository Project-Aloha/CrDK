/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
#include <Library/CrTargetRpmhLib.h>
#include <Library/cmddb.h>
#include <Library/rpmh.h>

#include <oskal/cr_assert.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_interrupt.h>
#include <oskal/cr_status.h>
#include <oskal/cr_string.h>
#include <oskal/cr_types.h>

#define RPMH_REGULATOR_VOLTAGE_REG 0
#define RPMH_REGULATOR_ENABLE_REG 4
#define RPMH_REGULATOR_MODE_REG 8

#define RPMH_MAX_CMDS_EACH_TCS 16

#define RPMH_DRV_ID_MAJOR_MSK GEN_MSK(23, 16)
#define RPMH_DRV_ID_MINOR_MSK GEN_MSK(15, 8)

#define RPMH_TCS_CMD_MSG_ID_LEN 8
#define RPMH_TCS_CMD_MSG_ID_WRITE_FLAG_BIT BIT(16)
#define RPMH_TCS_CMD_MSG_ID_RESPONSE_REQUEST_BIT BIT(8)
#define RPMH_TCS_CMD_STATUS_ISSUED_BIT BIT(8)
#define RPMH_TCS_CMD_STATUS_COMPLETED_BIT BIT(16)

#define TCS_AMC_MODE_TRIGGER BIT(24)
#define TCS_AMC_MODE_ENABLE BIT(16)

#define RSC_DRV_TCS_NUM_MSK GEN_MSK(11, 6)
#define RSC_DRV_NCPT_MSK GEN_MSK(31, 27)

VOID RpmhDrvTcsTxDoneIsr(VOID *Params);

STATIC inline UINT32
ReadTcsReg(RpmhDeviceContext *RpmhContext, UINT32 Reg, UINT32 TcsIndex)
{
  return CrMmioRead32(
      RpmhContext->drv_base_address + RpmhContext->tcs_offset +
      RpmhContext->drv_registers->reg_rsc_drv_tcs_offset * TcsIndex + Reg);
}

STATIC inline UINT32 ReadTcsCmdReg(
    RpmhDeviceContext *RpmhContext, UINT32 Reg, UINT32 TcsIndex,
    UINT32 CmdIndex)
{
  return CrMmioRead32(
      RpmhContext->drv_base_address + RpmhContext->tcs_offset +
      RpmhContext->drv_registers->reg_rsc_drv_tcs_offset * TcsIndex + Reg +
      RpmhContext->drv_registers->reg_rsc_drv_cmd_offset * CmdIndex);
}

STATIC inline VOID WriteTcsCmdReg(
    RpmhDeviceContext *RpmhContext, UINT32 Reg, UINT32 TcsIndex,
    UINT32 CmdIndex, UINT32 Value)
{
  CrMmioWrite32(
      RpmhContext->drv_base_address + RpmhContext->tcs_offset +
          RpmhContext->drv_registers->reg_rsc_drv_tcs_offset * TcsIndex + Reg +
          RpmhContext->drv_registers->reg_rsc_drv_cmd_offset * CmdIndex,
      Value);
}

STATIC inline VOID WriteTcsReg(
    RpmhDeviceContext *RpmhContext, UINT32 Reg, UINT32 TcsIndex, UINT32 Value)
{
  CrMmioWrite32(
      RpmhContext->drv_base_address + RpmhContext->tcs_offset +
          RpmhContext->drv_registers->reg_rsc_drv_tcs_offset * TcsIndex + Reg,
      Value);
}

// 1ms max wait(unit us)
#define RPMH_WRITE_MAX_WAIT_TIME 1 * 1000
STATIC inline VOID WriteTcsAsync(
    RpmhDeviceContext *RpmhContext, UINT32 Reg, UINT32 TcsIndex, UINT32 Value)
{
  WriteTcsReg(RpmhContext, Reg, TcsIndex, Value);
  CR_MEM_BARRIER_DATA_SYN_BARRIAR();
}
