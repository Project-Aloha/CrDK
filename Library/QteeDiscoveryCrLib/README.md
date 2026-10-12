# QTEE discovery codec

MIT-licensed portable codec relocated from CrWDDK/TreeCr without changing the fixed service boundary. It discovers the existing HDCP2P2 and HDCP SRM objects through the Android SM8450 modern smcinvoke object ABI; no arbitrary object, callback, application upload, session or secure call is exposed.

The platform adapter owns the exact two pages, cache synchronization, current power admission and one globally serialized SCM conduit. This library never maps physical memory or executes SMC itself. Accepted calls preserve the exact lease and object/bridge journals on uncertainty. `CrQteeNotIssued` is accepted only for terminal first-operation capability refusal before bridge registration; it is not a continuation or a guessed raw response.

Source references and provenance remain documented in CrWDDK/TreeCr/README.md and provider-next.md. Android references: OnePlusOSS/android_kernel_oneplus_sm8450 at 449760504b3a75728eb60479f5b34dc51cb263ce, drivers/firmware/smcinvoke.c and qtee_shmbridge.c. No GPL source text is copied into this independent codec.
