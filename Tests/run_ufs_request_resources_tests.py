#!/usr/bin/env python3
"""Compile unchanged production request-release logic against a delayed DMA mock."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
edk = crane.parents[1] / 'MU_BASECORE'
source = (crane / 'Override/MU_BASECORE/MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThru.c').read_text()
start = source.index('EFI_STATUS\nUfsReleaseRequestResources (')
end = source.index('\n}\n', start) + 3
with tempfile.TemporaryDirectory(prefix='crane-ufs-release-') as tmp:
    tmp = Path(tmp)
    (tmp / 'UfsRequestRelease.inc').write_text(source[start:end])
    includes = [tmp, crane / 'Include', edk / 'MdePkg/Include', edk / 'MdePkg/Include/X64',
                edk / 'MdeModulePkg/Include', crane / 'Override/MU_BASECORE/MdeModulePkg/Bus/Ufs/UfsPassThruDxe']
    exe = tmp / 'release-test'
    subprocess.run(['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', *['-I' + str(p) for p in includes],
                    str(crane / 'Tests/UfsRequestResourcesTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
