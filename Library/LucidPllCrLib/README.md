# SM8250 direct Lucid PLL owner

This unused library implements the selected HDK8245 DISPCC PLL1 lifetime at
physical `0x0af01000`, for the 600 MHz parent used by its 200 MHz MDP clock.
It is not selected by a package, target, INF or Windows property sheet yet.
It must not be substituted for an FSM-vote PLL or a Lucid EVO PLL.

Register ordering and constants come from the pinned Qualcomm Android sources:

- [clk-alpha-pll.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8250/blob/cad09e061ef6cf689a4a1e54d27562e0f042236a/drivers/clk/qcom/clk-alpha-pll.c)
- [dispcc-kona.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8250/blob/cad09e061ef6cf689a4a1e54d27562e0f042236a/drivers/clk/qcom/dispcc-kona.c)

Derived source is GPL-2.0-only and retains the Qualcomm copyright attribution.
Current Linux Lucid code is a cross-check, not an interchangeable configuration:
its upper configuration word and calibration-status register differ.

The native provider must first retain access to inherited display hardware,
fence real DPU/VBIF and host DMA, stop inherited dependents, and produce a genuine
SM8250 retirement claim. Only then may this owner program PLL1. It never creates
that claim and cannot bootstrap inherited display retirement itself.

All runtime calls require passive execution and external serialization. In
particular, the existing ClockLib `CR_LOCK` raises Windows IRQL and cannot
surround this library's real RPMh provider callbacks. Shared clock integration
needs a retained passive transition before this library can be selected.

Two independent provider tokens represent MMCX votes: common level 192 for the
source prepare cycle and board-clamped level 64 for steady operation. These
are not ARC indices. The provider resolves CMDDB, votes both selected board
sets, and retains MMCX's real MX/CX dependencies. MX's minimum is the actual
CMDDB-selected MMCX level minus one, subsequently rounded through MX CMDDB.
An allocated failed token is not proof that its requested voltage was applied.

Acquisition always executes the source's voltage-qualified RUN/STANDBY cycle;
it never infers inherited calibration from bit 27. A fresh lock-low/lock-high
observation qualifies each RUN. The cycle does not prove that hardware reran
analog calibration: the source disable path leaves RESET_N high. Runtime then
holds the steady token before withdrawing the calibration token. Failures keep
all outstanding ownership until explicit release succeeds.

Release gates global and per-output clocks, enters STANDBY, and checks those
controls before dropping votes. PLL lock may remain asserted and is not used
as the shutdown acknowledgement. Generic parent references, source reservation,
provider incarnation and final native rail/MMIO withdrawal must all respect
the retained cleanup state.

`Tests/run_lucid_pll_tests.py` compiles the actual library against original
Android configure/enable/disable function bodies, exercises every successful
callback position with failure before and after side effects, and compiles
native/NT sanitizer variants plus an AArch64 object. Hardware voltage, lock
behavior and visible panel output still require device validation.
