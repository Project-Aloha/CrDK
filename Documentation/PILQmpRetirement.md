# ADSP producer retirement and shared AOSS QMP

Capture68 (`capture68-3a500c5c7629.log`, lines3247..3251) reached EBS. PAS1
shutdown returned transport/service zero and cleared secure ownership. The
remaining timeout was in `QmpClose`, after the consumed `load_state off` and
this PIL owner's three ARC releases. `QmpClosing=1` identifies that phase;
the capture has no raw mailbox state/ACK values, so it does not identify the
exact acknowledgement that stalled. Windows booted and DSI remained active.

The previous code treated ADSP producer retirement as removal of the shared
AOSS transport: it wrote both channel/link DOWN and waited for both DOWN
ACKs. That completion contract is absent from the PAS consumer paths.
[Linux qcom_q6v5](https://github.com/torvalds/linux/blob/a53261b60834ce617cc4f3c7723dda8285c59789/drivers/remoteproc/qcom_q6v5.c)
sends `load_state off` during unprepare; its QMP reference release only drops
a device reference in
[qcom_aoss.c](https://github.com/torvalds/linux/blob/a53261b60834ce617cc4f3c7723dda8285c59789/drivers/soc/qcom/qcom_aoss.c).
The pinned
[Android PAS driver](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/remoteproc/qcom_q6v5_pas.c)
likewise shuts down PAS, sends off and releases power/proxy resources without
removing the shared QMP provider. Both AOSS provider removal functions send
DOWN without waiting for these ACKs. The separate Android
[msm_qmp mailbox](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/mailbox/msm_qmp.c)
consumer shutdown keeps the link established while withdrawing its channel.
These are distinct provider/consumer lifetimes; a local DOWN write does not
prove remote consumption or ADSP power-off.

`CrPilStop` now retains the actual PAS shutdown evidence and sends the off
request as before. It does not clear a timed-out payload length. After real
off consumption and the owner-specific ARC releases, a completed-handshake
producer retires only if two fenced observations find all eight AOP/APSS
link/channel state and ACK fields UP, unchanged valid mailbox magic/version,
the exact retained mailbox offset, a bounded stable mailbox size and pending
length zero. The transport remains UP for the next owner. All four software
QMP ownership/handshake flags clear only after this proof; crypto-path release
then runs. A failed crypto release retains that exact path for retry.

`QmpEstablished` records that the complete open handshake returned. Incomplete
setup keeps the prior partial-effect rollback path and its strict DOWN-ACK
requirements. Lack of such an acknowledgement retains the context and
prerequisites; this change promises no new cancellation/reset guarantee for
partial setup. A borrowed but incomplete transport also cannot pass the
established snapshot. A failed off request retains load-state, ARC demands
and crypto. A later retry never repeats an already completed PAS shutdown.

PIL owns no receive IRQ. Its native adapter uses direct mailbox reads, barriers
and architectural-timer busy delay. The EBS callback runs at TPL_NOTIFY;
the remote AOP continues independently. UCSICr drains its client and disables
only its LPASS receive signal before PIL stop. Synchronous RPMh completion
also polls the TCS status. No UEFI event, allocation, filesystem or lowered-TPL
worker is required for this QMP observation. This is AOSS messaging, separate
from USB/DP QMP PHY power sequencing.

Timeout diagnostics now include the first failing offset, observed value and
expected value. The EBS summary includes load-state, remaining ARC mask and
crypto path. PAS shutdown and producer retirement do not claim every physical
rail is off: the independent managed native boot-floor owner remains intact.
The existing PILH wire permits coherent UP or DOWN state/ACK pairs. Publication
still requires zero PIL ownership, consumed payload, successful native PAS
evidence, drained clients, final reserved-memory evidence and sealed ARC
ownership. An uncleared completed-handshake flag now also refuses publication.

`Tests/run_pil_qmp_retirement_tests.py --output <new-directory>` runs the actual
native/NT core with and without the SMCCC helper, a remote link-gated peer,
thirteen state/descriptor/pending faults, second-snapshot mutation, pending
off/retry, and the affected native adapter/EBS/publisher fixtures under
ASan/UBSan. The before proof compiles the saved original core and fails the
producer-retirement assertion. That model exposes the unsupported teardown
contract; it is not a measurement of the board's AOP implementation. Four
actual EFI ARM64 translation units compile independently. Firmware packaging
and a new physical EBS capture remain separately coordinated.
