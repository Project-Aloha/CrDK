#!/usr/bin/env python3
"""Compile the MDP XML reader and exercise the HDK8450 panel files."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
panel_dir = root / "Platforms/WaipioCrPkg/Device/qcom-hdk8450/Binaries/RawFiles"
with tempfile.TemporaryDirectory(prefix="crane-mdp-panel-") as tmp:
    executable = str(Path(tmp) / "mdp-panel-test")
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include/X64"),
        str(crane / "Tests/MdpPanelTest.c"),
        str(crane / "Driver/MDSSCrDxe/MdpPanel.c"),
        str(crane / "Driver/MDSSCrDxe/MdpDsiSequence.c"),
        "-o", executable,
    ], check=True)
    primary = [

        str(panel_dir / "Panel_lt9611uxc_dsi2hdmi_vid.xml"),
        str(panel_dir / "Panel_r66451_60hz_fhd_plus_dsc_vid.xml"),
        str(panel_dir / "Panel_truly_1080p_cmd.xml"),
    ]
    others = [str(path) for path in sorted(panel_dir.glob("Panel*.xml"))
              if str(path) not in primary]
    subprocess.run([executable, *primary, *others], check=True)
