#!/usr/bin/env python3
"""Exercise actual shared display retirement and partial resume under ASan/UBSan."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--expect-before', action='store_true')
args = parser.parse_args()
pkg = Path(__file__).resolve().parents[1]
root = pkg.parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
sources = [pkg / name for name in (
    'Tests/MdssRetireTest.c', 'Library/MdssCrLib/MdssRetire.c',
    'Library/MdssCrLib/MdssHardware.c')]
inputs = sources + [Path(__file__), pkg / 'Include/Library/MdssRetireCrLib.h',
                   pkg / 'Include/Library/MdssCrLib.h']
before = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
commands = []
for nt in (False, True):
    exe = out / ('nt' if nt else 'native')
    flags = ['clang', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-I' + str(pkg / 'Include')]
    flags += (['-D_KERNEL_MODE', '-I' + str(pkg / 'Tests/KernelMocks')] if nt else
              ['-fshort-wchar', '-I' + str(root / 'MU_BASECORE/MdePkg/Include'),
               '-I' + str(root / 'MU_BASECORE/MdePkg/Include/X64')])
    cmd = flags + [str(p) for p in sources] + ['-o', str(exe)]
    commands.append(cmd)
    result = subprocess.run(cmd, text=True, capture_output=True)
    (out / (exe.name + '-compile.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stderr)
        result.check_returncode()
    result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=60)
    (out / (exe.name + '.log')).write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    if args.expect_before:
        if result.returncode == 0 or 'resume retry' not in result.stderr:
            raise RuntimeError('expected actual partial resume retry assertion')
    else:
        result.check_returncode()
after = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
if before != after:
    raise RuntimeError('inputs changed during verification')
(out / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'expected_before_failure': args.expect_before,
    'scope': __doc__, 'inputs': before, 'commands': commands}, indent=2) + '\n')
