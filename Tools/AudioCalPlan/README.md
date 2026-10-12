# AudioCalPlan

This host executable loads an ACDB file and an explicit text profile, invokes the production portable `AudioCalCrLib`, writes three GPR command files plus driver metadata, and prints a JSON manifest. It never opens a device or sends a command. The public fixture profile is for offline tests; matching HDK8450 calibration is still required before board bring-up.

Build it through `Tests/run_audio_cal_tests.py --output OUTPUT`. Run the resulting `AudioCalPlan-native ACDB_FILE PROFILE_TEXT EXISTING_OUTPUT_DIR`; stdout contains the manifest. The profile syntax is demonstrated by `qcm6490-headphones-test.profile`. Every format, backend, tag, host port and command token is explicit. Non-success compiler results emit no files. Output directories should be separate for separate runs; files left from an older successful run are not deleted on failure.

A successful run writes `graph-open.gpr`, `initial-calibration.gpr`, `endpoint-config.gpr` and `driver-properties.bin`. Read `Library/AudioCalCrLib/README.md` for supported cases, provenance and ownership prerequisites. These bytes are not a supported standalone hardware replay script.

Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause. Referenced Qualcomm source/license attribution is preserved with the library.
