# ScsiDisk platform fork

This module preserves the tested native storage/USB behavior without further functional edits to official MU sources. It is selected explicitly by the platform DSC/FDF and must not be packaged alongside the original module (same FILE_GUID and BASE_NAME).

Baseline: MU_BASECORE `562c0927ef7bfc65d26d9caf322e569ccb23b6a9`, `MdeModulePkg/Bus/Scsi/ScsiDiskDxe`. `PROVENANCE.json` records each baseline, captured source, and fork hash. `MU_BASECORE.patch` records the functional delta at relocation; it applies to the baseline repository, not the fork directory. All original source licenses remain intact. The copied closure contains the INF-listed sources, local headers, and UNI metadata, with no copied package headers or libraries.

After relocation, ScsiDisk uses a private 64-byte SBC Block Limits VPD declaration. This retains correct B0 allocation lengths with the unmodified MU headers. Maximum WRITE SAME length is eight bytes at offset 36; the pinned MU declaration omits its upper four bytes. The actual caller/UefiScsiLib/UFS regression covers valid responses, field offsets and overflow rejection. `MU_BASECORE.patch` and the fork hashes include this change.

Necessity audit and regression mapping: `.tmp/waipio-continuation/mu-audit71/storage-usb.md`. This relocation is not a new hardware qualification.
