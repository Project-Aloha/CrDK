# Crane MU Plus module forks

These seven explicit driver forks come from [Microsoft mu_plus](https://github.com/microsoft/mu_plus/tree/49625b8f6cabe7e4d9797d192c354b14af0490d1), commit `49625b8f6cabe7e4d9797d192c354b14af0490d1`. They preserve the existing Crane bring-up behavior while allowing official MU sources to remain unmodified. Original copyright notices, driver names, file GUIDs, entry points and public protocol identities are retained. `LICENSE` is an exact copy of upstream `LICENSE.txt`.

Only each INF's source/local-header/resource closure is copied. Public package headers, library implementations and package declarations remain dependencies from the official packages. No fork includes an upstream C implementation textually. This is not an independent copy of mu_plus or an attempt to make firmware-specific HID/graphics state portable by renaming it.

`manifest.json` lists all 42 closure files, upstream/pre-migration/fork SHA256 hashes and original-to-fork INF mappings. Files with `functional_delta: false` match the pinned upstream bytes before relocation. `patches/upstream-functional.patch` records the pre-existing changes from that commit; `patches/relocation.patch` records the two INF edits required by this move. Keep these records when updating from upstream.

| Original module path | Preserved changes |
| --- | --- |
| `HidPkg/HidKeyboardDxe/HidKeyboardDxe.inf` | Callback and queued-event lifetime, retained partial teardown, self-unregistering notifications, allocation/error handling. |
| `HidPkg/UsbKbHidDxe/UsbKbHidDxe.inf` | Checked boot initialization, report ID/packet width, retained cancel/teardown, reentrant callback lifetime, bounded recovery, best-effort SET_IDLE and diagnostics. |
| `MsGraphicsPkg/GopOverrideDxe/GopOverrideDxe.inf` | Exact physical primary selection with existing/delayed GOP publication, ignoring ConSplitter's virtual GOP. |
| `MsGraphicsPkg/OnScreenKeyboardDxe/OnScreenKeyboardDxe.inf` | Renderer-associated GOP, deferred SWM binding, device-path pointer and event cleanup corrections. |
| `MsGraphicsPkg/SimpleWindowManagerDxe/SimpleWindowManagerDxe.inf` | GOP/SRE from the same handle and notification of both protocols. |
| `MsCorePkg/AcpiRGRT/AcpiRgrt.inf` | Optional unconfigured/missing image skips RGRT installation successfully. |
| `MsWheaPkg/HwErrBert/HwErrBert.inf` | Correct GUID array growth size and quiet optional empty extra-variable configuration. |

For every path above, the selected fork INF is `CranePkg/Override/MU_PLUS/` followed by that path. Include only one implementation of each original file GUID in a firmware image. The official component must be replaced in both DSC and FDF selections; placing copies here alone does not activate them.

The selector is moved from `gMsGraphicsPkgTokenSpaceGuid.PcdMsGopOverrideControllerGuid` to `gCranePkgTokenSpaceGuid.PcdMsGopOverrideControllerGuid`, `VOID*`, zero GUID default, token `0x00000006`. The local GOP override INF adds `CranePkg/CranePkg.dec`; its C-level `PcdGetPtr(PcdMsGopOverrideControllerGuid)` call remains identical. The platform owns the selected GUID value and must keep it consistent with its preferred console. Shared DEC and platform selection changes are coordinated outside these module copies.

The upstream USB HID INF's ExtraFiles entry misspells the existing `UsbKbHidDxeExtra.uni` as `UsbHidKbDxeExtra.uni`. The local INF points to the actual unchanged file; this changes metadata only. Unused BMP source assets and upstream explanatory screenshots are not part of the required compiler closure and are not copied. Original module documentation remains available at the pinned upstream module paths.

The focused Crane HID lifetime, USB HID and graphics-console tests compile these fork implementations. They retain upstream protocol/public-header dependencies. Host tests demonstrate the covered memory ownership and selection behavior; they do not prove physical keyboard compatibility or every hardware display path.
