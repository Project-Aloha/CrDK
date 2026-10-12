BatteryChargerCrLib implements the SM8450 legacy battery service on PMIC GLink owner32778. The native BatteryChargerCrDxe observes it through UCSICrDxe's fixed owner subscriptions. UCSICrDxe remains the sole GLink/IPCC/SMEM/PIL transport owner; the battery observer installs no receiver IRQ and never independently starts or stops ADSP.

The wire and units are qualified against OnePlus Android SM8450 commit `449760504b3a75728eb60479f5b34dc51cb263ce`, [`drivers/power/supply/qti_battery_charger.c`](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/power/supply/qti_battery_charger.c), and Linux commit `a53261b60834ce617cc4f3c7723dda8285c59789`, [`drivers/power/supply/qcom_battmgr.c`](https://github.com/sunflower2333/linux/blob/a53261b60834ce617cc4f3c7723dda8285c59789/drivers/power/supply/qcom_battmgr.c). The sc8280xp battery protocol has another layout and is excluded.

| Snapshot field | GET opcode | Property | Source unit / exported unit |
|---|---:|---:|---|
| Present |0x30|2|0/1 |
| Status |0x30|0|Linux power_supply status0..4 |
| SOC |0x30|4|hundredths percent / hundredths percent0..10000 |
| Voltage |0x30|7|microvolts / microvolts |
| Current |0x30|9|signed microamps / signed microamps |
| Temperature |0x30|12|signed hundredths Celsius / milli Celsius |
| USB online |0x32|0|0/1 |

Requests are24-byte owner/type/opcode/battery_id/property/value packets; battery_id and value are zero. Responses are24-byte owner/type/opcode/property/value/return-code packets. All integers are LE32, and a server error or out-of-range value never becomes a valid measurement. Requests are asynchronous and bounded to one outstanding RPC. The native timer advances the state machine every100ms, with a1s response deadline and1s pause after the last request of a sampling cycle. A field older than5s loses validity even when its last diagnostic value remains cached.

There is no transaction ID. A pending or timed-out property's opcode/property pair cannot be reused across a same-ADSP GLINK/PDR change or offline interval until its exact late reply is observed, or a qualified strictly newer nonzero ADSP generation arrives. Zero or lower generation snapshots make the service unavailable while preserving the highest qualified epoch and its property fences. Other unfenced properties continue sampling. A late reply retires its fence but is discarded as a measurement. A mismatched or malformed response cannot complete the active request. Every remote generation change/offline transition and native ExitBootServices invalidates all fields before transport IRQ drain and PAS shutdown. UI `GetSnapshot` is pure and performs no RPC; EFI_SUCCESS means current SOC is valid, and EFI_NOT_READY still copies diagnostics and other field validity.

Charging stays under ADSP firmware policy. This observer sends batteryGET0x30 and USBGET0x32 only: it does not issue batterySET0x31, USBSET0x33, charge-current/thermal limits, ship mode, wireless updates, or charge-enable policy. Android initializes its power supplies and reads properties before notification enable; its explicit current/ICL writes are user/thermal setters or restoration of previous nonzero requests after subsystem restart. This is evidence that reading does not require an AP charge-control write. It does not prove that a physical battery is charging: the app animates only an actual valid Charging status, and an unavailable or NotCharging response remains visible as such. Battery transport/ADSP/hardware validation is required on the board.

Verification uses actual core and DXE source under ASan/UBSan:

```sh
python3 Platforms/CranePkg/Tests/run_battery_charger_tests.py --output .tmp/battery-core
python3 Platforms/CranePkg/Tests/run_battery_dxe_tests.py --output .tmp/battery-dxe
python3 Platforms/CranePkg/Tests/run_usb_typec_tests.py
python3 Platforms/CranePkg/Tests/check_battery_arm64.py --output .tmp/battery-arm64
```

These test wire frames/units, all10001 legal SOC values, timeouts and late-reply fences, malformed and server-error responses, offline/restart validity, pure UI snapshots, subscriber identity and reentry gates, native unload refusal while callback storage is retained, and retry after failed event/timer/protocol/subscriber cleanup. The ARM64 checker compiles the actual EFI adapters/core and real WDK core. It does not emulate Windows installation, firmware activation or physical charging.

Windows may opt into `CrBatteryChargerEnableStatistics`, which adds technology
13, cycle count 15 and time-to-empty average 20 to the same serialized GET
scheduler. The original technology-only opt-in still sends eight properties;
the native default still sends seven and its revision-1 snapshot is unchanged.
The optional statistics accessor copies independent five-second validity bits.
Negative signed values, server errors, timeout and peer loss cannot publish a
measurement. Each optional opcode/property pair has the same late-reply fence
as mandatory requests, and an unsupported property does not stop those requests.

Android maps property 20 directly to `POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG`;
[the pinned power supply class documentation](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/Documentation/power/power_supply_class.rst)
defines that unit as seconds. The returned estimate describes the remote's
observed load; it is not a capacity or a way to calculate runtime at an arbitrary
requested discharge rate. A property only becomes supported data after its
exact successful response. Hardware support on this HDK remains to be tested.
