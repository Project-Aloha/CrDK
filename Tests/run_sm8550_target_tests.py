#!/usr/bin/env python3
"""Exercise the SM8550 provider and shared cores, without a selected board.

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

crane = Path(__file__).resolve().parents[1]
workspace = crane.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--cc', default='clang')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'artifacts.json').unlink(missing_ok=True)
target = crane / 'Target/Sm8550'
target_names = ['CrTarget.c', 'CrGpioTarget.c', 'CrPdcTarget.c',
                'CrRpmhTarget.c', 'CrDebugUartTarget.c']
assert all((target / name).is_file() for name in target_names), \
    'SM8550 production target resource provider is missing'
sources = [target / name for name in target_names] + [
    crane / 'Library/CrDalRegistryLib/CrDalRegistryLib.c',
    crane / 'Library/GpioLib/gpio.c', crane / 'Library/PdcLib/pdc.c',
    crane / 'Library/RpmhLib/rpmh.c', crane / 'Tests/Sm8550TargetTest.c']
overlay = out / 'host-overlay/oskal'
overlay.mkdir(parents=True, exist_ok=True)
for name in ('cr_memory.h', 'cr_time.h'):
    (overlay / name).unlink(missing_ok=True)
    (overlay / name).symlink_to(crane / 'Tests/Mocks/oskal' / name)
common = [args.cc, '-std=gnu11', '-fshort-wchar', '-g', '-O1', '-Wall',
          '-Wextra', '-Werror', '-fsanitize=address,undefined',
          '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
          '-include', 'Uefi.h',
          '-I' + str(overlay.parent), '-I' + str(crane / 'Include'),
          '-I' + str(target), '-I' + str(workspace / 'MU_BASECORE/MdePkg/Include'),
          '-I' + str(workspace / 'MU_BASECORE/MdePkg/Include/X64')]
inputs = {Path(__file__).resolve(), target / 'CrTargetLib.inf'}
runs = []
for base in (0, 0x00a9c000, 0x0098c000):
    # PCD values are build constants, matching a future board's explicit choice.
    flags = common + ['-D_PCD_VALUE_PcdUartSerialBase=' + hex(base)]
    for source in sources:
        dep = subprocess.check_output([*flags, '-MM', str(source)], text=True)
        inputs.update(Path(p).resolve() for p in shlex.split(
            dep.replace('\\\n', '').split(':', 1)[1]))
    before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    exe = out / ('sm8550-' + hex(base))
    command = [*flags, '-Wl,--gc-sections', *map(str, sources), '-o', str(exe)]
    built = subprocess.run(command, capture_output=True, text=True)
    (out / (exe.name + '.compile.log')).write_text(built.stdout + built.stderr)
    if built.returncode:
        print(built.stderr)
        built.check_returncode()
    tested = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    (out / (exe.name + '.log')).write_text(tested.stdout + tested.stderr)
    print(tested.stdout + tested.stderr, end='')
    tested.check_returncode()
    if before != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}:
        raise RuntimeError('Compiled input changed during host test')
    runs.append({'uart_base': base, 'command': command, 'returncode': tested.returncode,
                 'binary': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest()})
(out / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': 'Real source host ASan/UBSan; simulated MMIO, no board proof',
    'compiler': subprocess.check_output([args.cc, '--version'], text=True),
    'runs': runs, 'inputs': before}, indent=2) + '\n')
