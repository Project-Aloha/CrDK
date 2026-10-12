# SM8250 boot display retirement

This isolated, currently unselected library admits and physically stops the
recognized HDK8245 command display inherited from ABL before the cold display
core takes over. It is not a framebuffer driver and does not publish a GOP.

The native owner retains the actual inherited clock, power and interconnect
leases without retuning the running display. The core reads the actual route,
source geometry/address, IRQ banks, REGDMA, DSI/PHY and SMMU producer evidence.
It rejects unsupported live pipelines before changing hardware. Retirement
uses retained stages: stop software writers, mask IRQs, fence REGDMA, stop TE
and CTL, physically halt the selected source XIN, stop the host and DSC, gate
external clocks through their owner, and stop the SM8250 PHY. An error retains
dependencies and the old splash reservation for recovery.

`CheckFence` performs current physical checks for a subsequent SMMU producer
transaction. It does not grant stream ownership, retire inherited page tables,
or bypass the current `SmmuAttach` collision protection. Native integration and
SMMU transfer are separate, still-pending reviewed changes. The library's
state must remain alive after a potentially committed retirement effect.

Register sequences derive from Qualcomm's Android SM8250 tree, commit
`cad09e061ef6cf689a4a1e54d27562e0f042236a`, especially `techpack/display/msm/sde/`
and `techpack/display/msm/dsi/`. Board windows and physical wiring come from
the user-supplied `hdk8245_fdt`. Original source copies, hashes and the detailed
admission plan are retained in
`.tmp/waipio-continuation/hdk8245-panel/boot-retirement/`.

Do not substitute SM8450's VBIF ACK field, PHY layout, clock graph or handoff
record. The portable cold command library has its own separate qualification;
the existence of this native bootstrap library does not enable board display.

SPDX-License-Identifier: GPL-2.0-only

The qualified observer inventories twelve Android IRQ banks and all supplied
CTL/SSPP/interface routes. It accepts only a settled, single VIG0 or DMA0 linear
packed-RGB route to CTL0/LM0/PP0/DSC0/INTF1. It reads actual INTF status and both
PHY buffer and mux controls; a pending CTL flush or alternate live path is
rejected before writes. The producer callback must prove every mapped frame
byte through the inherited stage-1 context or real identity bypass.

StopTe retains the complete tearcheck engine while waiting for the current
transfer, then uses the original Android autorefresh debugbus helpers to exclude
queued work. Added selector readbacks detect dropped writes, and retained
journals restore both debug selectors. The waits are bounded to 34 ms and 1 s;
failure preserves access and the splash. SSPP pixel-extension settings are not
used as a DMA-off predicate. Full native/NT sanitizer, source-oracle and real
ARM64 compile receipts are under `boot-retirement/validation-v2/` and
`boot-retirement/stopte-composition/`; hardware and native resource binding
remain unvalidated until the separate integration is implemented.
