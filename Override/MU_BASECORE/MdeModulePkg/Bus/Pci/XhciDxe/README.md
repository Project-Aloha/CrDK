# XhciDxe platform fork

This module preserves the tested native storage/USB behavior without further functional edits to official MU sources. It retains bounded DEBUG-only controller, endpoint, ring and reset observations for bring-up diagnostics. It is selected explicitly by the platform DSC/FDF and must not be packaged alongside the original module (same FILE_GUID and BASE_NAME).

Baseline: MU_BASECORE `562c0927ef7bfc65d26d9caf322e569ccb23b6a9`, `MdeModulePkg/Bus/Pci/XhciDxe`. `PROVENANCE.json` records each baseline, captured source, and fork hash. `MU_BASECORE.patch` reconstructs the captured fork and diagnostics; apply `RenesasFirmware.patch` afterward for the current firmware-admission change. Both patches apply at the baseline repository root, not the fork directory. All original source licenses remain intact. The copied closure contains the INF-listed sources, local headers, and UNI metadata, with no copied package headers or libraries.

After relocation, read-only bring-up diagnostics and a Renesas firmware preflight were added. The preflight reads PCI status on 1912:0014/0015 before enabling or resetting the controller; a device without running firmware or a completed ROM load remains unavailable. It does not download firmware, write ROM or claim to restore USB hardware. The captured lifetime corrections remain intact. The INF additionally imports CranePkg.dec for Usb2HcFreeze; the protocol declaration resides in CranePkg/Include/Protocol/Usb2HcFreeze.h.

Necessity audit and regression mapping: `.tmp/waipio-continuation/mu-audit71/storage-usb.md`. This relocation is not a new hardware qualification.
