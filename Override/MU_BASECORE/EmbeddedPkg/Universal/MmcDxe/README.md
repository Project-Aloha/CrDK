# MmcDxe platform fork

This module preserves the tested native storage/USB behavior without further functional edits to official MU sources. It is selected explicitly by the platform DSC/FDF and must not be packaged alongside the original module (same FILE_GUID and BASE_NAME).

Baseline: MU_BASECORE `562c0927ef7bfc65d26d9caf322e569ccb23b6a9`, `EmbeddedPkg/Universal/MmcDxe`. `PROVENANCE.json` records each baseline, captured source, and fork hash. `MU_BASECORE.patch` records the functional delta at relocation; it applies to the baseline repository, not the fork directory. All original source licenses remain intact. The copied closure contains the INF-listed sources, local headers, and UNI metadata, with no copied package headers or libraries.

The C/header/UNI bytes are unchanged. The INF is byte-identical to its captured original.

Necessity audit and regression mapping: `.tmp/waipio-continuation/mu-audit71/storage-usb.md`. This relocation is not a new hardware qualification.
