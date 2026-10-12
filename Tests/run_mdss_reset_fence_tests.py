#!/usr/bin/env python3
"""Run production reset/MCU admission logic with host-only boundary models.

Not an EFI ABI, firmware-link, electrical timing, RPMh composition, or board test.
The panel source/header is compiled whole. LT9611 and GPIO checked-setter bodies
are extracted verbatim; transport, MMIO and boot services are modeled.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def extract_function(source: str, name: str) -> str:
    """Keep original bytes; ignore braces in comments/literals when locating a body."""
    tokens = r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    masked = re.sub(tokens, lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source, flags=re.S)
    pattern = rf'(?m)^(?:(?:STATIC|EFI_STATUS|CR_STATUS|VOID|EFIAPI)\s+)+{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{'
    matches = list(re.finditer(pattern, masked))
    if len(matches) != 1:
        raise ValueError(f'{name}: expected one production definition, found {len(matches)}')
    match = matches[0]
    depth = 1
    end = match.end()
    while depth and end < len(masked):
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    if depth:
        raise ValueError(f'{name}: unterminated function')
    return source[match.start():end] + '\n'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--source-root', type=Path, help='CrDK root; defaults to parent of Tests')
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    root = (args.source_root or tests.parent).resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which(args.cc)
    if not compiler:
        parser.error(f'compiler not found: {args.cc}')
    paths = {
        'bridge': root/'Driver/Lt9611uxcCrDxe/lt9611.c',
        'gpio': root/'Library/GpioLib/gpio.c',
        'lease': root/'Library/GpioPinLeaseLib/GpioPinLease.c',
        'panel': root/'Driver/MDSSCrDxe/MdpPanelPower.c',
        'panel-header': root/'Driver/MDSSCrDxe/MdpPanelPower.h',
    }
    inputs = list(paths.values()) + [Path(__file__).resolve(), tests/'MdssResetFenceHost.h',
              tests/'MdpPanelPowerTest.c', tests/'MdssResetFenceGpioTest.c', tests/'Lt9611ResetGateTest.c']
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    frozen = {str(p): sha(p) for p in inputs}
    bridge = paths['bridge'].read_text()
    selected = {name: extract_function(bridge, name) for name in
                ('Lt9611TestI2cReadyToBoot', 'Lt9611ConfigureVideo', 'Lt9611IrqWorker')}
    (out/'BridgeCallbacks.inc').write_text('\n'.join(selected.values()))
    selected['GpioApplyConfigChecked'] = extract_function(paths['gpio'].read_text(), 'GpioApplyConfigChecked')
    selected['CrGpioPinLeaseSetValue'] = extract_function(paths['lease'].read_text(), 'CrGpioPinLeaseSetValue')
    (out/'GpioChecked.inc').write_text(selected['GpioApplyConfigChecked']+'\n'+selected['CrGpioPinLeaseSetValue'])
    # Isolated fake dependency headers; never placed in production Include/.
    for name in ('Protocol/EFIRpmhCrProtocol.h', 'Protocol/EFIGpioCrProtocol.h', 'Library/BaseMemoryLib.h'):
        target = out/name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#include "MdssResetFenceHost.h"\n')
    flags = [compiler, '-std=c17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-I'+str(out), '-I'+str(tests)]
    cases = [
        ('panel', [paths['panel'], tests/'MdpPanelPowerTest.c']),
        ('gpio-checked', [tests/'MdssResetFenceGpioTest.c']),
        ('lt9611-gates', [tests/'Lt9611ResetGateTest.c']),
    ]
    records = []
    failed = False
    for name, sources in cases:
        command = [*flags, *map(str, sources), '-o', str(out/name)]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (out/f'{name}-compile.log').write_text(compiled.stdout+compiled.stderr)
        record = {'case': name, 'command': command, 'compile_exit': compiled.returncode}
        if compiled.returncode:
            failed = True
            print(compiled.stdout+compiled.stderr, file=sys.stderr)
        else:
            run = subprocess.run([str(out/name)], capture_output=True, text=True, timeout=30)
            (out/f'{name}.log').write_text(run.stdout+run.stderr)
            record['test_exit'] = run.returncode
            record['executable_sha256'] = sha(out/name)
            failed |= run.returncode != 0
            print(f'[{name}] exit={run.returncode}\n{run.stdout}{run.stderr}', end='')
        records.append(record)
    if frozen != {str(p): sha(p) for p in inputs}:
        raise RuntimeError('source changed during tests')
    (out/'artifacts.json').write_text(json.dumps({
        'status': 'FAIL' if failed else 'PASS', 'scope': __doc__, 'cases': records,
        'inputs': frozen, 'selected_function_sha256': {n: hashlib.sha256(s.encode()).hexdigest() for n,s in selected.items()},
    }, indent=2)+'\n')
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
