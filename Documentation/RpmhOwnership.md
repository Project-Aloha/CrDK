# Waipio RPMh rail ownership

`RpmhCrDxe` is a standalone Crane resource owner. It obtains each regulator's
VRM address from `CmdDBCrDxe`, initializes the target `RpmhDeviceContext` with
`RpmhLibInit`, and submits active-set commands through Crane's local RPMh DRV
TCS path. It does not locate or call Qualcomm NPA, PMIC VREG, ICB, or any
packaged RPMh driver protocol.

The driver reads the unique RPMh supply IDs from the immutable PCIe target
(`ldob5`, `ldob6`, `ldoh2`, `ldob7`, `ldob9` and M.2 prerequisite `smpb10`
on SM8450), plus the CrDAL shared rail list. Additional supplies resolve their
CmdDB address when acquired; a nonzero address is required before any vote. Voltage, mode, and enable operations
translate directly to the corresponding `RpmhLib` command; the driver retains
the request state if a nonzero command reports an error because the hardware
may already have applied it.

`AcquireVreg` reserves one reference before issuing voltage, mode, and enable
commands. Shared callers must use the same voltage and mode. The final
`RpmhEnableVreg(FALSE)` releases enable, mode, and voltage in that order and
stops at the first failure so a caller can retry. This keeps the shared
`ldob6` rail safe when PCIe, UFS, and display bring-up overlap.

`RpmhWrite` is the raw synchronous Crane RPMh entry point and forwards to
`RpmhLib`; it is available for Crane interconnect BCM batches and PIL proxy votes. All mutable
state and command submission are serialized at `TPL_NOTIFY`.

The current source and host tests prove the CmdDB lookup, direct TCS dispatch,
reference counting, and rollback rules. A successful build does not prove that
the board's PMIC accepted a vote; board validation still requires RPMh traces
and rail measurements across cold PCIe, UFS, and display initialization.

`PILCrDxe` owns the ADSP-specific `lcx.lvl` and `lmx.lvl` ARC proxy votes,
plus `xo.lvl`. It resolves addresses and supported ARC indices through CmdDB,
uses synchronous active-set `RpmhWrite`, and holds prerequisites until PAS
shutdown succeeds. It never sends ARC indices through the VRM regulator
API. The current Crane clock provider does not vote `xo.lvl`; another future
XO voter must aggregate requests rather than write a competing raw vote.
Failed submissions retain PIL ownership so shutdown/unload can retry.

The immutable `CrDalDeviceRpmhTopology` describes the HDK input supplies from
mainline `sm8450-hdk.dts`. L1B depends on S11B (968mV/AUTO), L6B and L9B on
S12B (1352mV/AUTO), and L2B/L5B/L7B/L1C/L2H on PM8350C BOB
(3296mV/AUTO). The SMPS requests round the mainline minima up to 8mV
selectors; the BOB request uses the Android default within the mainline range.
SMPS/BOB AUTO is hardware mode 6, whereas LDO HPM is mode 7.

Each active child owns one parent reference, independent of the number of
consumers sharing that child. Parent voltage/mode/enable are submitted before
the first child transaction. The last child withdrawal completes before its
parent lease is released; any failed child or parent withdrawal keeps the
relationship owned for retry. Duplicate children, missing descriptions,
inconsistent shared-parent tuples and cycles are rejected before publication.
Incomplete transactions cannot be shared by another successful acquisition.

HDK schematic sheets 23/33 identify L2B as the PM8350BH PDPHY/VCONN supply
as well as the AP USB HS supply. `UCSICrDxe` owns an L2B reference before
PMIC negotiation and releases it on rollback, unload and ExitBootServices;
the USB PHY shares it after role detection. PCIe1 additionally owns S10B,
which enables the external M.2 BOB and supplies the selectable slot IO rail
(sheets 52/64). The external `VREG_BOB_3P3` converter is a different circuit
from the PM8350C `bobc1` resource. The DC-fed external 3.3V buck is controlled
by hardware switches, and a successful RPMh vote does not measure M.2 VCC.
