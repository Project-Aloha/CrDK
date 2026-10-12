# SM8450 extended MDSS source checkpoint

DSC, DPHY/CPHY and bonded dual-DSI are separate registered library classes
with native INFs, Windows property sheets and managed SDK exports. The native
ClockExternal owner service and Windows revision3 interface accept their exact
byte, interface and rational pixel-clock plans.
The existing HDK single-DPHY/LT9611 path remains the deployed implementation.
Passing host tests and ARM64 object compilation do not establish panel support,
native takeover, OS DMA qualification or live external clock ownership.

| Module | Implemented behavior | Qualified limits |
| --- | --- | --- |
| `MdssDsc.c` / `MdssDscCrLib.h` | Immutable rate control, 128-byte PPS, DPU8.1 DSC1.2 encoder and pingpong programming, retained stop | DSC1.1/1.2 RGB444 table tuples 8bpc/8bpp, 10bpc/8bpp and 10bpc/10bpp; no fractional bpp or native chroma input |
| `MdssDsi.c` / `MdssDsiCrLib.h` | Full-raster validation, payload geometry, byte/symbol/VCO/pixel plan, DSI6G host programming | RGB888 video; 1–4 DPHY data lanes or 1–3 CPHY trios; one host or bonded hosts0/1; dual horizontal geometry must divide evenly |
| `DsiPhyExtended.c` | 5nm PLL and PHY4.3 DPHY/CPHY analogue/timing/lane programming, PLL0-master/PHY1-slave sharing | Exact SM8450 DSI/PHY windows; no CPHY single-controller split-link mode; caller supplies actual external byte/pixel clock ownership and honors fractional M/N |
| `MdssDsiPanel.c` / `MdssDsiPanelCrLib.h` | Immutable real panel power/reset/command/PPS composition and cache publication | Explicit board resources; delays retain microsecond reset precision; poisoned command cleanup physically fences reset/power |
| `MdssDsiCommands.c` | Embedded short/long packets, 64-byte FIFO, genuine DMA for long packets, DSC PPS and compression enable on each host | Write transport while video is off; no reads/BTA, TE, command-mode scanout or automatic panel sequence |
| `MdssDual.c` / `MdssDualCrLib.h` | DMA0/1 crop from one full-stride mapping, LM0/1, PP0/1, CTL0, DSI INTF1/2 synchronization, compression and complete start/stop composition | BGRX framebuffer and 8bpc DSC transport, maximum2560 pixels per mixer; full exclusive DCE/split-control ownership required |

`Controllers` describes bonded physical hosts, separate from DSC soft-slice and
hard-encoder counts. The PPS picture remains the complete panel picture.
The DPU compression timing changes the active horizontal payload, preserving
blanking. DPU8.1 does not receive DPU9 PIPE_ACTIVE/LAYER_ACTIVE writes.

CPHY uses16 payload bits per seven symbol cycles per trio. Its byte interface
clock is the byte clock; its pixel parent is VCO/PLL-post-divider times2/7 and
can require a rational M/N ratio. The current external-clock services own both host groups, retain complete
M/N/divider restore journals and guard ordinary clock callers from mutating
these leases. Register names or mode metadata cannot substitute for an actual
reserved owner and its current power/access prerequisites.

Every new status-returning IO operation checks the caller's current ownership
before access and accepts only exact synchronous success. Partial failures
retain touched hosts, PHYs, encoder state, panel ownership, command mapping and
external clocks. Stop fences the DPU first, restores an off-video command host
for panel termination, then withdraws clocks and PHY. Panel cleanup must be
able to reset its physical resources when a command engine is poisoned.

The caller must provide an actual panel profile and preparation sequence,
exclusive mappings/leases for all named engines, native-owner retirement,
independently qualified framebuffer and command IOVAs, permanent CPU aliases,
cache publication, power/rail/bandwidth ownership and external serialization.
The libraries never derive IOVA from a CPU pointer or physical address.

## Source matrix

Android paths below are relative to
`.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450/vendor/qcom/opensource/display-drivers/msm/`.
Mainline paths are relative to `.vscode/linux/drivers/gpu/drm/msm/`.

| Behavior | Primary source files |
| --- | --- |
| DSC tables/PPS | Android `sde_dsc_helper.c`; Linux `drivers/gpu/drm/display/drm_dsc_helper.c` |
| DSC1.2 registers/initial lines/routing | Android `sde/sde_hw_dsc_1_2.c`; mainline `disp/dpu1/dpu_hw_dsc_1_2.c` and `dpu_encoder.c` |
| CPHY payload and topology | Android `dsi/dsi_ctrl.c`, `dsi/dsi_display.c`, `dsi/dsi_phy_hw_v4_0.c`; mainline `dsi/dsi_host.c` |
| PHY timings and PLL/clock dividers | Android `dsi/dsi_phy_timing_calc.c`, `dsi/dsi_phy_timing_v4_0.c`, `dsi/dsi_pll_5nm.c`; mainline `dsi/phy/dsi_phy.c`, `dsi_phy_7nm.c` |
| Host and command packet registers | Android `dsi/dsi_ctrl_hw_cmn.c`, `dsi/dsi_ctrl_reg.h`; mainline `dsi/dsi_host.c`, `registers/display/dsi.xml` |
| DPU8.1 engines, synchronized split and compressed timing | Mainline `disp/dpu1/catalog/dpu_8_1_sm8450.h`, `dpu_hw_ctl.c`, `dpu_hw_lm.c`, `dpu_hw_top.c`, `dpu_hw_pingpong.c`, `dpu_hw_intf.c`, `dpu_encoder_phys_vid.c` |

The Android DSC1.2 RGB PPS writer leaves byte88 with a reused range byte.
The test uses the standard/Linux zero value when both native chroma flags are
clear, and checks every other PPS byte against the independently compiled
Android implementation.

## Validation receipts

Commands run from the workspace root:

```sh
python3 Platforms/CranePkg/Tests/run_mdss_dsc_tests.py --output .tmp/waipio-continuation/mdss-dsc40-agent4
python3 Platforms/CranePkg/Tests/run_mdss_dsi_extended_tests.py --output .tmp/waipio-continuation/mdss-dual41-agent-final
python3 Platforms/CranePkg/Tests/check_mdss_extended_arm64.py --output .tmp/waipio-continuation/mdss-extended-arm64-40-agent-final
```

DSC native and NT sanitizer tests compare2208 independent Android vectors and
exercise75 encoder-start and18 stop callback failure phases. Extended native
and NT sanitizer tests compare477 independently compiled Linux timing/Android
clock vectors, exercise3424 PHY/host,476 command and6636 complete-compositor
failure phases per ABI, and compare DPU timing/split outputs against unchanged
Linux functions. Explicit command tests reject stale or completion-time error
status, retain failed transfers, and preserve unrelated W1C interrupt status.
The object checker compiles nine actual implementation files for EFI ARM64 and
real WDK ARM64 with warnings treated as errors. It does not activate a panel.

Native compiler intrinsics remain explicitly bound where aggregate assignments
generate runtime memory helpers. The packaged XML converter preserves owned
command payloads and normalizes FF wait rows; its DSC bpc/bpp input is explicit,
so it never guesses private XML profile IDs. Native extended resource composition
uses actual EFI ClockExternal, RPMh, ICB, SMMU and IOMMU protocols, distinct read
mappings for pixels/commands and GCD-qualified reserved backing. The board panel
owner still supplies actual reset/power leases; this API does not infer pins
from a mode or publish a GOP/EBS witness by itself.

Windows `CrMdssExtendedClockWdm` references the actual permanent provider and
checks its D0/access/native grant and incarnation. Its exact plan path is tested
with the real permanent provider and ClockLib, separately from OS connection,
DMA and panel qualification. Current deployed HDK inventory describes the
LT9611 single-DPHY path; assigning another controller or changing the physical
panel requires an immutable board profile and corresponding assigned resources.

Current panel/native-caller checks:

```sh
python3 Platforms/CranePkg/Tests/run_mdss_panel_tests.py --output .tmp/waipio-continuation/mdss-panel48-delay-final
python3 Platforms/CranePkg/Tests/run_mdp_extended_panel_tests.py --output .tmp/waipio-continuation/mdp-extended-panel48-final3
python3 Platforms/CranePkg/Tests/run_mdp_extended_native_tests.py --output .tmp/waipio-continuation/mdp-extended-native49-final
```

Panel native/NT tests cover544 callback phases per ABI. The real XML test
covers the packaged LT9611 video and R66451 CPHY/DSC video assets, including
wait rows, owned commands and invalid compression geometry. New native frontend
checks include actual private ClockLib ownership and the portable hardware
cores; RPMh/ICB/IOMMU/board GPIO boundaries are injected protocol fixtures.
The native MDSS and LT9611 INFs now compile the production selected-profile
caller behind `PcdMdssHdkExtendedProfileEnable` (default FALSE). The HDK producer
reads the actual packaged LT9611 XML, validates its exact board mode and
publishes a selected-profile protocol plus the existing deferred MDSS protocol.
The resident LT9611 driver owns the same GPIO9/109 supplies, GPIO107 reset and
FIFO/MCU gate used by its legacy startup. It readbacks each pin, excludes foreign
bridge programming, and prevents deferred I2C accesses while powered off.
The actual selected entry then acquires native resources, publishes one physical
BGRX GOP, and provides retained power and EBS closure. The existing default HDK
panel remains selected until the opt-in path receives a separate build and test.

```sh
python3 Platforms/CranePkg/Tests/run_mdp_hdk_extended_profile_tests.py --output .tmp/waipio-continuation/mdp-hdk-profile52-final
python3 Platforms/CranePkg/Tests/run_mdp_extended_native_tests.py --output .tmp/waipio-continuation/mdp-extended-native52-final
python3 Platforms/CranePkg/Tests/run_mdp_extended_output_tests.py --output .tmp/waipio-continuation/mdp-extended-output52-final
python3 Platforms/CranePkg/Tests/run_lt9611_handoff_tests.py --output .tmp/waipio-continuation/lt9611-extended52-final
python3 Platforms/CranePkg/Tests/check_mdp_extended_native_arm64.py --output .tmp/waipio-continuation/mdp-extended-profile52-arm64-final
```

The combined actual producer/entry/GOP/private-clock/core suite covers516
failure, committed-effect, power, EBS and deferred-provider cases; the generic
extended native caller covers752 provider phases and the GOP owner177 lifetime
cases. Six actual EFI ARM64 frontend/owner objects compile. These fixtures model
GPIO, RPMh, ICB, SMMU and boot-service boundary effects; they establish source
behavior and retained ownership, not physical panel operation.

CPHY, bonded dual DPHY/CPHY and DSC still require their own actual physical panel
power/reset binding and a Windows extended resource/profile caller. The HDK
LT9611 GPIO binding cannot authorize another panel. The current native wire
cannot encode complete extended PHY/DSC topology, so its retained extended
snapshot is explicitly UNCERTAIN and Windows `CanClaim` remains unavailable.
No extended firmware handoff, hardware validation or DPU2D capability follows
from this opt-in single-DPHY caller checkpoint.
