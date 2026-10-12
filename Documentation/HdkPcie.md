# HDK8245 and HDK8350 native PCIe

PCIe110 selects MU ArmPciCpuIo2Dxe and PciHostBridgeDxe, with the native
Crane PCIe core and owned clock, GPIO, RPMh, ICC and SMMU adapters.
HDK8245 attempts RC0/RC1/RC2; HDK8350 attempts RC0/RC1, including ports
marked disabled in Android. One port's acquisition or link failure does not
prevent subsequent ports being attempted. An unavailable shared boot floor
prevents PCIe access until the RPMh provider acknowledges CX/MX/XO demand.

Register/range/SID/PHY and electrical pin facts come from the supplied board
DTBs and pinned public Linux/Android sources. Generate/check target facts with
`python3 Platforms/CranePkg/Tools/generate_hdk_pcie.py --check`.
Pinned references and review/build receipts are under
`.tmp/hdk8245-hdk8350-pcie110/`; no edk2-msm or leaked BSP source is used.

The controller and PHY supplies use the existing native RPMh provider.
Endpoint sequences include WLAN rails/enable, WiGig rails/RF reference,
HDK8350 WiGig GPIO47 plus50ms and RC GPIO6, and HDK8245 RC2 interface
GPIO62/PONRST89. WiGig GPIO14/67 pinctrl changes only mux/pull/drive and
preserves its existing direction/latch. Missing optional CmdDB rails are
qualified on use, preserving RPMh publication and other consumers.

All exposed roots use bus0..1 and exact root/endpoint SID pairs. This profile
supports direct endpoints; it does not establish arbitrary downstream switches,
multifunction DMA identities or IRQ/MSI. UEFI enumeration and storage use polling.
PCIe coherent WB SMMU policy is separate from the existing UFS policy.
Every live UFS domain retains its own exact SID and cache attributes.

The HDK8350 shared S12B supply retains enable only, without changing inherited
voltage/mode. A WLAN cold-power voltage guarantee is unproven. LDOE7 parent
power is inherited where public DT does not establish its physical input.
HDK8245 PONRST89 high+40ms follows supplied DT flags/minimum delay; the pinned
public PCIe consumer lacks that extension, so board behavior must confirm it.
GPIO6/47 drive/pull choices are native policy where no dedicated state is given.

Host regressions cover actual targets, every clock request/reset route,
all ICC paths, concurrent UFS/PCIe exact SMMU mappings above4GiB, per-port
failures, supply/GPIO partial acquisition and failed cleanup retry.
Build/package checks establish selected source bytes and image composition.
Fresh board logs are still required for PHY/link status, BDF enumeration,
BAR allocation and endpoint media reads. Display, USB and Windows enablement
are separate work.

Build serially from the activated repository environment:

```sh
source .venv/bin/activate
./build_uefi.py -d qcom-hdk8350 -t DEBUG -s 0
./build_uefi.py -d qcom-hdk8245 -t DEBUG -s 0
```

Canonical images are `Build/LahainaCrPkg/qcom-hdk8350.img` and
`Build/KonaCrPkg/qcom-hdk8245.img`. PCIe110 build helpers restore shared
BootShim bytes and retain prior109 images in the evidence directory.
