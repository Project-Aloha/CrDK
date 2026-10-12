# Charging screen

`ChargerCr` stays in UEFI boot services and disables the boot watchdog. The
HDK boot policy launches it from its firmware volume when ABL's captured boot
arguments identify charger mode. It can also be started manually from a shell.

The application draws a battery icon, animates charging, and shows the integer
state of charge beneath the icon. Unknown or stale battery data shows `--%`.
The battery protocol supplies copied observations; the application performs no
battery RPCs. A plain 100 ms event drives the loop at `TPL_APPLICATION`, with no
event callback retaining application storage.

After 30 seconds without button activity, the physical MDSS display owner stops
DPU scanout and the DSI video engine. It retains the original framebuffer,
mapping, clocks, PHY and bridge for wake. An IRQ-observed short power press wakes that output even if it has already
been released by the next application poll. The non-destructive physical press
sequence is separate from navigation key repeats.
A continuously observed power hold longer than three seconds invokes a cold
reboot; unknown button samples or a gap over 250 ms break the hold. The existing
navigation key mappings remain unchanged.

The app uses a display-power protocol and GOP on the same physical handle.
The independent DisplayPort GOP is not powered by this primary owner's protocol.
On HDK8450, the LT9611 external monitor controls its own backlight; stopping DSI
output does not claim ownership of that backlight or of a PMIC backlight rail.
Failed output stops retain an uncertain journal and are retried before wake.

Validation:

```sh
python3 Platforms/CranePkg/Tests/run_charger_app_tests.py --output .tmp/charger-tests
python3 Platforms/CranePkg/Tests/run_buttons_tests.py
python3 Platforms/CranePkg/Tests/run_mdp_gop_publish_tests.py
```

These tests execute the actual app, state machine, raster, button owner and MDSS
adapter under sanitizers. They establish timing, error and storage behavior;
hardware battery readings and display idle/wake still require a board test.
