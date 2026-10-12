# UFS power data from the Android boot FDT

HDK8450 captures the ABL-provided tree before the HOB allocator and memory
initialization can reuse it. `UfsPowerFdtCrLib` validates the bounded complete
tree and extracts a pointer-free supply record. SEC copies that record and the
parse status into a dedicated GUID HOB. DXE never follows the saved FDT address.

`CrDALDxe` qualifies the record against the selected target, then publishes a
private immutable UFS context. Other DAL devices remain available if UFS power
data is missing, malformed, ambiguous or unsupported. A failed capture is not
permission to use a guessed voltage. HDK8450 requires the HOB. The developing
SM8250 port retains its explicitly logged legacy profile when early capture is
not selected; its native UFS controller is not yet enabled in the board image.

The parser follows host and PHY supply phandles. Android RPMh resources are
identified through the provider parent's `qcom,resource-name`; mainline RPMh
resources use the qualified PMIC controller, `qcom,pmic-id` and regulator node.
Names such as `regulator-name` and DTS labels are not resource identifiers.
Six distinct roles are retained: VCC, VCCQ, VCCQ2, VDD-HBA, VDDA-PHY and VDDA-PLL.
An Android VDD-HBA GDSC is handled by the existing domain sequence, not as a PMIC
rail. A qualified fixed always-on supply remains unvoted. Missing host supplies
remain inherited; missing PHY supplies are unsupported.

Voltage selection operates in microvolts:

1. Intersect provider constraints, explicit consumer constraints and the
   board-qualified role/resource interval. Aliased consumers must agree.
2. Prefer an explicit initialization voltage inside the complete intersection.
   Otherwise use an exact fixed interval, or the documented target preference
   only if it remains in the intersection.
3. Require exact PMIC step alignment and exact conversion to RPMh millivolts.
   Do not round or clamp. Malformed or physically unsupported initialization
   values are rejected rather than hidden by a fallback.

An initialization request is not a measurement of the voltage left by ABL.
For example, Android Kona describes L17A initialization at 2.496 V but the UFS
consumer requests 2.504–2.950 V. The qualified target preference is 2.504 V.
Mainline range-only descriptions can likewise use a qualified preference.
No voltage is inferred from the UFS protocol version.

The current rule sets preserve PHY-before-device acquisition and deduplicate
shared physical rails. VCC can vary within the qualified PMIC interval and the
tree's narrower requirements, capped at 3.008 V under the existing 3.296 V
parent vote. VCCQ remains 1.2 V on these targets because the
existing parent supply topology is qualified for that voltage. This is not a
general importer of arbitrary regulator dependencies. Unsupported always-on
RPMh obligations are rejected: ordinary UFS cleanup cannot promise a permanent
baseline lease. Android SM8250's extra `smpa4` vote is also not yet qualified by
the native target; mainline's explicit fixed supply is a separate case.

All functional changes are in Crane. Official MU sources retain only the
separately inventoried diagnostic patches.

Sources used for the board policy are the local pinned Android SM8450
`waipio-cdp.dtsi` and `waipio-regulators.dtsi`, Android SM8250 `kona-qrd.dtsi`
and `kona-regulators.dtsi`, mainline `sm8450-hdk.dts` / `sm8250-hdk.dts`, and
`drivers/regulator/qcom-rpmh-regulator.c` for PMIC5 NLDO/PLDO step tables.
Parser and integration test receipts record the actual source and fixture
hashes. Build validation does not establish physical voltage or board operation.

## UEFI DMA policy and Windows handoff

SM8450 UFS SID `0xe0` uses coherent stage-1 translation from its first UEFI
attachment (`MAIR0=0xff`, inner shareable). The revision-4 UFS target sets
`DmaCoherent=TRUE`, causing ordinary UFS allocations, including requests with
`Attributes=0`, to request `EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED`. The SMMU
adapter applies CPU `EFI_MEMORY_WB` while preserving protection attributes.
This covers the transfer/task lists and permanent command/response/PRDT pool.

A missing WB capability or failed cached allocation fails the request; there
is no NC fallback. Every cached common-buffer mapping retains the allocation's
policy, including subranges. Both explicit-SID and PCI grants reject noncoherent
domains before publishing access. A PCI function's full stream set is checked
before any grant. Revocation remains available. Failed frees retain the actual
WB/NC policy for retry; failed attribute recovery quarantines the allocation.
Default allocations for other clients continue to use Normal-NC (`WC` on Arm).
Streaming data retains its existing clean/bounce/invalidate ownership sequence.

The public Waipio Android kernel `e93f69dc83e6` allocates UFS descriptors
through `dmam_alloc_coherent`. Its coherent `dma_pgprot` branch preserves
`PAGE_KERNEL`, and `dma_info_to_prot` selects `IOMMU_CACHE`; `io-pgtable-arm`
encodes WB and inner shareable. UFS105 pairs these CPU and IOMMU attributes.
The earlier MAIR-only change preceded the first-NOP regression; UFS104 restored
NC and the user confirmed Windows desktop boot. That working image is retained.
Host tests and source evidence do not prove the physical cause of that failure
or establish hardware success for the new always-coherent UFS105 configuration.

The legacy Windows profile stops UFS, revokes the firmware mappings and
releases only its owned E0 bank to coherent physical DMA at EBS
(`SCTLR=0x0a5f0000`). Both UEFI translation and the Windows physical DMA phase
are coherent; the transition changes address ownership, not the cache policy.
`_CCA=1` remains enabled. The CR profile's OS IOMMU contract remains separate.
