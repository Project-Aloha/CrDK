#!/usr/bin/env python3
"""Run actual MDSS GOP publication/clear with mocked hardware providers."""
from pathlib import Path
import subprocess
import tempfile
crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
with tempfile.TemporaryDirectory(prefix="crane-mdp-gop-") as tmp:
    exe = str(Path(tmp) / "gop-test")
    includes = [crane / "Include", root / "Platforms/MUCommonCrPkg/Include",
                root / "MU_BASECORE/MdePkg/Include", root / "MU_BASECORE/MdePkg/Include/X64",
                root / "MU_BASECORE/MdeModulePkg/Include", root / "MU_BASECORE/ArmPkg/Include"]
    subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined",
                    *["-I" + str(p) for p in includes],
                    str(crane / "Tests/MdpGopPublishTest.c"),
                    str(crane / "Driver/MDSSCrDxe/MdpPanel.c"),
                    str(crane / "Driver/MDSSCrDxe/MdpDsiSequence.c"),
                    str(crane / "Driver/MDSSCrDxe/MdpScanout.c"),
                    str(crane / "Library/MdssCrLib/Mdss.c"), str(crane / "Library/MdssHandoffCrLib/Handoff.c"), "-o", exe], check=True)
    source = root / "Platforms/WaipioCrPkg/Device/qcom-hdk8450/Resources/Panel_lt9611uxc_dsi2hdmi_vid.xml"
    subprocess.run([exe, str(source), str((1 << 12) | (1 << 15)), "baseline"], check=True)
    for scenario in ("handoff", "handoff-busy", "handoff-fence", "handoff-retire", "handoff-register"):
        subprocess.run([exe, str(source), str((1 << 12) | (1 << 15)), scenario], check=True)
    xml = source.read_text() + """
<DSIInitSequence>05 11 00
ff 78
15 51 aa
</DSIInitSequence>
<DSITermSequence>05 28
FF 14
05 10 00
</DSITermSequence>
"""
    for bllp, eof in ((True, True), (False, True), (True, False), (False, False)):
        variant = xml + "<BorderColor>0x123456</BorderColor>"
        for tag, enabled in (("DSILowPowerModeInBLLP", bllp), ("DSILowPowerModeInBLLPEOF", eof)):
            variant = variant.replace(f"<{tag}>True</{tag}>", f"<{tag}>{str(enabled)}</{tag}>")
        path = Path(tmp) / "panel.xml"
        path.write_text(variant)
        expected = (int(bllp) << 12) | (int(eof) << 15)
        subprocess.run([exe, str(path), str(expected)], check=True)
    oversized = source.read_text() + "<DSIInitSequence>05 11\n39 " + "aa " * 4093 + "</DSIInitSequence>"
    path.write_text(oversized)
    subprocess.run([exe, str(path), "0", "unsupported"], check=True)
