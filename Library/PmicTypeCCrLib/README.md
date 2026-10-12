# Qualcomm PMIC Type-C observation

`PmicTypeCCrLib` consumes the SM8450-HDK PMIC wire packets already received by the single PEP GLink service. The OS UCSI client remains the sole owner of USBC commands (owner 32779). The broker owns PAN (32780), sends `PAN_ENABLE`/`PAN_ACK`, and exposes an observed route with a generation through `WdfTypeCPolicyPublic.h`. Battery packets (32778) remain in their separate client queue. No second receiver or IRQ is created.

The source-derived protocol is GPL-2.0-only. References are the checked-out primary source, not binary driver guesses:

- Linux commit `a53261b60834ce617cc4f3c7723dda8285c59789`, `drivers/soc/qcom/pmic_glink_altmode.c`: 24-byte PAN request, commands 0x10/0x11, 32-byte SC8280x-format notification; port/orientation/mux at bytes 12/13/14, DP pin and HPD bits at byte 20, and SVID in the notification opcode's upper 16 bits. The SM8450 Android source uses the same notification/request sizes.
- OnePlus SM8450 Android commit `449760504b3a75728eb60479f5b34dc51cb263ce`, `drivers/soc/qcom/altmode-glink.c` and `drivers/usb/typec/ucsi/ucsi_glink.c`: owners, opcodes, enable/ack commands and callback completion semantics. PAN reply tails are not interpreted as an invented result code; Linux and Android only complete the header-correlated outstanding request. Captured HDK replies are 16 bytes.
- Linux `drivers/usb/typec/ucsi/ucsi.h` and Android's corresponding file: connector status uses change bits 0..15, connected bit 19, power direction bit 20, partner flags 21..28, partner type 29..31. The observer supports the actual UcmUcsiCx 1.0, 48-byte UCSI 1.x mailbox; it does not truncate a UCSI 2.x reply.

A usable status requires an accepted OS `GET_CONNECTOR_STATUS` for connector 1, exact successful USBC write response, a freshly accepted OS read, and a completed 9..16-byte connector status without busy/error/cancel/unsupported flags. A completed command is consumed once. Observed USBC read/write requests have the same five-second bound as the actual PPM client. Receive checks expiration before interpreting the reply; the pure cached query also checks expiration without sending an RPC. A timed-out USBC operation invalidates route admission until a verified new ADSP generation, so a late reply cannot revive it. Orientation/mux changes invalidate status until the OS obtains a fresh connector status. HPD-only events update their event sequence without changing the route generation. Unknown port/orientation/mux/SVID or malformed responses cannot establish an active SuperSpeed or DP route. A qualified connector status can still report USB2 while PAN is unavailable.

The wire contains no transaction identifier. PAN has one outstanding request, a five-second deadline and no automatic reuse after an uncertain send or timeout. Those failures retain the original outstanding request until its exact late response or a verified new authenticated ADSP generation. A late response can finish cleanup but does not reopen PAN command admission in the same ADSP generation. An unanswered request can therefore retain the last transport lease until remote completion or reboot. Elapsed time, target reopening and a local IRQ stop do not prove firmware completion.

The final client `End` pumps a bounded amount of real wire work to retire an outstanding PAN request and queued notification acknowledgement, retaining the same client token and physical PIL/SMEM/SEND/rail/IRQ dependencies while incomplete. Quarantine is irreversible: the physical service closes access and drains its direct IRQ/DPC before closing broker admission, then retains mappings and dependencies without MMIO or normal cleanup. A client token can be retired as metadata during quarantine; this does not retire the underlying physical owner.

`GetState` is a PASSIVE, pure snapshot which independently checks the current underlying wire and live OS UCSI owner. Failure leaves the caller's outputs unchanged. Its success and generation grant no reset privilege, exclusive PHY claim, DMA ownership or D0 lease. Those remain separate source-qualified interfaces and native writer-transfer evidence.

Validation runs actual source with independent raw packets:

```
python3 Platforms/CranePkg/Tests/run_pmic_typec_tests.py --output .tmp/typec-core
python3 ../CrWDDK/tests/run_glink_service_tests.py --output ../CrWDDK/Build/typec-service
python3 ../CrWDDK/tests/run_glink_resources_integration_tests.py --output ../CrWDDK/Build/typec-physical
```

The host boundaries model firmware responses and public NT/provider calls. They do not establish Windows hardware boot, PHY takeover or deployed USB role functionality.
