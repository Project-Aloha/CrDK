/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssDsiCmdCrLib.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>
#define CMDBIT(n) (1U << (n))
CR_STATUS CmdPlanCheck(CR_MDSS_CMD *);
CR_STATUS CmdReady(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW);
CR_STATUS CmdRead(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW, UINT32, UINT32 *);
CR_STATUS CmdWrite(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW, UINT32, UINT32);
CR_STATUS CmdUpdate(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW, UINT32, UINT32 Mask, UINT32 Value);
CR_STATUS CmdChecked(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW, UINT32, UINT32 Value, UINT32 Mask);
CR_STATUS CmdFence(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW);
CR_STATUS CmdDelay(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW, UINT32 Us);
CR_STATUS CmdPoll(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW, UINT32, UINT32 Mask, UINT32 Value, UINT32 Us);
CR_STATUS CmdDmaReady(CR_MDSS_CMD *, CR_MDSS_CMD_WINDOW);
CR_STATUS CmdPhyStart(CR_MDSS_CMD *);
CR_STATUS CmdPhyStop(CR_MDSS_CMD *);
CR_STATUS CmdHostStart(CR_MDSS_CMD *);
CR_STATUS CmdHostStop(CR_MDSS_CMD *);
CR_STATUS CmdPacket(CR_MDSS_CMD *, UINT8 Type, CONST UINT8 *, UINTN, BOOLEAN LowPower);
CR_STATUS CmdPanelOn(CR_MDSS_CMD *);
CR_STATUS CmdPanelOff(CR_MDSS_CMD *);
/* DPU/DSC author owns only these source units. Start called while host/PHY,
 * frame/command mappings and all native providers are qualified. DPU start
 * establishes the route/TE/autorefresh; Active becomes true in Lifecycle only
 * after first real frame-completion. Stop must fence DMA before clearing its
 * touched/inflight flags; IRQ ownership is drained by Lifecycle first. */
CR_STATUS CmdDscStart(CR_MDSS_CMD *);
CR_STATUS CmdDscStop(CR_MDSS_CMD *);
CR_STATUS CmdDpuStart(CR_MDSS_CMD *);
CR_STATUS CmdDpuStop(CR_MDSS_CMD *);
CR_STATUS CmdDpuIrq(CR_MDSS_CMD *, UINT32 *Events);
