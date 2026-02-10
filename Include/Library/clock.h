/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_time.h>
#include <oskal/cr_types.h>

#define CLOCK_NODE_BRANCH_HALT 0
#define CLOCK_NODE_BRANCH_HALT_ENABLE 1
#define CLOCK_NODE_BRANCH_HALT_DELAY 2
#define CLOCK_NODE_BRANCH_HALT_BYPASS 3
#define CLOCK_NODE_BRANCH_HALT_POLL 4

#define CLOCK_NODE_HALT_WAIT_TIMEOUT_US 200
#define CLOCK_NODE_HALT_REG_CBCR_CLK_OFF_MSK BIT(31)
#define CLOCK_NODE_HALT_REG_CBCR_NOC_FSM_STATUS_MSK GEN_MSK(30, 28)
#define CLOCK_NODE_HALT_REG_CBCR_NOC_FSM_STATUS_ON BIT(1)

#define CLOCK_NODE_BRANCH_VOTED BIT(7)

#define CLOCK_NODE_RCG_CMD_REGISTER_CMD_REG_OFFSET 0
#define CLOCK_NODE_RCG_CMD_REGISTER_CFG_REG_OFFSET 4
#define CLOCK_NODE_RCG_CMD_REGISTER_M_REG_OFFSET 8
#define CLOCK_NODE_RCG_CMD_REGISTER_N_REG_OFFSET 0xc
#define CLOCK_NODE_RCG_CMD_REGISTER_D_REG_OFFSET 0x10

#define CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK GEN_MSK(10, 8)
#define CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_MSK GEN_MSK(13, 12)
#define CLOCK_NODE_RCG_CMD_REGISTER_CFG_HW_CLK_CTL_MSK BIT(20)
#define CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_DUAL_EDGE_MSK BIT(13)
#define CLOCK_RCG2_UPDATE_TIMEOUT_US 500 // 500us in reference code

#define CLOCK_NODE_RCG_CMD_REGISTER_CMD_UPDATE_MSK BIT(0)
#define CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_OFF_MSK BIT(31)
#define CLOCK_NODE_RCG_CMD_REGISTER_CMD_ENABLE_MSK BIT(1)

#define CLOCK_NODE_GDSC_PWR_STS_ON BIT(0)
#define CLOCK_NODE_GDSC_PWR_STS_OFF BIT(1)
#define CLOCK_NODE_GDSC_PWR_STS_RET BIT(2)
#define CLOCK_NODE_GDSC_PWR_STS_RET_ON                                         \
  (CLOCK_NODE_GDSC_PWR_STS_ON | CLOCK_NODE_GDSC_PWR_STS_RET)
#define CLOCK_NODE_GDSC_PWR_STS_OFF_ON                                         \
  (CLOCK_NODE_GDSC_PWR_STS_ON | CLOCK_NODE_GDSC_PWR_STS_OFF)

/* Helper Macros */
#define CLOCK_CLOCK_REGISTER(ctx, cc_type, clk, register)                      \
  (ctx->ClockControllers[cc_type].Address +                                    \
   ctx->ClockControllers[cc_type].Clks[clk]->register)

#define CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(src, src_cfg, rcg_freq_hz, h, m, n)  \
  {.Source      = {.Source = (src), .Config = (src_cfg)},                      \
   .M           = (m),                                                         \
   .N           = (n),                                                         \
   .PreDiv      = (2 * (h) - 1),                                               \
   .FrequencyHz = (rcg_freq_hz)}

enum ClockRcg2Policy {
  CLOCK_RCG2_POLICY_CEIL,
  CLOCK_RCG2_POLICY_FLOOR,
};

typedef enum {
  CC_TYPE_GCC      = 0,
  CC_TYPE_VIDEO_CC = 1,
  CC_TYPE_GPU_CC   = 2,
  CC_TYPE_CAM_CC   = 3,
  CC_TYPE_DISP_CC  = 4,
  CC_TYPE_APSS_CC  = 5,
  CC_TYPE_MC_CC    = 6,
  CC_TYPE_MAX, // For termination
} ClockControllerType;

typedef enum {
  CLOCK_NODE_TYPE_GDSC,
  CLOCK_NODE_TYPE_BRANCH,
  CLOCK_NODE_TYPE_BRANCH_2,
  CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR,
  CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  CLOCK_NODE_TYPE_MAX = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
} ClockNodeType;

/* Clocks Structures */
typedef struct _ClockController    ClockController;
typedef struct _ClockDriverContext ClockDriverContext;
typedef struct _ClockNode          ClockNode;

typedef struct {
  UINT16 Source;
  UINT16 Config;
} ClockRcgSource;

typedef struct {
  ClockRcgSource Source;
  UINT16         M;
  UINT16         N;
  UINT16         PreDiv;
  UINT64         FrequencyHz;
} ClockRcgFreqTable;

typedef struct _ClockController {
  UINTN       Address;
  UINT32      Size;
  BOOLEAN     MemMapped;
  UINT32      ClkCount;
  ClockNode **Clks;
  UINT32      GdscCount;
  ClockNode **Gdscs;
} ClockController;

typedef struct _ClockNode {
  ClockNodeType    Type;
  CONST CHAR8     *Name;
  ClockController *ParentController;
  UINT16           ParentCount;
  ClockNode      **Parents;
  union {
    struct {
      /* Branch(2) info */
      UINT32 EnableRegister;
      UINT32 EnableMsk;
      UINT32 HwcgRegister;
      UINT32 HwcgMsk;
      UINT32 HaltRegister;
      UINT8  HaltCheckFlag;
      UINT32 HaltMsk;
    };
    struct {
      /* RCG(2) Info */
      UINT32             CmdRegister;
      UINT16             CmdRCfgOffset;
      UINT16             MNDWidth;
      UINT16             HIDWidth;
      BOOLEAN            HwClockCtrl;
      UINT8              FreqCount;
      ClockRcgFreqTable *FreqTable;
    };
    struct {
      /* Gdsc info */
      UINT32  GdscRegister;
      UINT32  PwrStsFlag;
      UINT16  GdscFlags;
      UINT32  ClampIoCtlRegister;
      UINT32  CollapseCtlRegister;
      UINT32  CollapseCtlMsk;
      UINT32  GdscHwCtrlRegister;
      UINT16  CxcCount;
      UINT32 *CxcRegisters;
    };
  };
} ClockNode;

typedef struct _ClockDriverContext {
  UINTN            ClockControllerCount;
  ClockController *ClockControllers;
} ClockDriverContext;

/* Debugcc Structures */
typedef struct {
  CONST CHAR8 *Name;
  UINT32       MuxSel;
  UINT32       PreDivVal;
} DebugccInfo;

typedef struct {
  CONST CHAR8  *Name;
  UINT32        DebugOffset;
  UINT32        PostDivOffset;
  UINT32        CbcrOffset;
  UINT32        SrcSelMsk;
  UINT32        SrcSelSft;
  UINT32        PostDivMsk;
  UINT32        PostDivSft;
  UINT32        PostDivVal;
  UINT32        PeriodOffset;
  UINT32        EnMsk;
  DebugccInfo **DebugClks;
} DebugccController;

typedef struct {
  UINT32              CtlReg;
  UINT32              StatusReg;
  UINT32              XoDiv4CbcReg;
  DebugccController **ClockControllers;
} DebugccDriverContext;

CR_STATUS ClockLibInit(OUT ClockDriverContext **ClockContext);
CR_STATUS ClockDeinit(VOID);

CR_STATUS ClockEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN UINT64 RateHz, IN BOOLEAN Enable);

CR_STATUS GetClockNode(
    IN ClockDriverContext *ClockContext, IN CONST CHAR8 *ClockName,
    IN OUT ClockControllerType *ControllerType, IN OUT UINT32 *ClockId,
    OUT ClockNode **TargetClockNode);

BOOLEAN ClockRcg2CheckEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode);

CR_STATUS ClockGdscEnable(IN ClockDriverContext *Context, IN ClockNode *Gdsc);
VOID      DebugccDumpAllClocksFreq(VOID);
CR_STATUS
DebugccMeasureClockRate(IN CONST CHAR8 *ClockName, OUT UINT64 *FrequencyHz);