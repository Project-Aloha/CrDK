# ABL charging mode on HDK8450

The HDK8450 build preserves the incoming ARM64 `x0` device-tree address through
BootShim and SEC. Before the firmware reuses bootloader RAM, it inspects
`/chosen/bootargs` and the bootconfig trailer of the initrd described by
`linux,initrd-start` and `linux,initrd-end`. Only the compact result is copied
into a GUID HOB. The original DTB and initrd are not retained by the app.

The exact `androidboot.mode=charger` argument selects charging mode. A valid
bootconfig `androidboot.mode` value takes precedence over the command line.
The parser checks the FDT structure, mapped ranges, trailer size and checksum,
and bounded bootconfig syntax. Absent mode information leaves ordinary boot
policy active. Invalid or inaccessible input is reported by `AndroidBootCr`.

After console connection, the HDK boot policy loads `ChargerCr` from the
firmware volume before ordinary OS boot selection. Charging mode remains in
boot services, keeping the ADSP owner available. If this application fails to
start or returns, charging-mode policy requests shutdown instead of proceeding
to an installed OS. Other board packages use null boot-mode and charger hooks.

The battery driver subscribes to the existing PMIC GLINK owner and uses the
portable `BatteryChargerCrLib` to read ADSP battery properties. It observes
charging; ADSP retains its charging and thermal policy. Unknown, absent or
expired readings are not represented as a measured percentage. See the
[application notes](../Application/ChargerCr/README.md) for display and button
behavior and the [battery core notes](../Library/BatteryChargerCrLib/README.md)
for wire format, units and request lifetime.

## Build and board checks

```sh
source .venv/bin/activate
./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0
```

The packaged application can also be copied from
`Build/WaipioCrPkg/DEBUG_CLANGPDB/AARCH64/ChargerCr.efi` to an accessible filesystem
and launched from the UEFI shell. This manual launch tests the UI and ADSP
observations; it does not test ABL mode detection.

For an automatic charging boot, retain the serial log from firmware entry.
Expect `AndroidBootCr` to report mode 2, then `ChargerBootCr` to launch the
application. Check percentage validity, charging animation, display idle after
30 seconds, short power wake, and a continuous power hold longer than three
seconds causing reboot. A normal ABL boot should still enter ordinary boot
policy. No board result is inferred from the host tests or successful build.

The primary physical DSI display has an owner-backed power operation. The
independent DP output and an external monitor's backlight have separate
ownership; see the application notes for the HDK LT9611 limitation.

## Source references

- [Android bootconfig handoff](https://source.android.com/docs/core/architecture/bootloader/implementing-bootconfig)
- The repository's actual `BaseFdtLib` is used for complete FDT validation.
- Android SM8450 PMIC GLINK battery protocol source pins are recorded in the
  battery core notes.
