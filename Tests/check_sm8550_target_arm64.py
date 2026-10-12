#!/usr/bin/env python3
"""Compile SM8550 target and shared cores as ARM64 DEBUG COFF objects.

No platform build, board image, linker/package or hardware claim is implied.
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
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', default='clang')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'artifacts.json').unlink(missing_ok=True)
target = crane / 'Target/Sm8550'
sources = [target / name for name in ('CrTarget.c', 'CrGpioTarget.c',
    'CrPdcTarget.c', 'CrRpmhTarget.c', 'CrDebugUartTarget.c')]
sources += [crane / relative for relative in (
    'Library/CrDalRegistryLib/CrDalRegistryLib.c', 'Library/GpioLib/gpio.c',
    'Library/PdcLib/pdc.c', 'Library/RpmhLib/rpmh.c',
    'Library/DebugUartLib/debug_uart.c')]
flags = [args.cc, '--target=aarch64-pc-windows-msvc', '-ffreestanding',
    '-std=gnu11', '-fshort-wchar', '-fms-extensions', '-g', '-O0', '-D_DEBUG=1',
    '-Wall', '-Wextra', '-Werror', '-Wno-microsoft-anon-tag', '-include', 'Uefi.h',
    '-D_PCD_VALUE_PcdUartSerialBase=0', '-I' + str(crane / 'Include'),
    '-I' + str(workspace / 'MU_BASECORE/MdePkg/Include'),
    '-I' + str(workspace / 'MU_BASECORE/MdePkg/Include/AArch64')]
inputs = {Path(__file__).resolve(), target / 'CrTargetLib.inf'}
for source in sources:
    dep = subprocess.check_output([*flags, '-MM', str(source)], text=True)
    inputs.update(Path(p).resolve() for p in shlex.split(
        dep.replace('\\\n', '').split(':', 1)[1]))
before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
objects = []
for source in sources:
    obj = out / (source.stem + '.obj')
    command = [*flags, '-c', str(source), '-o', str(obj)]
    built = subprocess.run(command, capture_output=True, text=True)
    (out / (obj.name + '.log')).write_text(built.stdout + built.stderr)
    if built.returncode:
        print(built.stderr)
        built.check_returncode()
    # IMAGE_FILE_MACHINE_ARM64 is 0xaa64 in the COFF header.
    assert obj.read_bytes()[:2] == bytes((0x64, 0xaa)), 'Expected ARM64 COFF object'
    objects.append({'source': str(source), 'object': str(obj), 'command': command,
                    'sha256': hashlib.sha256(obj.read_bytes()).hexdigest()})
after = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
if before != after:
    raise RuntimeError('Compiled input changed during ARM64 object check')
(out / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'target': 'DEBUG',
    'scope': 'Actual ARM64 COFF target and shared-core objects; no link, package or board proof',
    'compiler': subprocess.check_output([args.cc, '--version'], text=True),
    'objects': objects, 'inputs': before}, indent=2) + '\n')
print(f'SM8550 ARM64 DEBUG: {len(objects)} actual target/shared-core COFF objects PASS')
