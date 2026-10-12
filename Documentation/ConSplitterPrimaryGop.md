# HDK8450 console framebuffer selection

The HDK has two independent physical GOPs: the primary DSI/bridge display and
the hot-plug DP display. The default ConSplitter graphics aggregation publishes
`PixelBltOnly`, with no physical framebuffer, after its second GOP is added.
Microsoft requires a BGRX linear framebuffer for Windows boot graphics; see
[UEFI protocol requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/uefi-requirements-that-apply-to-all-windows-platforms#protocol-requirements).

`gCranePkgTokenSpaceGuid.PcdConSplitterPrimaryGopGuid` is an optional
fixed `VOID*` PCD containing a 16-byte GUID. Its all-zero default preserves the
existing aggregate behavior on other boards. The HDK selects
`9042A9DE-23DC-4A38-96FB-7ADED080516A`, its exact single vendor-node primary
display path. Selection uses the actual bound controller's published GOP,
including the rendering-engine wrapper when present.

With that policy, the stable virtual console GOP forwards QueryMode, SetMode,
and Blt to the selected primary GOP. It copies the actual current mode and
framebuffer metadata into its own mode storage. Direct framebuffer writes and
GOP Blts therefore reach the same primary DSI framebuffer. DP retains its own
physical GOP and framebuffer, and SimpleTextOut still writes to both consoles.
There is no background framebuffer mirroring. Applications wanting independent
DP graphics can use the DP controller's GOP directly.

Secondary arrival never replaces the primary framebuffer. Removing the
selected controller closes forwarding and clears virtual framebuffer metadata
before releasing the parent console relationship. A failed stop restores that
exact selection. Reentrant arrival during stop cannot replace the reserved
controller identity. If the primary is absent, a secondary remains a text
console without being substituted as the selected graphics framebuffer.

The capture44 Windows boot attempt clears both outputs, continues healthy DP
scanout of black pixels, then calls ResetSystem. It contains no confirmed
ExitBootServices entry. The multi-GOP framebuffer incompatibility is reproduced
by actual-source tests; the capture does not prove it caused the reset.

Validation:

```sh
python3 Platforms/CranePkg/Tests/run_con_splitter_gop_tests.py --require-linear --output .tmp/waipio-continuation/gop-linear45-final
python3 Platforms/CranePkg/Tests/run_con_splitter_gop_tests.py --output .tmp/waipio-continuation/gop-aggregate45-default
python3 Platforms/CranePkg/Tests/run_dp_tests.py
```

The sanitizer fixture executes the actual mode-selection, QueryMode, SetMode,
Blt, text output, delete, and stop functions with the real private header. It
covers late primary arrival, exact path matching, coherent linear/Blt writes,
independent secondary graphics, provider errors, copied mode ownership, count
retention, primary/secondary removal, and reentrant stop success/failure.

The implementation is now selected explicitly from CranePkg/Override/MU_BASECORE/MdeModulePkg/Universal/Console/ConSplitterDxe; official MU sources are unchanged.
