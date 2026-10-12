# SM8450 shared display interrupt observation

`CrMdssIrqSourcesQuiet` checks the interrupt enable registers needed before the
SM8450 MDSS parent interrupt can be connected for the two implemented TOP0
leaves: WB2 completion and DSI0 video vblank. It reads registers and barriers;
it never writes an enable or acknowledgement register.

The caller must retain the six actual assigned, powered register views, retire
foreign register writers, and serialize this call against its own interrupt
enable/acknowledgement operations. A successful observation does not establish
any of those conditions. The Windows PCFG6 child inventory supplies missing
register descriptors; it does not provide an IRQ, DMA or power grant.

| View | Physical base/span | Registers checked, relative to view |
|---|---|---|
| MDSS parent | `AE00000/1000` | Final `HW_INTR_STATUS 10` |
| DPU | `AE01000/8F000` | TOP0 enable/status `10/14`, TOP0_2 enable `08`, HIST enable `1C`, INTF0..3 enables `341C0..371C0`, TEAR1/2 enables `35800/36800`, LTM enables `69350/6A350` |
| DSI0 | `AE94000/400` | Physical interrupt control `110`, mask `A222AA02` |
| DSI1 | `AE96000/400` | Physical interrupt control `110`, mask `A222AA02` |
| DP0 AHB | `AE90000/200` | Packed enables in `20/24/28/34`, separate PSR mask `30` |
| DP0 AUX | `AE90200/200` | HPD mask `0C`, bits0..3 |

Use `AllowedTop0=0` before physical connection. Other callers may permit only
WB2 bit4 and/or DSI0 vblank bit27. All other known enables must be clear. A final
MDP parent bit is accepted only when enabled, pending, permitted TOP0 status
explains it. Other parent bits always refuse the observation. Disabled latched
status is preserved; it is not evidence that an interrupt is enabled.

On refusal, the diagnostic identifies the first offending register. On any IO
failure, the output remains unchanged. Nonfinal successful callback statuses
are normalized to `CR_NOT_READY`. The observation is transient and cannot be
stored as a future authorization token.

## Source qualification

The source inventory deliberately combines mainline and the Android SM8450
driver. Mainline alone omits target features which may have enabled IRQs before
the Windows owner starts.

- Linux commit `a53261b60834ce617cc4f3c7723dda8285c59789`:
  `arch/arm64/boot/dts/qcom/sm8450.dtsi`,
  `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_interrupts.c`,
  `catalog/dpu_8_1_sm8450.h`, `registers/display/dsi.xml`,
  `dsi/dsi_cfg.h` and `dp/dp_reg.h`.
- OnePlus modules/devicetree commit
  `877203a7cf9b79973d44a9db716490f95d820110`:
  `vendor/qcom/opensource/display-drivers/msm/dsi/dsi_ctrl_hw_cmn.c`,
  `dsi_catalog.c`, `dsi_ctrl_reg.h`; `dp/dp_catalog.h`, `dp_catalog.c`,
  `dp_hdcp2p2.c`; `sde_hdcp_1x.c`; and `sde/sde_hw_catalog.c`,
  `sde_hw_interrupts.c`.
- The same Android commit's
  `vendor/qcom/proprietary/display-devicetree/display/waipio-sde-common.dtsi`,
  `waipio-sde.dtsi`, and HDK display include chain supply the target inventory.
  These Git blobs can be read even when outside the local sparse checkout.

Android DSI enables command stream0..2 at bits11/13/15 and dynamic refresh and
deskew at bits29/31, in addition to the older mainline XML's bits1/9/17/21/25.
Both controllers use the common interrupt callback. Error submasks at physical
`10C` do not cause a refusal when their interrupt-control master is disabled.

Waipio defines two LTM instances. Their MDSS-relative DSPP bases `55000/57000`
plus LTM offsets `15300/14300`, plus IRQ enable `50`, minus the DPU view offset
`1000`, produce `69350/6A350`. The known enables are statistics-done bit0 and
statistics writeback ping-pong bit5. Generic legacy AD4 offsets and generic
INTF4..8 registers are not inferred from another SoC's catalog.

DP STATUS2 includes HDCP masks17/20/24/27; both Android HDCP1 and HDCP2.2 use
these. HDCP2.2 also uses STATUS3 bit2. PSR uses the separate mask at30 and MST
uses packed masks2/5 at34. The ordinary AUX/link and AUX HPD masks remain part
of the same check.

## Validation and remaining integration

`Tests/run_mdss_irq_sources_tests.py --output <directory>` compiles and executes
the actual source in native and NT sanitizer fixtures. It checks every bit in
the permitted read inventory, all supported TOP0 enable/pending combinations,
foreign parent status, no writes, output preservation and every read/barrier
failure position. The receipt hashes the source files and immutable target DT
blobs. Independent review retained failing pre-fix demonstrations for the DSI
and LTM omissions.

The Windows `Common/CrMdssIrqRootWdm.c` owner now maps/borrows these six views
under optional PCFG6 assignment. It checks native writer retirement and current
clock/rail/ICB ownership, adds the DP AUX clock vote, and disconnects/drains IRQ
callbacks before releasing resources. Its first consumer is extended-display
INTF1 vblank; no WB2 job is submitted. The default PCFG2/PFLG14 profile still
omits these resources. Host and full PEP WDK checks validate implementation,
not physical IRQ behavior. The qualifier alone grants no resource or DMA access.
