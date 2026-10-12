# Waipio display XML key reference

The panel XML parser is a compatibility parser for the files packaged with
the platform. Its implementation uses the public Android/Linux display
contracts and does not load, call, or copy an OEM firmware driver. Register
programming and resource ownership are implemented from the Linux DRM MSM
drivers and the Android Waipio device-tree sources.

## DisplayResetInfo

The key contains five UINT32 values. Decimal and `0x`/`0X` hexadecimal tokens
are accepted, separated by whitespace or commas.

| Index | Meaning |
| --- | --- |
| 0 | Reset assert state/type |
| 1 | Reset assert duration, microseconds |
| 2 | Pre-reset duration, microseconds |
| 3 | Post-reset duration, microseconds |
| 4 | Assert-on-power-down field |

Zero duration values select defaults: assert=10, pre=1000 and post=10000
microseconds. The fifth field is retained for compatibility; its runtime
effect is not established by the public display contract.

The parser accepts the numeric-list grammar used by the Android panel
configuration files. For example, `1, 0x14 30,0X28, 0` represents
`1/20/30/40/0`. Parsing these fields does not implement a generic panel-reset
engine. The active LT9611 reset path is implemented separately from the
public Linux bridge sequence.

## Backlight keys

| Key | Values |
| --- | --- |
| `BacklightType` | none=0, PMIC=1, I2C=2, DSI=3, ACPI=4 |
| `BacklightPmicControlType` | none=0, LPG=1, WLED=2, PWM=3 |

The parser retains these values and their presence bits. Generic native-panel
backlight execution remains outside the current HDK8450 path.

## Public sources

The register and timing implementation is based on the Linux DRM MSM sources:

- `drivers/gpu/drm/msm/dsi/`
- `drivers/gpu/drm/msm/disp/`
- `drivers/gpu/drm/msm/registers/display/`
- `drivers/clk/qcom/dispcc-sm8450.c`

The Waipio rail and DSI host descriptions come from the public Android
device-tree sources:

- [waipio-sde.dtsi](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/oneplus/sm8450_s_12.1_10_pro/vendor/qcom/proprietary/display-devicetree/display/waipio-sde.dtsi)
- [waipio-sde-common.dtsi](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/oneplus/sm8450_s_12.1_10_pro/vendor/qcom/proprietary/display-devicetree/display/waipio-sde-common.dtsi)

These sources provide data and documented hardware contracts only. No
prebuilt display, I2C, GPI, clock, RPMh, or NPA driver is a dependency of the
Crane display path.
