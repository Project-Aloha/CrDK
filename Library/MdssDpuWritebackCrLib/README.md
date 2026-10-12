# SM8450 single-mixer WB2 core

This portable library programs one linear BGRX copy through DMA1/XIN5,
LM2, PP2, CTL2 and WB2/XIN6. The copy width is limited to **2560** by the
single mixer. WB2's independent 4096-pixel limit does not enlarge this recipe.
Identity and source LR+UD reflection (180 degrees) are supported; no linear
90/270, scaling, UBWC, rotation block, composition or performance guarantee
is advertised. Existing native and Windows providers remain unchanged.

The maximum *touched footprint* is 16 MiB. A larger valid reserved primary
mapping is accepted as `OutputBytes`; its full extent and the touched range
must fit 32-bit IOVA space. Input and output mappings must be page aligned,
non-overlapping in IOVA, and large enough for `(height-1)*stride+width*4`.
The actual mapper independently excludes physical aliases and supplies READ
input / WRITE output permissions. A number in a job is never a DMA grant.

## Call and lifetime contract

`Init` accepts zeroed permanent storage and exact DPU 0xAE01000/0x8F000 and
VBIF 0xAEB0000/0x3000 views. It performs no IO. The owner must hold the real
assignment, power/domain/clocks/ICB, native and foreign-writer retirement,
buffer/domain references and complete operation rundown for every later call.
No generic readiness flag substitutes for these prerequisites. Every IO
callback checks actual current physical admission before accessing a register.

The calls form separate stages:

1. `Prepare` at PASSIVE validates geometry before IO, binds the exact stable
   job address/snapshot, rejects conflicting routes across all six CTLs,
   including LM2/LM3 and MERGE_3D_1, fresh-halts XIN5/6, programs checked
   ordinary configuration and observes its owned pending flush. It disables
   inherited merge mode/mux and crop enable. It then observes unhalt and
   rechecks the prepared image. Repeated exact Prepare requalifies the image;
   a copied or changed job cannot replace it.
2. The future typed adapter holds the **actual IRQ synchronization lock**
   across real `CrMdssIrqArm` and `Kick`. Kick calls only bounded, nonpaged,
   DIRQL-safe Read32/Write32/Fence callbacks. These callbacks cannot acquire
   provider/PASSIVE locks, call RootReady/SPB/clock services, wait, allocate
   or delay. Kick rechecks the prepared image and binds the exact IRQ token,
   leaf owner and arm generation before the only CTL2 START write. No generic
   user-supplied kick callback exists.
3. `Complete` consumes a copied event from the real synchronized IRQ ledger
   Take. It checks token address and immutable fields, leaf owner, exact job,
   registration, generation, sequence and WB2 bit4. It performs no MMIO.
   A dropped START produces no event; a matching event after an error or
   nonfinal Kick records observation but **cannot return successful copy**.
   Polling idle, reset/flush progress and a timeout never manufacture success.
4. `Stop` at PASSIVE closes new job admission, detaches routes, observes CTL2
   reset busy then clear and no pending start/flush, fresh-halts both XINs and
   checks source-qualified idle, then restores ordinary configuration. Its
   journal includes possibly committed writes and retries exact masked
   restores. Shared force, QoS, memory-type, OT and gather fields preserve
   neighbours. W1C/status/START/FLUSH/reset are never ordinary restore entries.
   Fast reset completion before busy observation safely declines; it is not
   certified by an already-zero register.
5. `CheckStopped` supplies a read-only *current* off fence for the actual
   mapper's future typed callback: exact job and arm generation, stopped
   state, no cleanup uncertainty, quiet routes/CTL and actual halt/idle
   reads. Zero generation is valid only for a job never kicked. Stop leaves
   the owned halt requests asserted, and restores force controls only after
   acknowledgment. A fresh halt observation and the currently asserted
   request/ACK are distinct checks.
6. After producer stop, outer exact IRQ job retirement/discard, live leaf
   teardown and ISR/DPC/waiter/output/CPU drain, `Retire` rereads off evidence
   and drops only job metadata. It unmaps/frees/releases nothing. `Uninit`
   requires an idle retired core. Quarantine is irreversible metadata-only
   closure after caller rundown; it makes no fresh IO and keeps uncertain
   job/configuration/storage reachable.

Prepare/Stop failures require the same job and retained journal until stop
and retirement succeed. `Complete` returning success is still distinct from
physical stop: output pages, mapping permissions and resource votes remain
owned through their actual producer/DPC/CPU fences.

## Target source choices

Mainline revision `a53261b60834ce617cc4f3c7723dda8285c59789` supplies the
SM8450 DPU8.1 catalog and `dpu_hw_{sspp,lm,ctl,wb,vbif,merge3d}` /
`dpu_encoder_phys_wb` register recipe. Android revision
`877203a7cf9b79973d44a9db716490f95d820110` supplies the target Waipio DT,
NRT QoS/memory-type/write OT and target physical halt/idle policy.

The target [Android VBIF ops](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/877203a7cf9b79973d44a9db716490f95d820110/vendor/qcom/opensource/display-drivers/msm/sde/sde_hw_vbif.c)
bind `get_xin_halt_status` unconditionally to a helper reading
VBIF+0x204 **high16**. The target `sde_vbif_get_xin_status` consumes this
helper and requires clock-status clear. Accordingly this core uses request
bits5/6 and ACK bits21/22. Mainline presently reads low16 at the same offset;
this difference is explicit and not probed by guessing both layouts.
Independent low-bit echo cannot satisfy the target fence. Hardware target
validation is still required. Android's access-prohibited-is-idle shortcut
is not used.

The [target Waipio catalog](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/877203a7cf9b79973d44a9db716490f95d820110/vendor/qcom/proprietary/display-devicetree/display/waipio-sde-common.dtsi)
sets mixer width2560, NRT QoS3 at all eight levels and memory type3. XIN6
write OT is2/6/16 at the documented pixel-rate thresholds, otherwise32.
No default READ OT exists in the captured target, so XIN5's read OT field
is preserved. Unrelated whole-register Android shareability clears are not
copied into this shared-bank core.

## Validation and remaining integration

Run `python3 Platforms/CranePkg/Tests/run_mdss_dpu_writeback_tests.py --output
<receipt-directory>` for native/NT ASan+UBSan tests against the actual core
and actual IRQ ledger. The fixture independently models configuration,
flush/start/reset effects, raster writes, IRQ status and high-field halt ACK.
It covers both output orientations, untouched guards/padding, geometry,
full-mapping bounds, retained partial/error/nonfinal effects, dropped writes,
stale/low-only ACK, exact event identity, every callback fault phase, current
off rechecks and no-IO quarantine. Host models are not a hardware grant.
`check_mdss_writeback_arm64.py` compiles actual EFI and real WDK objects.

After core and independent review, the library is registered in the Crane
catalogue/DEC and managed Windows SDK. It remains absent from native DSC/FDF
and has no active Present caller. Operative Present needs the real Root WB topology
and independent bandwidth owner, typed synchronized kick/waiter, READ/RW
primary remap journal, scanout/cursor pause+off fence, staging and crash CPU
gates, device-write visibility and READ restoration before POST. Successful
hardware Present writes reserved primary directly; no normal result-to-primary
CPU copy follows. These services are outside this core.

## Licensing and attribution

The interface and implementation deliberately use GPL-2.0-only because this
portable register/sequencing port derives from GPL Linux/Qualcomm display
sources. This choice does not relabel the separate original MIT IRQ interface
or MIT test code. Upstream notices include:

- Linux `dpu_hw_wb.c`: Copyright (c)2022 Qualcomm Innovation Center, Inc.
  All rights reserved.
- Linux `dpu_hw_sspp.c`: Copyright (c)2015-2018, The Linux Foundation.
  All rights reserved.
- Android `sde_hw_wb.c`: Copyright (c)2021-2022 Qualcomm Innovation Center,
  Inc. All rights reserved; Copyright (c)2015-2021, The Linux Foundation.
  All rights reserved.

The exact upstream files/revisions and hashes are preserved in the plan's
`sources.json` and test receipts. No upstream notice was removed from its
source file; this new port records source attribution here and in its header.
