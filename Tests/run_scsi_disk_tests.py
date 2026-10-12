#!/usr/bin/env python3
"""Exercise the production ScsiDisk MODE SENSE path with a fake SCSI transport."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
edk2 = crane.parents[1] / "MU_BASECORE"
source_dir = crane / "Override/MU_BASECORE/MdeModulePkg/Bus/Scsi/ScsiDiskDxe"
source = (source_dir / "ScsiDisk.c").read_text()
chunks = []
for signature in ("VOID *\nAllocateAlignedBuffer (", "VOID\nFreeAlignedBuffer (",
                  "EFI_STATUS\nIsWriteProtected ("):
    start = source.index(signature)
    end = source.index("\n/**", start)
    chunks.append(source[start:end])
with tempfile.TemporaryDirectory(prefix="crane-scsi-disk-") as tmp:
    (Path(tmp) / "ScsiWriteProtection.inc").write_text("\n".join(chunks))
    exe = Path(tmp) / "scsi-test"
    cmd = ["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
           "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    for directory in (edk2 / "MdePkg/Include", edk2 / "MdePkg/Include/X64",
                      edk2 / "MdeModulePkg/Include", source_dir, Path(tmp)):
        cmd.append("-I" + str(directory))
    subprocess.run(cmd + [str(crane / "Tests/ScsiWriteProtectionTest.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
