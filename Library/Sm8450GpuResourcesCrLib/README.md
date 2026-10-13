# SM8450 GPU resource resolver

This is the pure layout increment for the separate GpuCr driver. It validates exactly seven resource extents and resolves checked source word offsets. It allocates no storage, creates no mapping and performs no hardware IO. It is registered in CranePkg and the CRDK catalogue for managed SDK export. No active native or Windows caller is added by that registration.

`CrSm8450GpuResourcesBuild` accepts the seven typed descriptors in any order, requires the exact relationship/base/length, and returns a canonical 184-byte layout. The resolver rechecks every layout field on every call. Outputs remain unchanged after a failed call; input/output objects must be naturally aligned and disjoint, and the caller must keep input immutable during the call.

| View | Physical base/length | Intended composition |
| --- | --- | --- |
| GPU | 03d00000/40000 | Dedicated assignment |
| CX-debug | 03d61000/800 | Dedicated assignment; 800-byte extent despite page-rounded mapping |
| GMU-low | 03d6a000/26000 | Dedicated complement below GPUCC |
| GMU-high | 03d9a000/5000 | Dedicated complement above GPUCC |
| RSCC | 03d50000/10000 | Dedicated independent register namespace |
| GMU-PDC | 0b290000/10000 | Dedicated independent register namespace |
| GPUCC | 03d90000/a000 | Borrow existing permanent PEP mapping, with actual owner serialization |

GMU-low, GPUCC and GMU-high partition the mainline broad GMU span 03d6a000/35000. Mapping that broad span again would alias PEP GPUCC. CX-misc03d9e000/1000 is a source-relative slice inside GMU-high; it resolves to that view's offset4000, never an eighth mapping. Current Windows `AcpiCrDsdt/sm8450/crane/gpio.asl` assigns PIO PDC0b220000/30000, a different extent ending at0b250000; the pinned mainline PDC node uses only10000 bytes. Neither includes GMU-PDC.

GPU and GMU word-offset namespaces both begin at GPU03d00000 but deliberately admit different views. GMU offsets can resolve to the two GMU complements or borrowed GPUCC. RSCC, PDC, CX-debug, CX-misc and GPUCC have independent origins. The result is a 24-byte description `{View,ByteOffset,Bytes,Relation,PhysicalAddress}`, with no CPU pointer. A DWORD-aligned nonempty span must fit wholly within one admitted view and its namespace; contiguous physical views do not permit a cross-view operation. Shifts, additions, descriptor extents, pointer spans and aliases are checked before output mutation. Page padding is never admitted.

The dedicated/borrowed relationship describes the intended future composition, not acknowledged ownership. The actual Windows assignment owner must copy and validate raw/translated descriptors, retain one exact map per dedicated view, obtain counted GPUCC operations from its sole owner, and prove current powered access plus native/ABL/GMU retirement before performing any IO. Closing counted operations and draining ISR/DPC/worker users must precede unmap or prerequisite release. No power, clock, reset, IOMMU attach, firmware authentication or load authority follows from a successful resolver call.

## Source basis and licensing

Primary Linux revision `a53261b60834ce617cc4f3c7723dda8285c59789`:

- [`arch/arm64/boot/dts/qcom/sm8450.dtsi`](https://raw.githubusercontent.com/torvalds/linux/a53261b60834ce617cc4f3c7723dda8285c59789/arch/arm64/boot/dts/qcom/sm8450.dtsi): GPU/CX resources, broad GMU, RSCC, GMU-PDC, GPUCC and PIO PDC extents. This file has SPDX BSD-3-Clause and Copyright(c)2021 Linaro Limited.
- [`drivers/gpu/drm/msm/adreno/a6xx_gmu.h`](https://raw.githubusercontent.com/torvalds/linux/a53261b60834ce617cc4f3c7723dda8285c59789/drivers/gpu/drm/msm/adreno/a6xx_gmu.h): `GMU_BYTE_OFFSET` and the explicit GPU-origin comment; independent RSCC word-offset accessors.
- [`drivers/gpu/drm/msm/adreno/a6xx_gpu.h`](https://raw.githubusercontent.com/torvalds/linux/a53261b60834ce617cc4f3c7723dda8285c59789/drivers/gpu/drm/msm/adreno/a6xx_gpu.h): CX-misc word-offset accessors.
- [`drivers/gpu/drm/msm/adreno/a6xx_gmu.c`](https://raw.githubusercontent.com/torvalds/linux/a53261b60834ce617cc4f3c7723dda8285c59789/drivers/gpu/drm/msm/adreno/a6xx_gmu.c): PDC independent word-offset helper.

This interface and resolver are newly written MIT code. They use the physical-resource facts and namespace conventions, without copying upstream accessor implementations or register programming sequences. Upstream GPL-2.0 notices remain in the unchanged source snapshots produced by the source proof. The split is an explicit local ownership policy derived arithmetically from those extents, not a new hardware-resource assertion.

## Validation

`python3 Platforms/CranePkg/Tests/run_sm8450_gpu_resources_tests.py --output <new-directory>` runs actual native and NT builds under ASan/UBSan. The suite checks all5040 descriptor orderings, exact ends, both sides of GPUCC, cross-view rejection, missing/duplicate/mixed-family mappings, CX page padding, every altered normalized-layout byte, output aliases and address arithmetic overflow. The ARM64 object checker builds the same source for EFI and the real WDK. `check_sm8450_gpu_resource_sources.py --linux-root .vscode/linux --windows-root /home/sunf/CrWDDK --output <new-directory>` derives the partition from pinned DTS bytes, distinguishes the actual Windows PIO extent, and checks the actual C policy and source-relative units.
