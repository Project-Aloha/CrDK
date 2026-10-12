#!/usr/bin/env python3
"""Actual pure SM8250 inventory/admission with source-derived Android route seed."""
from pathlib import Path
import argparse, hashlib, json, subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
pkg = root / 'Platforms/CranePkg'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
seed = root / '.tmp/waipio-continuation/hdk8245-panel/boot-observe/validation-v2/BootObserveReference.c'
receipt = root / '.tmp/waipio-continuation/hdk8245-panel/boot-observe/validation-v2/artifacts.json'
sources = [pkg / 'Tests/MdssBoot8250PreservedTest.c', seed,
           *[pkg / 'Library/MdssBoot8250CrLib' / x for x in ('Io.c', 'Observe.c', 'Retire.c')]]
frozen = {}
commands = []
def digest(q):
    return hashlib.sha256(q.read_bytes()).hexdigest()
def pin(q):
    q = q.resolve()
    d = digest(q)
    assert q not in frozen or frozen[q] == d, q
    frozen[q] = d
def run(cmd, log):
    commands.append(list(map(str, cmd)))
    r = subprocess.run(cmd, text=True, capture_output=True)
    (out / log).write_text(r.stdout + r.stderr)
    if r.returncode:
        print(r.stdout + r.stderr)
        r.check_returncode()
    return r
pin(receipt)
previous = json.loads(receipt.read_text())
assert previous['status'] == 'PASS'
for name, expected in previous['inputs'].items():
    q = Path(name)
    q = q if q.is_absolute() else root / q
    if q == seed:
        assert digest(q) == expected, q
for q in [*sources, Path(__file__)]:
    pin(q)
base_flags = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-Wno-sign-compare', '-Wno-misleading-indentation', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-I' + str(pkg / 'Include')]
for nt in (False, True):
    mode = 'nt' if nt else 'native'
    flags = [*base_flags]
    flags += ['-D_KERNEL_MODE', '-I' + str(pkg / 'Tests/KernelMocks')] if nt else [
        '-fshort-wchar', '-I' + str(root / 'MU_BASECORE/MdePkg/Include'),
        '-I' + str(root / 'MU_BASECORE/MdePkg/Include/X64')]
    objects = []
    for i, q in enumerate(sources):
        dep = out / f'{mode}-{i}.d'
        obj = out / f'{mode}-{i}.o'
        run([*flags, '-MM', '-MF', dep, q], f'{mode}-{i}-deps.log')
        for name in dep.read_text().split(':', 1)[1].replace('\\\n', '').split():
            pin(Path(name))
        extra = ['-fno-sanitize=shift-base'] if q == seed else []
        run([*flags, *extra, '-c', q, '-o', obj], f'{mode}-{i}-compile.log')
        objects.append(obj)
    exe = out / f'{mode}-preserved-route'
    run(['clang', '-fsanitize=address,undefined', *objects, '-o', exe], f'{mode}-link.log')
    r = run([exe], f'{mode}-run.log')
    print(mode + ': ' + r.stdout.strip())
for q, d in frozen.items():
    assert digest(q) == d, q
(out / 'artifacts.json').write_text(json.dumps({'status': 'PASS', 'scope': __doc__,
    'commands': commands, 'inputs': {str(q): d for q, d in sorted(frozen.items())}}, indent=2) + '\n')
