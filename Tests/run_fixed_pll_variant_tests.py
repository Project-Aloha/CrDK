#!/usr/bin/env python3
"""Exercise the actual fixed-PLL core with LUCID and EVO layouts. SPDX-License-Identifier: MIT."""
from pathlib import Path
import subprocess
import tempfile

pkg = Path(__file__).resolve().parents[1]
edk = pkg.parents[1] / 'MU_BASECORE/MdePkg/Include'
with tempfile.TemporaryDirectory(prefix='crane-fixed-pll-') as tmp:
    exe = Path(tmp) / 'test'
    subprocess.run([
        'clang', '-std=c11', '-fshort-wchar', '-O1', '-g',
        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
        '-fno-sanitize-recover=all', '-I' + str(pkg / 'Include'),
        '-I' + str(edk), '-I' + str(edk / 'X64'),
        str(pkg / 'Tests/FixedPllVariantTest.c'),
        str(pkg / 'Library/ClockLib/fixed_pll.c'), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
