# AudioGraphCrLib

This portable owner executes a bounded, in-band AudioReach command sequence: GET_SPF_STATE, GRAPH_OPEN, required calibration SET_CFG, endpoint SET_CFG, and an explicit GRAPH_CLOSE. It compiles its own immutable plan with AudioCalCrLib and decodes replies with AudioGprCrLib. The transport and graph authority are retained caller bindings. The library contains no MMIO, allocation, IRQ, timer, filesystem or OS driver operations.

`Tools/AudioGraphSession` is its executable consumer: it composes the actual compiler/session/GPR/GLINK libraries with a scripted remote SMEM/APM peer. The peer is a host-only test authority and is excluded from the library INF/props. There is no active native or Windows audio binding, no PREPARE/START, no DSP buffer mapping and no playback/capture claim.

## Ownership and entry points

Initialize a virgin `CR_AUDIO_GRAPH_PORT` once for a globally reserved AP source port and stable owner identity. Its monotonically increasing session and token counters survive all sessions, channel resets, D3 and PnP restarts. Do not zero, copy or reconstruct this object to reuse a port. This library does not allocate a global GPR port or make driver unload safe; the eventual permanent OS provider must retain that reservation until old deliveries are impossible.

`CrAudioGraphInit` compiles an explicit ACDB/profile into caller-owned storage, rejects external module links, builds the targeted CLOSE, and burns five distinct tokens. It performs no transport or authority calls. It preserves the session/port on failure; plan/compiler workspace is disposable scratch until successful admission. Request host/token fields are replaced by the permanent owner. All other profile values remain explicit. The 153,288-byte plan and 65,536-byte CLOSE storage must not be placed on a kernel stack.

The public structures expose storage sizes only; all fields become private after initialization. Keep the session, plan/storage, port, bindings and callback code stable and serialized. Receive/State copy complete bounded events into an eight-entry mailbox, never retain transport input pointers and never call the transport/provider. They may be invoked synchronously during a binding call. Tick and RequestClose reject reentry. The future OS adapter must serialize the mailbox against Tick, marshal IRQ Wake to its worker, and never hold a mailbox spin lock while calling a provider. This increment does not implement that OS callback/rundown adapter.

Tick dispatches copied events and makes at most one Send. The pending identity and deadline exist before Send; its explicit NotAccepted/Accepted/Unknown disposition is recorded before queued replies are dispatched. A success return meaning queued bytes is distinct from a matching APM acknowledgement. Backpressure can retry a definitely unaccepted packet until its existing deadline; accepted or uncertain packets are not blindly resent. Deadlines apply when the worker dispatches replies, so delayed worker execution is conservative: a reply processed after the deadline cannot convert timeout to success.

Inspect `CrAudioGraphGetStatus`, particularly Failed, Pending, GraphPossible and NamespaceHeld, even when Tick returns an error. An error return never authorizes freeing state. `DspStatus/DspOpcode` retain the first nonzero DSP result; `CleanupDspStatus/CleanupDspOpcode` separately retain CLOSE results. Error and CleanupError preserve local/transport failure information. Configured means all three graph/configuration commands were acknowledged; it does not establish hardware or stream readiness. Releasing retains a failed namespace release. Ended is reached only after graph and namespace retirement; it still does not retire the named GLINK subscription or drain OS work.

## Admission and recovery authority

The binding's Admit must reserve the exact owner/session/host-port/epoch/plan claim, prove an empty owned graph namespace and verify matching firmware, board profile and muted physical endpoint prerequisites. Nonzero cookies returned with failure are retained and released explicitly. No production implementation supplies an automatic readiness boolean. Graph IDs and external modules cannot be borrowed from another owner in this version.

The session requires matching nonzero channel, ADSP and audio-PD generations. Current GLINK channel state alone is insufficient. Any offline or differing generation event closes forward admission permanently for that session. A new generation, SPF ready reply, service End, timeout or local reset does not discharge remote ownership.

An accepted OPEN, including a failed or timed-out OPEN, can own remote SGs. A matching completed error permits targeted cleanup, but a missing completion cannot safely be raced by CLOSE. Late matching completion in an unchanged epoch permits cleanup while preserving the original timeout/error. A nonzero or timed-out CLOSE preserves the entire unresolved SG set, including partial-close uncertainty. No CLOSE_ALL, ignored required calibration failure or NOT_FOUND-as-retired shortcut exists.

The authority's Retired callback is an exact query of the retained cookie and old claim. Exact success must prove that this graph generation is gone **and that old queued/in-flight command bytes cannot reach a later firmware instance**. It cannot be inferred from a counter or transport close. The scripted peer tests this distinction with a partially transmitted OPEN. Release then drops the exact namespace reservation; any non-success retains it. The session never independently restarts or unloads ADSP shared with charger/UCSI.

This proof is graph-only. Future OOB/persistent calibration and PCM streaming need separate retained DMA/writer/mapping journals and SMMU/PIL retirement. A graph CLOSE or audio-PD restart is not a general DSP-DMA safety certificate. A real OS integration also needs an explicit transfer of unresolved ownership to the reset coordinator before dropping an independent audio PIL lease.

## Wire and corpus scope

APM routing is AP domain3 to ADSP domain2, destination APM port1. Replies must match source/destination domains and ports, wire token, expected original opcode, exact payload and the retained generation tuple. GPR extended headers remain supported. SPF needs an explicit ready-state reply; BASIC success alone is insufficient.

OPEN/calibration/endpoint packets are the compiler's owned bytes. Data and control link peer MIIDs must all belong to the selected plan. The existing qcm6490 headphone diagnostic contains an external `0x4133` link and is intentionally rejected unchanged. The accepted corpus has 50 closed plans and 66 external-link plans; the other 2,357 requests retain C1a's unsupported/empty outcomes. Public data is format/command test material and is not HDK8450 tuning.

CLOSE uses module1's SUB_GRAPH_LIST `0x08001005`, selected SG order, zero parameter error field and eight-byte padding, followed by the exact inter-SG connection parameter bytes that were opened. It names only this session's selected graph and never invents successful per-SG cleanup from an aggregate error.

## Sources and validation

Implementation references retain Qualcomm's BSD notice in LICENSE:

- [AudioReach Graph Services 3094583c726710b66eb0204fc5d6a7b336d9e575](https://github.com/AudioReach/audioreach-graphservices/tree/3094583c726710b66eb0204fc5d6a7b336d9e575): `gsl/src/gsl_graph.c` graph/configuration sequencing and `gsl_graph_close_sgids_and_connections`, `gsl_common.c` transaction behavior, `spf/api/apm/{apm_api,apm_module_api,apm_sub_graph_api}.h` wire formats.
- [OnePlus Android audio-kernel 877203a7cf9b79973d44a9db716490f95d820110](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/tree/877203a7cf9b79973d44a9db716490f95d820110/vendor/qcom/opensource/audio-kernel): `ipc/gpr-lite.c`, `dsp/audio_pdr.c` and `dsp/audio_notifier.c`. Android confirms that audio-PD readiness is separate from the common ADSP edge.
- [AudioReach configuration d9b13696adc1e0fd34a6ce8ef383763a981574b9](https://github.com/AudioReach/audioreach-conf/tree/d9b13696adc1e0fd34a6ce8ef383763a981574b9): 16 genuine fixtures already pinned by C1a.

`Tests/run_audio_graph_tests.py --output DIR` builds the unchanged upstream ACDB oracle, runs all 2,473 genuine requests, and composes real production cores with the independently encoded peer under native/NT ASan and UBSan. It covers all 20 intents, all three OPEN-handshake orders, fragmentation/backpressure, sibling PMIC/QRTR traffic, errors at every phase, partial cleanup, token exhaustion/reuse prevention, typed aliases, callback ordering, timeout/late/duplicate/malformed/extended replies, independently changing epochs, queued old commands, mailbox overflow and retained release. Synthetic control-link mutations are clearly separated from genuine corpus results. `check_audio_graph_arm64.py` compiles actual EFI and WDK ARM64 objects. These qualify software behavior only; matching HDK calibration, actual audio-PD/provider contracts, SoundWire/macros/WCD/WSA and a live OS frontend remain required.
