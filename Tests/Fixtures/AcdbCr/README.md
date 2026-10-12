# ACDB fixture provenance

`inventory.json` records sixteen unchanged real public files from [Qualcomm AudioReach conf](https://github.com/AudioReach/audioreach-conf/tree/d9b13696adc1e0fd34a6ce8ef383763a981574b9), including their SHA256 and independent byte/chunk/HEAD inventory. The runner reads these files from `.vscode/audioreach-conf` by default or `--fixtures PATH`, verifies every hash, and builds the C oracle from this saved inventory. It does not download or rewrite fixtures. Clone the pinned repository into `.vscode` to reproduce. The binary files are not vendored here; upstream BSD-3-Clause license is retained alongside the metadata.

None is HDK8450 calibration. They test the container format only and must not be sent to HDK8450 ADSP or used to unmute its speakers. C tests additionally build explicitly synthetic edge cases and mutate in-memory copies; those are not vendor calibration files.
