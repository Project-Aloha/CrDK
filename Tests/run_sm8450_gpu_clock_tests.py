#!/usr/bin/env python3
"""Run actual GPU clock graph/core against a controlled MMIO model."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
receipt = out / 'artifacts.json'
receipt.unlink(missing_ok=True)
runs = []
for kernel in (False, True):
    name = 'nt' if kernel else 'native'
    exe = out / name
    cmd = ['clang', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
           '-I' + str(p / 'Include'), '-MMD', '-MF', str(out / (name + '.d'))]
    if kernel:
        cmd += ['-D_KERNEL_MODE', '-pthread', '-I' + str(p / 'Tests/KernelMocks')]
    else:
        cmd += ['-fshort-wchar', '-I' + str(p.parents[1] / 'MU_BASECORE/MdePkg/Include'),
                '-I' + str(p.parents[1] / 'MU_BASECORE/MdePkg/Include/X64')]
    cmd += [str(p / 'Tests/Sm8450GpuClockTest.c'), str(p / 'Library/ClockLib/fixed_pll.c'),
            '-o', str(exe)]
    with (out / (name + '.log')).open('w') as log:
        subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
        subprocess.run([str(exe)], check=True, timeout=60, stdout=log, stderr=subprocess.STDOUT)
    runs.append({'flavor': name, 'command': cmd,
                 'binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()})
inputs = [p / 'Tests/Sm8450GpuClockTest.c', p / 'Tests/ClockOwnershipTest.c',
          p / 'Include/Library/clock.h', p / 'Library/Sm8450ClockLib/Sm8450Clock.c',
          p / 'Library/Sm8450ClockLib/Sm8450GpuClock.h']
inputs += list((p / 'Library/ClockLib').glob('*.[ch]'))
receipt.write_text(json.dumps({'status': 'PASS', 'scope': 'actual GPU boot graph and clock core; controlled MMIO, no hardware power/clock proof',
    'runs': runs, 'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}}, indent=2) + '\n')
print('PASS: native and NT actual GPU clock graph; ' + str(receipt))
