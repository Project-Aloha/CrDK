# HDK8450 MP Services

`Driver/MpServicesCrDxe/MpServicesCrDxe.inf` is selected by the HDK8450 DSC, FV and APRIORI manifests after native RAM discovery. The stock PSCI MP driver must not also be selected. No new platform PCD or GIC handler is required for this first stage. Module linkage and packaged presence are checked as part of the firmware build; physical SM8450 execution still needs a device test.

The provider checks the actual `gArmMpCoreInfoGuid` HOB for eight exact affinities `0x000, 0x100, ... 0x700`, and finds the BSP by MPIDR. Processor numbers are array positions, not `AFF0` or `ArmPlatformGetCorePosition`. It requires the actual SMC PSCI 1.0 convention and probes CPU_ON64, CPU_OFF and AFFINITY_INFO64 support. Initial AP availability is established by a bounded CPU_ON / READY / CLOSE / confirmed-OFF round trip. A processor already reported ON by firmware is left untouched and unavailable.

## Supported call contract

`GetNumberOfProcessors`, `GetProcessorInfo` (including extended topology), `WhoAmI`, `EnableDisableAP`, and blocking `StartupThisAP` / `StartupAllAPs` with `WaitEvent == NULL` and `TimeoutInMicroseconds == 0` are implemented. `WhoAmI` can run on an AP without Boot Services. Other calls require the BSP, at TPL_CALLBACK or lower, and dispatch is serialized. Parallel dispatch starts all requested APs before joining; single-thread dispatch joins in processor-number order.

`SwitchBSP`, finite procedure timeouts, and nonblocking calls return `EFI_UNSUPPORTED` before CPU_ON, argument publication or output changes. This is a deliberately reduced MP surface, not a complete implementation of every PI MP Services guarantee. PI timeout semantics require terminating the procedure before reporting timeout; a remotely sent SGI cannot guarantee this for an arbitrary privileged callback that masks interrupts or replaces VBAR. No such termination is claimed here.

AP procedures must be bounded, AArch64 ABI conforming, and operate only on caller-owned data or explicitly AP-safe interfaces. They must not allocate, log, call Boot Services, or initiate ExitBootServices. The caller must keep procedure code and its argument alive until the blocking call returns. Normal return preserves architectural control state required by the provider. All private AP entry paths mask interrupts. A procedure that deliberately changes translation, exception vectors, or the provider's reserved storage lies outside this stage's contract.

## Entry and retirement lifetime

Before any CPU_ON, initialization allocates one reserved executable entry page, seven reserved 128-byte mailboxes in separate pages, and seven reserved 16 KiB stacks. The 4 KiB copied entry has no relocation and includes its own exception vectors and an immutable affinity-to-frame table. It checks EL1 or non-VHE EL2 and incoming MMU/data-cache-off state before dereferencing the incoming context. The exact trusted frame is recovered from the copied table, including in an exception; software-defined TPIDR registers are not trusted.

READY is published before any procedure address can be consumed. RUN checks the AP's own PARange and 4 KiB/LPA2 support against the captured TCR, including an enabled TTBR1, before using the current BSP translation registers. EL2 checks the AP's own incoming E2H as well as the BSP's format. It qualifies FP access and initializes FPCR and FPSR to zero. Each dispatch captures current BSP registers rather than retaining stale page-table addresses across a later memory-map change.

Mailboxes remain normal WB memory, matching the surrounding reserved RAM cache class. BSP maintenance cleans all mailbox bytes before CPU_ON/RUN, refreshes the shared command/stage line before changing a command, and performs writeback/invalidate of the complete 128-byte frame before reading a stage or raw fault. The BSP holds no dirty mailbox data while an AP may publish a stage. This avoids a 4 KiB NC island inside a reserved 64 KiB range and avoids stale second-line fault/status reads during cache-off transitions. Caller data uses the platform's coherent WB execution environment once AP caches are enabled.

After a normal procedure return, execution is back in reserved code before DONE is published. The private off path performs clean-only set/way maintenance using CCIDX 0 or 1 geometry, disables MMU and data cache, invalidates local TLBs, and calls CPU_OFF with unused SMCCC argument registers zeroed. A returned CPU_OFF result is retained in the frame; the CPU then parks in reserved code with MMU/data cache disabled. Its park does not depend on BSP page tables that an OS might reclaim.

Only actual AFFINITY_INFO at level zero reporting OFF retires an accepted CPU_ON. READY, DONE, a successful send, or a mailbox flag is insufficient. Startup/off transitions are bounded to 500 ms; procedure execution for timeout zero is unbounded as specified. Failed or uncertain APs are disabled, their exact entry/frame/stack allocation stays reserved, and neither a caller argument nor task is restarted on that AP. Initialization failure closes the valid owned event before returning and releases only objects whose CPU is confirmed OFF. Retained initialization state cannot be overwritten by reentry.

The provider pins its loaded image before protocol publication. At ExitBootServices it closes admission, sends CLOSE to reserved frames, and waits a bounded interval for accepted APs to report OFF. Legal blocking use leaves no active callback at this boundary. A denied/off-pending processor remains reserved and is reported as a failure; the event does not pretend to force-cancel an arbitrary procedure. MP methods are unavailable after closure.

## Validation

Run:

```sh
python3 Platforms/CranePkg/Tests/run_mp_services_tests.py --output .tmp/mp-services-review
```

The runner links actual provider C with controlled PSCI/UEFI boundaries under ASan/UBSan. It checks accepted CPU_ON ownership, late entry, denied off, foreign-on processors, all allocation/attribute failures, unsupported preflight, output retention, event/image lifetime, WB mailbox visibility, and cache-off fault publication. It also compiles the actual C and assembly for ARM64 COFF, verifies the copied entry is exactly 4096 bytes with zero relocations, records disassembly/source hashes, and rejects unintended freestanding memory intrinsics or the unsupported all-cache C helper.

Current 44-mode receipt: `.tmp/waipio-continuation/mp-cr40/final-format-guard2/artifacts.json`. Separate mutations that remove the CPU_ON writeback or refresh only the first fault-frame line fail the independent weak-cache fixture, recorded in `.tmp/waipio-continuation/mp-cr40/cache-mutations/artifacts.json`. These host tests do not execute privileged assembly. The persistent `Tests/run_mp_entry_system_tests.py` QEMU fixture executes the actual copied entry with modeled PSCI denial. Its 15 current EL1/non-VHE-EL2 normal/fault NC/WB and rejected-E2H/PA-width/granule/DS cases pass at `.tmp/waipio-continuation/mp-entry41-final/artifacts.json`; Cortex-A57 CCIDX0 is executed, while CCIDX1 only has source review. HDK CPU_ON/OFF, AP coherency and OS takeover still require device testing after activation.

The parent-owned `Application/MpCrTest/MpCrTest.inf` is the intended initial shell diagnostic. It uses caller-owned slots, performs no AP logging or Boot Services, records WhoAmI/MPIDR/checksums, and verifies results on the BSP. The HDK DSC builds it separately; it is not automatically run. Copy `Build/WaipioCrPkg/MpCrTest.efi` to an accessible filesystem and run `MpCrTest.efi` in the UEFI shell. A complete result reports seven secondary processors verified; compare the initialization `MpCr:` line and retain any unavailable/OFF errors for diagnosis before relying on MP in other firmware clients.

## Primary source checks

- Local `MU_BASECORE/MdePkg/Include/Protocol/MpService.h` defines blocking timeout-zero, AP-safe call restrictions and termination requirements for finite timeout.
- [UEFI 2.11 AArch64 binding](https://uefi.org/specs/UEFI/2.11/02_Overview.html#aarch64-platforms) specifies the FP environment and the memory cache-class rules for reserved/runtime/NVS 64 KiB ranges.
- Local Linux reference revision `a53261b60834ce617cc4f3c7723dda8285c59789`, [SM8450 DTS](https://github.com/sunflower2333/linux/blob/a53261b60834ce617cc4f3c7723dda8285c59789/arch/arm64/boot/dts/qcom/sm8450.dtsi), and Android device-tree reference revision `877203a7cf9b79973d44a9db716490f95d820110`, [Waipio DTS](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/877203a7cf9b79973d44a9db716490f95d820110/kernel_platform/qcom/proprietary/devicetree/qcom/waipio.dtsi), agree on affinities and PSCI SMC.
- The checked local ARM PSCI definitions and existing `ArmSmcLib` supply the real calling convention. [Linux secondary entry](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/head.S) and [ARM64 cache maintenance](https://github.com/torvalds/linux/blob/master/arch/arm64/mm/cache.S) are source references for architectural barriers and cache geometry; the reserved lifetime policy here is specific to this firmware provider.

Still unfinished: proven SGI reservation/per-AP GIC initialization, arbitrary finite-timeout termination, nonblocking event lifetime and cancellation, and BSP switching. Native OSKAL's global HardwareInterrupt2 wrapper and the cached BSP redistributor do not supply an AP-local cancellation implementation.
