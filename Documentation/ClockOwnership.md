# Waipio clock ownership

The packaged Qualcomm `ClockDxe` remains the only owner of GCC hardware.
`ClockCrDxe` adapts Crane resource requests to its native `EFI_CLOCK_PROTOCOL`;
it does not map GCC or initialize the portable `ClockLib`. Native IDs are
resolved by CrDAL metadata, and aliases share one reference-counted native
vote.

## Binary evidence

The active HDK and MTP `PatchedBinaries/ClockDxe.efi` files are identical:

```
4ac5cf05dec3f990f3afc71889ee9b134fd2502af30abadd55f280a68644c5a2
```

The HDK original binary has SHA256
`c394b76595273dd8ab0bce5f8e1b84ec973a87963f546cb2a5e900b9e95bc1d9`
and the same relevant layout. In both layouts, the PCIe PIPE records are:

| Resource | Name field RVA | Domain pointer field | Domain RVA | Mux register |
| --- | ---: | ---: | ---: | ---: |
| `gcc_pcie_0_pipe_clk` | `0x22fc0` | `0x22fc8` | `0x29938` | `0x17b060` |
| `gcc_pcie_1_pipe_clk` | `0x23160` | `0x23168` | `0x29ad8` | `0x19d064` |

Both domains have a null BSP pointer and zero BSP length. Their DomainMux
`ConfigMux` routine at RVA `0xf374` reads only byte zero of the supplied mux
configuration and writes that selector directly to the mux register. It does
not read the rate, divider, M, N, or twice-D fields. Thus the native call

```
SelectExternalSource (Clock, Id, 0, 0, 0, 0, 0, 0)
```

selects source zero; divider zero is ignored by this simple mux. Linux
`phy_mux_enable()` selects that external PHY input before enabling the PIPE
clock. A plain native `EnableClock` does not perform this mux write.

`gcc_pcie_1_phy_aux_clk` is also a no-BSP mux:

| Binary | Name string RVA | Name field RVA | Domain pointer field | Domain RVA | BSP pointer/length |
| --- | ---: | ---: | ---: | ---: | --- |
| HDK original | `0x15cc2` | `0x23090` | `0x23098` | `0x29a08` | `NULL / 0` at `0x29a08/0x29a10` |
| active patched HDK/MTP | `0x15cc2` | `0x23090` | `0x23098` | `0x29a08` | `NULL / 0` at `0x29a08/0x29a10` |
| MTP original (`e9c75b7c1b3ee97afcbfe5ecc6d251f7ec06a260509a32ec178ec75234558c0e`) | `0x154ea` | - | - | `0x289c8` | `NULL / 0` at `0x289c8/0x289d0` |

In the active/HDK layout, its HAL CGR address is `0x19d080` and the pmControl
table at `0x2f0e8` points to `ConfigMux` RVA `0xf374` and `DetectMux` RVA
`0xf390`. The original MTP table at `0x2e0a8` points to the equivalent routines
at `0xeea0` and `0xeebc`. Each `ConfigMux` is the same two-operation body:
`ldrb w9, [x1]` reads only HAL configuration byte zero (`nMuxSel`), then
`str w9, [x8]` writes it to the CGR. It never reads byte one (`nDiv2x`). The
corresponding detect routine reads the selector from the register and returns a
fixed `nDiv2x` of two. This also proves that divider zero in the external-source
request is ignored by this mux. Qualcomm's BOOT.MXF.2.5.1 HAL source at commit
`c60ac2f1da8da430f246da52f375e1d986e77349` has the same
`HAL_clk_GenericConfigDomainMux` implementation.

Qualcomm `Clock_SetClockFrequency` returns `DAL_ERROR_NOT_SUPPORTED` when the
BSP pointer is null. Linux describes its mux at `0x9d080`: selector zero is the
external QMP PHY AUX clock, selector two is XO, and QMP registers the external
clock at a fixed 20 MHz. Therefore both Crane IDs
`gcc_pcie_1_phy_aux_clk_src` and `gcc_pcie_1_phy_aux_clk` resolve to the native
leaf with `NO_RATE | EXTERNAL_SOURCE`. The adapter skips the unsupported exact
20 MHz request and selects source zero before the first native enable vote.

The host adapter test starts each simple mux at selector two, proves the first
alias vote changes it to selector zero before `EnableClock`, and proves a
second alias vote neither reprograms the mux nor adds another native enable.
