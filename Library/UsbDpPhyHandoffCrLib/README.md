# Joint native combo-PHY qualification

This pure library combines independently read current-boot USBH and MDSH
observations for the known SM8450 HDK native PHY producer. It calls the actual
`CrUsbHandoffCanTakeover` and `CrMdssHandoffCanClaim` cores. All registered
MDSH producers must be frozen, including the PHY and SMMU. The DP route must
be absent, or stopped with all four streams revoked. A live DP mapping is
refused even if it is independently valid for display takeover.

The known native snapshot covers QMP base `0x088e8000`, bytes `0x4000` and
the actual global shared PHY owner. Its USB owner/readiness/touched journal
also describes the HS operations at the HDK's separate `0x088e3000` view;
that relationship is established by the native producer and shared core
source, not an HS address encoded in MDSH. Arbitrary other producers cannot
use this fixed profile. A clean absent PHY yields no demands. A clean DP-only
owner has all four clocks and the first three common rails; a USB owner has
all six rails. Missing/unknown/retained-error demands are refused.

Selected values are copied into fixed canonical slots from the native journal.
The policy follows the actual `UsbDpPhyCrLib/UsbDpPhy.c` Init/Common/Usb
functions and `UsbDpPhyCrDxe.c` HandoffFreeze/HandoffSnapshot producer:

| Slot | GCC clock | Rate (Hz) |
| --- | --- | --- |
| 0 | gcc_usb3_0_clkref_en | 0 (gate) |
| 1 | gcc_usb3_prim_phy_aux_clk | 19200000 |
| 2 | gcc_usb3_prim_phy_com_aux_clk | 19200000 |
| 3 | gcc_usb3_prim_phy_pipe_clk | 0 (gate) |

| Slot | Rail | mV | Mode |
| --- | --- | --- | --- |
| 0 | bobc1 | 3296 | 6 |
| 1 | ldob1 | 912 | 7 |
| 2 | ldob6 | 1200 | 7 |
| 3 | ldob5 | 880 | 7 |
| 4 | ldoc1 | 1800 | 7 |
| 5 | ldob2 | 3072 | 7 |

Both producer generation/transfer identities are returned independently.
They are separate counters and need not equal. The caller independently
validates each locator, reserved RAM and source identity before passing its
immutable snapshots. CRC and sequence checks detect damage, not provenance.

Success authorizes no IO. The Windows coordinator still must hold its exact
resource assignment, hardware D0 and reset-owner reservations, exclusive
ResourceHub FSA connection and actual operation rundown. Before any service
mutation, it must reserve, apply and read back the selected native demands
under a distinct permanent source owner. Failed partial application keeps
that owner and its storage. This evidence never authorizes `ReleaseOwner`
of the native baseline or active display ownership.

Run the actual EFI/NT sanitizer fixture:

```sh
python3 Platforms/CranePkg/Tests/run_usb_dp_phy_joint_handoff_tests.py \
  --output .tmp/waipio-continuation/usb-dp-phy-joint53-final
```

It uses the actual record builders/parsers and actual combo-PHY Init/Acquire
policy as its journal oracle. NT/EFI primitive and peripheral callbacks are
host models; the test does not establish a Windows or physical-board grant.
The fixed policy is source-derived from the GPL-2.0-only combo-PHY core and
this library preserves that license.
