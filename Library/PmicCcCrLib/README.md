# PM8150B CC observer

This portable backend observes PM8150B Type-C connector state through the shared SPMI provider. It does not require ADSP. The native adapter publishes the existing EFI Type-C protocol; the SM8450 ADSP/PAN backend remains a separate implementation.

Admission requires the exact HDK8150 or HDK8245 Android FDT topology, matching DAL arbiter ranges and the documented PM8150B v1/v2 revision tuple. HDK8245 explicitly has `qcom,batteryless-platform`; enabled fuel-gauge nodes and battery profiles are not proof of an installed battery. The two boards' interrupt bindings differ: HDK8150 uses legacy `qcom,pdc-sm8150` with a GIC-style SPI481 specifier; HDK8245 uses PDC pin1 through `interrupts-extended`. Both select source513 in the native PDC mapping.

The core subscribes to all eight CC IRQs, wakes for bounded snapshots, and performs no idle register polling. A snapshot is published only after two identical samples with stable attachment/debounce and no reported water/VBUS error. An IRQ invalidates the cached snapshot immediately. An inherited enabled PD PHY prevents inferring the negotiated data role from CC. PD-ready/DP state is not fabricated. SetDataRole/SetPowerRole are unsupported in this observation unit.

VBUS/VCONN enabling, role swaps, PD transmit/receive, charger settings and battery/profile programming are deliberately separate controller milestones and are not performed by this observer. Later backends must preserve the same shared arbiter/IRQ ownership and use the board-specific power policy.

Register/protocol definitions and sequences were checked against these primary sources:

- https://github.com/OnePlusOSS/android_kernel_oneplus_sm8150 at `1dd473abda05a72f6978c47b2a7d80828db6b426`: `drivers/power/supply/qcom/smb5-lib.c`, `smb5-reg.h`, `qpnp-smb5.c`, `drivers/usb/pd/qpnp-pdphy.c`, `include/linux/qpnp/qpnp-revid.h`, `drivers/irqchip/qcom/pdc-sm8150.c`.
- https://github.com/OnePlusOSS/android_kernel_oneplus_sm8250 at `cad09e061ef6cf689a4a1e54d27562e0f042236a`: corresponding SMB5, PD PHY and policy-engine sources.
- https://github.com/sunflower2333/linux at `a53261b60834ce617cc4f3c7723dda8285c59789`: `drivers/usb/typec/tcpm/qcom/`, `drivers/input/misc/pm8941-pwrkey.c`, and SM8150/SM8250 board device trees.

The reference Qualcomm files carry Copyright (c) 2017–2020, The Linux Foundation, and GPL version 2 notices. `PmicCc.c`, its public header, and `PmicCcCrDxe` are GPL-2.0-only; `COPYING` preserves the source-tree license. Shared ownership and EFI routing files retain their own MIT notices.
