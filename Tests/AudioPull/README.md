# Portable audio Pull regression

Run from the firmware workspace:

```
python3 Platforms/CranePkg/Tests/run_audio_pull_tests.py --output .tmp/audio-pull-tests
```

The exact Audio87 reviewed runtime/GLINK peer and labeled synthetic ACDB/request
fixtures are preserved here. The runner uses current canonical Graph, Pull,
PCM, Cal, ACDB, GPR and GLINK cores in EFI and NT host modes under ASan/UBSan.
It checks packet layout, both MAP journals, configuration, media/event replies,
graph CLOSE before reverse UNMAP, late timeout replies, source-loss retention
and no-effect backpressure. The peer is a test model, excluded from production
INF/props. These synthetic fixtures are not HDK calibration or installed
firmware evidence and do not establish an analog route, HAL DMA, a Windows
endpoint or audible output. Native DMA/View and actual authenticated PEP/PIL
composition have separate recorded tests; this suite does not replace them.

Origin: Audio87/91 development checkpoints, preserved source manifest
1166701889b673afc0cf4c9872e14fbbed2cde906f2a68bac139dc852d41bf98.
AudioGraphPeer retains its BSD-3-Clause notice. Production Pull retains its
complete LICENSE and pinned Qualcomm AudioReach references.
