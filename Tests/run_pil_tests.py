#!/usr/bin/env python3
"""Run independent native PIL fixtures with address/undefined sanitizers."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [crane / "Include", root / "MU_BASECORE/MdePkg/Include",
            root / "MU_BASECORE/MdePkg/Include/X64"]
with tempfile.TemporaryDirectory(prefix="crane-pil-") as directory:
    executable = str(Path(directory) / "pil")
    command = ["clang", "-std=c11", "-fshort-wchar", "-g", "-O1",
               "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
               "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", *["-I" + str(p) for p in includes]]
    subprocess.run(command + [str(crane / "Tests/PilTest.c"),
                   str(crane / "Library/PILCrLib/PilElf.c"),
                   str(crane / "Library/PILCrLib/Pil.c"),
                   str(crane / "Library/PILCrLib/PilUefi.c"), "-o", executable], check=True)
    subprocess.run([executable], check=True)
    subprocess.run(command + [str(crane / "Tests/PilDxeLifecycleTest.c"),
                   str(crane / "Library/PILCrLib/PilClients.c"),
                   str(crane / "Library/PILCrLib/PilHandoffUefi.c"),
                   str(crane / "Library/RpmhArcVoteLib/RpmhArcVote.c"),
                   str(root / "MU_BASECORE/MdePkg/Library/BaseLib/CheckSum.c"),
                   str(crane / "Library/PILCrLib/PilElf.c"), "-o", executable], check=True)
    subprocess.run([executable], check=True)
