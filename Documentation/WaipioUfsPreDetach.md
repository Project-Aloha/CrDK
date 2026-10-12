# Waipio UFS106: observe the inherited UFS SMMU before detach

This is a capture-only experiment for HDK8450. It prints the inherited UFS
SID `0xE0` configuration and halts before the first SMMU write. It does not
enter DXE or Windows. The continuously coherent, continuously translated
Windows handoff remains a separate unfinished repair.

## Boot the experiment

From the workspace root:

```sh
fastboot boot Build/WaipioCrPkg/qcom-hdk8450-ufs106-predetach.img
```

The image is 2048000 bytes, SHA256:

```text
ea23405d92597daadfd8b1ec1ff64aeb2531a45fd8166d609235ac922c542265
```

Capture serial output from `SEC: serial ready`, through the lines beginning
`UFS106 PREDETACH:`, to `END ... HALT before detach`. Stopping there is
intentional. `SEC: platform ready` should not appear after this capture.
No device boot, flash or reset was performed during host qualification.

## What the capture establishes

The hook is in `QcomMmuDetachHelperLib.MmuDetach`, called by the real SM8450
`PlatformInitialize`. `EarlyInitialization` initializes serial first. Boot
data capture, HOB construction, `MemoryPeim` and DXE occur later. Thus the
capture precedes native SMMU retirement/ownership changes. CPU startup and
the BootShim have already executed; this is not a trace inside ABL itself.

- Global ID/geometry, sCR0 and fault registers are recorded.
- Every valid SMR matching E0, including masks, is paired with its raw S2CR.
- Translating routes identify CBAR/CBA2R, SCTLR/ACTLR, full TTBR0/1, TCR/TCR2,
  CONTEXTIDR, MAIR0/1, FSR/FAR/FSYNR and CBFRSYNRA.
- Shared bank references, dedicated stage-2 banks and the final bank are
  identified. Multiple E0 matches are explicitly ambiguous.
- Repeated reads flag observed changes; this is not an atomic snapshot.

| Evidence | Interpretation |
| --- | --- |
| S2CR type 1 | Stream bypass; CBNDX does not select a translation bank. |
| S2CR type 2 | Stream fault route. |
| Type 0, CBAR type 1, SCTLR.M=0 | Stage-1 bypass with stage-2 bypass. |
| Type 0, CBAR type 1, SCTLR.M=1 | Stage-1 translation configured; inspect global enable and TCR as well. |
| CBAR type 0/2/3 | Stage-2, fault or nested configuration; raw fields only. |
| sCR0.CLIENTPD=1 | Bank M alone does not establish active translation. |
| No E0 match | Inspect global unmatched-stream policy; do not invent a bank. |

SCTLR WACFG/RACFG/SHCFG/MTCFG/MEMATTR overrides apply only when M=0, per the
public Android driver. With translation enabled, MAIR is only the attribute
palette: leaf PTE AttrIndx/SH and effective routing are needed to establish
the DMA memory attributes. `_CCA=1` alone does not prove either configuration.

TTBR memory is deliberately not dereferenced: this early path has no verified
APPS RAM access/grant inventory. A nonzero TTBR is not proof of a readable page
table. If the board capture shows translation enabled, qualify the captured
table addresses and format before adding bounded PTE inspection. This image
does not establish IOVA-to-PA mappings or identity coverage.

## Source, build and validation

`PcdSmmuUfsPreDetachCapture` defaults to FALSE in MUCommonCrPkg. The experiment
build temporarily sets it TRUE only for HDK8450 and restores the board PCD
file afterward. Even geometry/lookup failures halt in capture mode. The
observer issues no ATS commands, cache/TLB maintenance or fault-clear writes.
Direct SerialPortLib output remains available with DEBUG output masked.

The host test compiles the actual helper and SM8450 caller:

```sh
python3 Platforms/CranePkg/Tests/run_qcom_mmu_detach_tests.py
```

ASan/UBSan checks cover the previous detach contracts and the new capture
path, including high TTBR halves, fault preservation, shared/wildcard/ambiguous
routes, bypass and stage-2 decoding, bad geometry, masked DEBUG, changing
routes, caller halt and zero SMMU writes.

The canonical `.venv` build used
`./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0`, Clang 18.1.3/CLANGPDB.
The Android kernel payload matches BootShim plus FD. Official GenFw
rebase/strip/TE conversion reproduces every byte of the SEC module extracted
from the outer FVMAIN_COMPACT; the capture PCD and diagnostic markers were
verified in the actual outputs. Existing platform/FV policies pass.

Evidence, source baseline, scoped diff, build log, 5062 archived inputs and
preservation receipts are in `.tmp/waipio-ufs106/`. The normal image, previous
UFS104 working copy, Windows CR image, BootShim and held PHY drafts are
preserved. The build directory's intermediate modules reflect the experiment;
the restored normal `.img` remains UFS105. Use the explicit diagnostic filename.

Register definitions and bypass interpretation reference only public Android
`waipio-msm-5.10` commit `e93f69dc83e69e5139e6a7911041f357b5e2b304`,
`drivers/iommu/arm/arm-smmu/arm-smmu.{c,h}`. No edk2-msm or leaked BSP source
was used. Hardware capture and Windows repair validation remain pending.
