#!/usr/bin/env python3
"""Compile actual GMU/SQE parser under native/NT ASan+UBSan."""
import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
c = Path(__file__).resolve().parents[1]
r = c.parents[1]
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
(o / 'artifacts.json').unlink(missing_ok=True)
sources = [c / 'Tests/AdrenoFirmwareCrTest.c',
           c / 'Library/AdrenoFirmwareCrLib/AdrenoFirmware.c']
inputs = {*sources, c / 'Include/Library/AdrenoFirmwareCrLib.h',
          c / 'Library/AdrenoFirmwareCrLib/AdrenoFirmwareCrLib.inf', Path(__file__).resolve()}
runs = []
for nt in (False, True):
    flags = ['-std=c17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    if nt:
        flags += ['-D_KERNEL_MODE', '-I' + str(c / 'Tests/KernelMocks')]
    else:
        flags += ['-fshort-wchar', '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                  '-I' + str(r / 'MU_BASECORE/MdePkg/Include/X64')]
    flags += ['-I' + str(c / 'Include')]
    for s in sources:
        dep = subprocess.check_output(['clang', *flags, '-MM', str(s)], text=True)
        inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n', '').split(':', 1)[1]))
    before = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    exe = o / ('adreno-firmware-nt' if nt else 'adreno-firmware-native')
    cmd = ['clang', *flags, *map(str, sources), '-o', str(exe)]
    subprocess.run(cmd, check=True)
    x = subprocess.run([str(exe)], text=True, capture_output=True)
    log = o / (exe.name + '.log')
    log.write_text(x.stdout + x.stderr)
    if x.returncode:
        print(x.stdout + x.stderr, end='')
        x.check_returncode()
    if before != {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}:
        raise RuntimeError('Compiled input changed during tests')
    print(x.stdout, end='')
    runs.append({'nt': nt, 'command': cmd, 'output': x.stdout,
                 'log': str(log), 'binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()})
(o / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': 'Actual portable parsing only; no firmware/hardware/authentication claim',
    'runs': runs, 'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)}
}, indent=2) + '\n')
