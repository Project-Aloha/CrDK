#!/usr/bin/env python3
"""Compile actual portable SDHCI command timing in native and NT type modes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
cr = Path(__file__).resolve().parents[1]
root = cr.parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
sources = [cr/'Tests/SdhciCommandTimingTest.c', cr/'Library/SdhciMsmLib/SdhciMsm.c']
runs = []
for nt in (False, True):
    exe = out/('nt' if nt else 'native')
    cmd = ['clang', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I'+str(cr/'Include')]
    if nt:
        cmd += ['-D_KERNEL_MODE', '-I'+str(cr/'Tests/KernelMocks')]
    else:
        cmd += ['-fshort-wchar', '-I'+str(root/'MU_BASECORE/MdePkg/Include'),
                '-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    cmd += [*map(str, sources), '-o', str(exe)]
    subprocess.run(cmd, check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(r.stdout+r.stderr, end='')
    r.check_returncode()
    runs.append({'mode': 'NT' if nt else 'native', 'command': cmd, 'output': r.stdout})
inputs = sources+[Path(__file__), cr/'Include/Library/SdhciMsmLib.h']
(out/'artifacts.json').write_text(json.dumps({'scope': 'actual portable core, modeled MMIO; no physical card test',
    'sources': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},
    'runs': runs}, indent=2)+'\n')
