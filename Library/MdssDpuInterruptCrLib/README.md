# Owned SM8450 TOP0 events

This library is catalogued for shared CRDK use and has no active native consumer. `MdssDpuInterrupt.c` owns selected TOP0 event metadata; `MdssIrqSources.c` independently observes all source banks qualified by its target inventory. Neither function grants the physical interrupt line, register mappings, power, native writer retirement or a DMA mapping.

The common IO enum and callback layout live in `Include/Library/MdssDpuInterruptCrLib.h`. The source qualifier uses its separate `MdssIrqSourcesCrLib.h`; its register policy and pinned source evidence are documented in `Documentation/MdssInterruptSources.md`.

The ledger supports WB2 bit4 and DSI0 video INTF1 vblank bit27. Stable registration token addresses, owner identities, incarnations and generations reject copied/stale tokens. Enable changes preserve other bits. Capture acknowledges only observed enabled owned status, masks WB one-shot, and records an event only after successful mask/ack readback. A nonfinal status, dropped write or unexpected enabled source closes new arm admission and retains owners/jobs. Vblank delivery may coalesce to the latest sequence. An immediately reasserted status after acknowledgement is conservatively treated as uncertain.

Arm performs no writeback kick. The future actual WB producer must publish its immutable job/output identity and execute its short source-qualified final CTL kick under the same interrupt synchronization lock. A captured WB event is an interrupt transaction completion, never a DMA stop proof. Disarm, Discard and RetireJob retire only IRQ metadata; the real producer must stop/fence and drain DPC/output users before freeing buffers, withdrawing bandwidth or returning mappings.

The Windows backend is `CrWDDK/Common/CrMdssInterruptWdm.c`. It uses actual OSKAL registration, an actual PEP hardware token, a counted DIRQL admission gate, `KeSynchronizeExecution`, and a DPC event ledger. A failed/nonfinal OS connect retaining a pointer is disconnected without using that uncertain object for synchronization. ISR errors send a status wake and seal admission; Unregister can disconnect that sealed instance without further MMIO. Surprise quarantine does not mask/stop DMA or free any uncertain job. Running and queued DPCs drain before references can retire.

`CrWDDK/Common/CrMdssIrqRootWdm.c` now connects this backend to the permanent PEP display owner for optional PCFG6 assignment. It borrows the existing DPU/DSI0 mappings, owns four additional views, checks the retired native writers and current clock/rail/ICB votes, and retains a separate DP AUX clock vote. The extended display path acquires this owner before startup and arms INTF1 vblank afterward. Stop and POST transfer retire leaf metadata while the physical line is connected, then disconnect/drain callbacks before releasing mappings, votes and the hardware token. Partial failures retain their exact journals; fault shutdown seals admission and retains uncertain ownership. Empty coalesced DPC wakes are harmless.

The default PCFG2/PFLG14 assignment has no MDSS/IRQ group, so linking this service does not enable it on that profile. No parent hardware mask is demonstrated: fault wake/disconnect does not prove containment of a foreign asserted level source. The current producer registers vblank only; WB2 execution and writable DMA output remain separate work. Host/WDK validation does not establish physical IRQ or DMA-stop behavior on the board.

Validation commands:

```
python3 Platforms/CranePkg/Tests/run_mdss_dpu_interrupt_tests.py --output <native-and-NT-output>
python3 tests/run_mdss_interrupt_wdm_tests.py --output <Windows-host-output>
python3 tests/run_mdss_irq_root_wdm_tests.py --output <Windows-owner-output>
python3 tests/check_mdss_interrupt_wdk.py --output <Windows-WDK-output>
```

The Windows commands run from `CrWDDK`. Host tests execute actual sources with controlled MMIO/OS and lower-provider boundaries. The standalone WDK check compiles/links the backend, core, qualifier and OSKAL units. `tests/check_pep_host_wdk.py` checks the full imported PEP project, including the actual Root owner and display integration. Neither check deploys or demonstrates a hardware IRQ or writeback engine.
