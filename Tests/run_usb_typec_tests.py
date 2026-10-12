#!/usr/bin/env python3
"""Compile the native Type-C transport and exercise independent wire fixtures."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [crane / "Include", root / "MU_BASECORE/MdePkg/Include",
            root / "MU_BASECORE/MdePkg/Include/X64", root / "MU_BASECORE/EmbeddedPkg/Include"]
with tempfile.TemporaryDirectory(prefix="crane-typec-") as temporary:
    executable = str(Path(temporary) / "transport")
    subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
                    str(crane / "Tests/UsbTypeCTransportTest.c"),
                    str(crane / "Library/UcsiCrLib/Glink.c"),
                    str(crane / "Library/UcsiCrLib/PmicGlink.c"),
                    str(crane / "Library/UcsiCrLib/Qrtr.c"),
                    "-o", executable], check=True)
    subprocess.run([executable], check=True)
    executable = str(Path(temporary) / "charger-log")
    subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
                    str(crane / "Tests/ChargerLogTest.c"),
                    str(crane / "Library/UcsiCrLib/ChargerLog.c"),
                    "-o", executable], check=True)
    subprocess.run([executable], check=True)
    executable = str(Path(temporary) / "qrtr")
    subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
                    str(crane / "Tests/QrtrServiceTest.c"),
                    str(crane / "Library/UcsiCrLib/Qrtr.c"),
                    "-o", executable], check=True)
    subprocess.run([executable], check=True)
    executable = str(Path(temporary) / "lifecycle")
    subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
                    str(crane / "Tests/UcsiDxeLifecycleTest.c"),
                    str(crane / "Library/UcsiCrLib/PmicGlink.c"),
                    str(crane / "Library/UcsiCrLib/Qrtr.c"),
                    str(crane / "Library/UcsiCrLib/ChargerLog.c"),
                    "-o", executable], check=True)
    subprocess.run([executable], check=True)
    executable = str(Path(temporary) / "ipcc")
    subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
                    str(crane / "Tests/IpccIrqTest.c"),
                    str(crane / "Library/UcsiCrLib/Ipcc.c"),
                    "-o", executable], check=True)
    subprocess.run([executable], check=True)
