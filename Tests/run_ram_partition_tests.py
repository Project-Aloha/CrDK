#!/usr/bin/env python3
"""Compile actual portable RAM parser under native/NT ASan+UBSan."""
import argparse, hashlib, json, subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
c = Path(__file__).resolve().parents[1]
edk = c.parents[1] / 'MU_BASECORE'
sources = [c/'Tests/RamPartitionCrTest.c', c/'Library/RamPartitionCrLib/RamPartition.c']
runs = []
for nt in (False, True):
    exe = a.output / ('ram-partition-nt' if nt else 'ram-partition-native')
    cmd = ['clang', '-std=c17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    if nt:
        cmd += ['-D_KERNEL_MODE', '-I'+str(c/'Tests/KernelMocks')]
    else:
        cmd += ['-fshort-wchar', '-I'+str(edk/'MdePkg/Include'),
                '-I'+str(edk/'MdePkg/Include/X64')]
    cmd += ['-I'+str(c/'Include'), *map(str,sources), '-o', str(exe)]
    subprocess.run(cmd, check=True)
    r = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    print(r.stdout, end='')
    runs.append({'nt': nt, 'command': cmd, 'output': r.stdout,
                 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest()})
inputs = [*sources, c/'Include/Library/RamPartitionCrLib.h',
          c/'Library/RamPartitionCrLib/RamPartitionCrLib.inf', Path(__file__)]
(a.output/'artifacts.json').write_text(json.dumps({
    'runs': runs, 'inputs': {str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
}, indent=2)+'\n')
