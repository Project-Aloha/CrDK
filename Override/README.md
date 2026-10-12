# Platform-owned MU modules

Functional platform changes live here instead of in the official MU submodules.
`Modules.json` lists the explicit DSC/FDF replacements. Original module names and
GUIDs are retained, so a firmware volume must select exactly one implementation.

These are source forks of individual UEFI modules, not portable Cr library cores.
They preserve the current storage, USB/HID, console, Shell and ESP variable
behavior while removing the dependency on edited upstream sources. Shared
Andromeda component selections and their platform FDF/APRIORI references move
together; this preserves the previously shared patched implementation. Validation
of HDK8450 does not establish that every other board is boot-tested.

Each module records its upstream revision, source hashes and patch provenance.
The upstream license and source copyright notices are retained. Unmodified source
files in a module closure are identifiable in that inventory. Common official
headers and libraries continue to come from the pinned MU packages. Newly added
protocols, library declarations and platform selector PCDs belong to CranePkg.

Development policy:

- Prefer existing library classes, protocols and platform configuration for new
  behavior. Use portable Cr libraries with OSKAL for hardware logic.
- Modify these explicit platform modules when a repair needs private UEFI driver
  state and no usable upstream extension exists. Record the difference so a
  future upstream fix can remove the corresponding fork.
- Keep official `MU_BASECORE` and `Common/MU` free of functional changes. The user
  permits temporary diagnostics: current DXE EBS, image-exit and reset messages
  are the only retained official-source patches.
- The experimental DXE reserved-memory guard has been withdrawn. Framebuffer
  retention must be solved at the platform layer; this migration does not add
  DP retention through ExitBootServices.

The detailed initial necessity audits are under
`.tmp/waipio-continuation/mu-audit71/`. Retaining a functional patch in a platform
module preserves tested behavior; it does not assert every hunk is intrinsically
necessary or that unresolved keyboard compatibility is fixed.

Validate the official-source exception list and platform identities with:

```sh
python3 Platforms/CranePkg/Tools/check_mu_overrides.py
python3 Platforms/CranePkg/Tools/check_mu_overrides.py --build-dir Build/WaipioCrPkg/DEBUG_CLANGPDB
```

HDK8450 Native71 completed the requested DEBUG build with Secure Boot disabled
in `.venv`. All 18 selected replacement application/driver PE payloads were
matched to their explicit platform module outputs in the generated FV. The
legacy serial shim is not selected on this target; ShellLib has no standalone
FV image. No hardware run was performed for this migration.

Native79 updates the platform SWM pointer FIFO. Consecutive pending motion is
coalesced while press/release coordinates are retained; a full queue drops only
the oldest sample. The original migration patches remain intact, and the
MU_PLUS manifest records the subsequent patch and both source hashes. Actual
queue routines are tested with `Tests/run_swm_pointer_queue_tests.py`.
