# AudioGraphSession

This host diagnostic runs the actual AudioGraphCrLib, AudioCalCrLib, AudioGprCrLib and named GLINK core against a scripted SMEM/APM peer. It does not open a physical device or attach to a running ADSP.

Build and exercise it with:

```sh
python3 Platforms/CranePkg/Tests/run_audio_graph_tests.py --output .tmp/audio-graph-tests
.tmp/audio-graph-tests/AudioGraphSession-native \
  .vscode/audioreach-conf/qcom/qli/Kaanapali/acdbdata/acdb_cal.acdb \
  Platforms/CranePkg/Tools/AudioGraphSession/kaanapali-closed-test.profile
```

The test profile selects a genuine closed graph from the pinned public Kaanapali file. It is not HDK8450 calibration or a physical route. The profile syntax matches AudioCalPlan; its transport numbers are file-format placeholders, replaced by the session's permanent port/token owner. The tool compiles owned packets, releases input ACDB/workspace storage, then obtains simulated SPF/OPEN/SET_CFG/CLOSE replies through actual fragmented GLINK FIFOs. JSON identifies simulation and explicitly reports hardware_ready=false.

AudioGraphPeer is a test authority only. Its Admit records the exact claim against an empty simulated graph inventory. Its explicit retirement operation and callback require the same cookie/old claim, empty graph inventory and drained old command queues. Its finite script never supplies production readiness. The library INF/props excludes every file in this directory.

The full test harness uses all 50 closed genuine plans and preserves 66 external-module dependency rejections. The earlier qcm6490 headphone profile is deliberately rejected because it links to an unowned external module. Malformed/control-link test mutations are synthetic and never presented as stock calibration. Current limitations and the future live adapter requirements are in `Library/AudioGraphCrLib/README.md`.
