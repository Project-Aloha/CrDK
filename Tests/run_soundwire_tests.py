#!/usr/bin/env python3
"""Compile actual SoundWire controller and real probe consumer under native/NT ASan+UBSan."""
import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[3])
a = p.parse_args()
c = Path(__file__).resolve().parents[1]
r = a.workspace.resolve()
base = r / 'Platforms/CranePkg'
upstream = r / '.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450/vendor/qcom/opensource/audio-kernel'
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
(o / 'artifacts.json').unlink(missing_ok=True)
sources = [c / 'Tests/SoundWireCrTest.c',
           c / 'Library/SoundWireCrLib/SoundWire.c', c / 'Library/SoundWireCrLib/Sm8450.c']
inputs = {*sources, c / 'Include/Library/SoundWireCrLib.h',
          c / 'Library/SoundWireCrLib/SoundWireCrLib.inf', Path(__file__).resolve()}
runs = []
for nt in (False, True):
    flags = ['-std=c17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    if nt:
        flags += ['-D_KERNEL_MODE', '-I' + str(base / 'Tests/KernelMocks')]
    else:
        flags += ['-fshort-wchar', '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                  '-I' + str(r / 'MU_BASECORE/MdePkg/Include/X64')]
    flags += ['-I' + str(c / 'Include'), '-DCONFIG_SWRM_VER_1P7']
    for d in ('soc', 'asoc/codecs/wcd938x', 'asoc/codecs/wsa883x'):
        flags += ['-isystem', str(upstream / d)]
    for s in sources:
        dep = subprocess.check_output(['clang', *flags, '-M', str(s)], text=True)
        inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n', '').split(':', 1)[1]))
    before = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    exe = o / ('soundwire-nt' if nt else 'soundwire-native')
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
    'status': 'PASS', 'scope': 'Actual SoundWire controller/OSKAL seam and source-qualified register model; no real MMIO/provider/playback claim',
    'runs': runs, 'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)}
}, indent=2) + '\n')
