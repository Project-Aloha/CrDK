/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once
#include <Library/FixedPllVoteLib.h>

#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_lock.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_time.h>
#include <oskal/cr_types.h>

#define CLOCK_NODE_BRANCH_HALT 0
#define CLOCK_NODE_BRANCH_HALT_ENABLE 1
#define CLOCK_NODE_BRANCH_HALT_DELAY 2
#define CLOCK_NODE_BRANCH_HALT_SKIP 3
#define CLOCK_NODE_BRANCH_HALT_BYPASS CLOCK_NODE_BRANCH_HALT_SKIP
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
#define CLOCK_NODE_RCG_CMD_REGISTER_CMD_ROOT_EN_MSK BIT(1)

#define CLOCK_NODE_GDSC_PWR_STS_ON BIT(0)
#define CLOCK_NODE_GDSC_PWR_STS_OFF BIT(1)
#define CLOCK_NODE_GDSC_PWR_STS_RET BIT(2)
#define CLOCK_NODE_GDSC_PWR_STS_RET_ON                                         \
  (CLOCK_NODE_GDSC_PWR_STS_ON | CLOCK_NODE_GDSC_PWR_STS_RET)
#define CLOCK_NODE_GDSC_PWR_STS_OFF_ON                                         \
  (CLOCK_NODE_GDSC_PWR_STS_ON | CLOCK_NODE_GDSC_PWR_STS_OFF)
#define CLOCK_NODE_GDSC_FLAG_VOTABLE BIT(0)
#define CLOCK_NODE_GDSC_FLAG_RETAIN_FF_EN BIT(4)
#define CLOCK_NODE_GDSC_FLAG_HW_CTL       BIT(5)

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
  /* A regular register mux has no gate operation of its own. */
  CLOCK_NODE_TYPE_MUX,
  /* Qualcomm PHY mux: 0 selects the PHY-generated clock, 2 selects XO. */
  CLOCK_NODE_TYPE_PHY_MUX,
  CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR,
  CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  /* Fixed firmware-configured FSM PLL, retained local vote/readback only. */
  CLOCK_NODE_TYPE_FIXED_PLL,
  /* Register divisor is observed only; no set-rate or gate writes. */
  CLOCK_NODE_TYPE_READ_ONLY_DIVIDER,
  /* Firmware-owned source table; consumers select a qualified level locally.
     The target check reads its contract; acquiring this node writes no gate. */
  CLOCK_NODE_TYPE_READ_ONLY_SOURCE,
  CLOCK_NODE_TYPE_MAX = CLOCK_NODE_TYPE_READ_ONLY_SOURCE,
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

/* Target hook used when an RCG selects a source with a target-owned
 * analogue block (for example SM8450 DISPCC PLL0/PLL1).  Generic clock code
 * still owns the RCG transaction; the hook prepares the selected parent. */
typedef CR_STATUS (*CLOCK_SOURCE_PREPARE)(
    IN ClockDriverContext *ClockContext,
    IN ClockNode *TargetClockNode,
    IN CONST ClockRcgFreqTable *TargetFreq);
typedef CR_STATUS (*CLOCK_READ_ONLY_SOURCE_CHECK)(
    IN ClockNode *TargetClockNode, IN UINT64 RateHz);

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
  /* Runtime ownership state.  The first claimant records the pre-existing
     control bit so the final release does not disable firmware-owned state. */
  UINT32           ReferenceCount;
  BOOLEAN          RestoreControlEnabled;
  BOOLEAN          RestoreHwControlled;
  BOOLEAN          ReleasePending;
  /* Branch dependency votes form an acquired prefix. Retain its exact length
     and gate ownership across failed enable rollback/final disable; retry
     must not release a parent reference already withdrawn successfully. */
  UINT16           ParentsHeld;
  BOOLEAN          GateHeld;
  /* Exact RO divisor snapshot, not merely the rounded output frequency. */
  UINT32           ActiveDivider;
  /* An RCG may be configured while its leaf branch is gated.  Preserve that
     external source selection on the subsequent cold enable. */
  BOOLEAN          ExternalConfigPending;
  /* A table-rate update is not qualified by UPDATE clearing alone. Preserve
     the exact masked pre-write image until all divider/source readbacks agree
     or rollback is acknowledged. Failed restoration pins this RCG and every
     acquired ancestor; cleanup retries the same image before withdrawing it. */
  BOOLEAN          RcgRestorePending;
  UINT32           RcgSavedValue[4];
  UINT32           RcgSavedMask[4];
  UINT64           ActiveRateHz;
  UINT32           DividerRegister; /* optional separate post-divider */
  UINT8            DividerWidth;
  BOOLEAN          DividerReadOnly;
  UINT8            RequiredDivider; /* fixed target contract; zero permits any */
  /* Immutable parallel edge rates. NULL/zero forwards the requested rate;
     fixed-PLL parents retain their exact described rate in that default case.
     These descriptions must outlive every reference and cleanup retry. */
  CONST UINT64    *ParentRates;
  UINT64          FixedRateHz; /* zero unrestricted; enable/validation only */
  UINT64          DividerInputRateHz; /* RO node input; ceil(input/(raw+1)) */
  /* Branch AON policy: final withdrawal never writes its control/status,
     while its own dependency references are still released. */
  BOOLEAN         KeepEnabled;
  union {
    CR_FIXED_PLL_VOTE FixedPll;
    struct {
      /* Branch(2) info */
      UINT32 EnableRegister;
      UINT32 EnableMsk;
      UINT32 HwcgRegister;
      UINT32 HwcgMsk;
      UINT32 HaltRegister;
      UINT8  HaltCheckFlag;
      UINT32 HaltMsk;
      /* Optional immutable edge policy: acquire parent I only when the
         requested child rate is at least ParentMinimumRates[I]. Zero means
         unconditional. The target must qualify this monotonic policy against
         its complete supported rate table and the core's CEIL selection.
         NULL preserves the ordinary acquired-prefix behavior. */
      CONST UINT64 *ParentMinimumRates;
      /* Exact acquired edges, including references retained by failed parent
         acquisition. ParentsHeld is the visited prefix when policy is present;
         skipped edges have no bit. Never reconstruct ownership from a rate. */
      UINT32 ParentHeldMask;
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
    struct {
      /* PHY mux info. The mux is owned by the clock provider, while the
       * selected source is supplied by the QMP PHY clock provider. */
      UINT32 MuxRegister;
      UINT32 MuxMask;
      UINT32 MuxPhyValue;
      UINT32 MuxRefValue;
      /* Ordinary-mux first acquisition saves only its selector bits. A
       * failed transaction retains its owner until exact restore readback;
       * a successful transaction commits normal persistent mux selection. */
      UINT32 MuxSavedValue;
      BOOLEAN MuxAcquirePending;
      BOOLEAN MuxRestorePending;
    };
  };
  /* Validate an inherited source without changing its firmware-owned state. */
  CLOCK_READ_ONLY_SOURCE_CHECK ReadOnlySourceCheck;
} ClockNode;

typedef struct _ClockDriverContext {
  UINTN            ClockControllerCount;
  ClockController *ClockControllers;
  CLOCK_SOURCE_PREPARE PrepareSource;
  VOID            *TargetState; /* private target instance, if supplied */
  CR_LOCK          Lock;
  BOOLEAN          LockInitialized;
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

CR_STATUS ClockLibInit(IN OUT ClockDriverContext **ClockContext);
CR_STATUS ClockDeinit(IN OUT ClockDriverContext *ClockContext);

CR_STATUS ClockEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN UINT64 RateHz, IN BOOLEAN Enable);
/* Existing ownership only: serialized fixed-source/RO-divider readbacks,
 * no new reference or programming. Used for an unchanged enabled vote. */
CR_STATUS ClockCheckAcquired(ClockDriverContext *Context,ClockNode *Node,UINT64 RateHz);

CR_STATUS ClockRcg2SetRate(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode,
    IN UINT64 RateHz, IN UINT8 Policy);

CR_STATUS ClockRcg2UpdateConfig(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode);

CR_STATUS GetClockNode(
    IN ClockDriverContext *ClockContext, IN CONST CHAR8 *ClockName,
    IN OUT ClockControllerType *ControllerType, IN OUT UINT32 *ClockId,
    OUT ClockNode **TargetClockNode);

CR_STATUS GetGdscNode(
    IN ClockDriverContext *ClockContext, IN CONST CHAR8 *GdscName,
    OUT ClockNode **TargetGdscNode);

BOOLEAN ClockRcg2CheckEnable(
    IN ClockDriverContext *ClockContext, IN ClockNode *TargetClockNode);

/* Divider is twice the source divisor for RCG2 (HID encoding is Divider-1).
 * M/N/TwiceD preserve fractional pixel clocks. Configure before enabling the
 * leaf; an ordinary mux may switch a single owner's live PHY/XO parent.
 * Failed RCG relatch restores registers and sets ReleasePending for retry.
 */
CR_STATUS ClockSelectExternalSource(ClockDriverContext *Context,ClockNode *Node,
                                    UINT32 Source,UINT32 Divider,
                                    UINT32 M,UINT32 N,UINT32 TwiceD);
CR_STATUS ClockSetDivider(ClockDriverContext *Context,ClockNode *Node,UINT32 Divider);
CR_STATUS ClockRecoverConfiguration(ClockDriverContext *Context,ClockNode *Node);

CR_STATUS ClockGdscEnable(IN ClockDriverContext *Context, IN ClockNode *Gdsc);
CR_STATUS ClockGdscDisable(IN ClockDriverContext *Context, IN ClockNode *Gdsc);
/* Serialized reference-counted GDSC ownership, restoring inherited collapse/HW
 * control on final release. Failed restore keeps the reference for retry. */
/** Acquisition reports a new reference, including retained failed cleanup.
 * Owned FALSE on error means the caller must not withdraw a foreign vote. */
CR_STATUS ClockAcquire (ClockDriverContext *Context, ClockNode *Node,
                        UINT64 RateHz, BOOLEAN *Owned);
/* Atomic first vote on an external PHY/reference mux. Rejects foreign or
 * retained node/selector owners before changing hardware. */
CR_STATUS ClockAcquireExternal (ClockDriverContext *Context, ClockNode *Node,
                                UINT64 RateHz, UINT32 Source, BOOLEAN *Owned);
CR_STATUS ClockGdscAcquire (ClockDriverContext *Context, ClockNode *Node,
                            BOOLEAN *Owned);

CR_STATUS ClockGdscSet(IN ClockDriverContext *Context, IN ClockNode *Gdsc,
                       IN BOOLEAN Enable);
/* Pure descriptor/range preflight for resource providers; no MMIO or votes. */
CR_STATUS ClockGdscValidate (ClockDriverContext *Context, CONST ClockNode *Gdsc);
VOID
DebugccDumpAllClocksFreq(
    IN ClockDriverContext *ClockContext,
    IN DebugccDriverContext *DebugccContext);
CR_STATUS
DebugccMeasureClockRate(
    IN ClockDriverContext *ClockContext,
    IN DebugccDriverContext *DebugccContext,
    IN CONST CHAR8 *ClockName,
    OUT UINT64 *FrequencyHz);
