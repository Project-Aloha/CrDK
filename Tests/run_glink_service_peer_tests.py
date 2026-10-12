#!/usr/bin/env python3
"""Independent actual GLINK service replay, native and NT with ASan/UBSan."""
import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--case', choices=['handshake', 'large', 'fault', 'close', 'backpressure', 'gpr', 'nonreuse', 'quiesce'])
a = p.parse_args()
c = Path(__file__).resolve().parents[1]
r = c.parents[1]
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
(o / 'artifacts.json').unlink(missing_ok=True)
(o / 'Glink.tmh').write_text('/* Host-only WPP trace substitute. */\n')
sources = [c / 'Tests/GlinkServicePeerTest.c', c / 'Library/UcsiCrLib/Glink.c',
           c / 'Library/AudioGprCrLib/AudioGpr.c']
inputs = {*sources, Path(__file__).resolve()}
runs = []
for nt in (False, True):
    flags = ['-std=c17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    if nt:
        flags += ['-D_KERNEL_MODE', '-pthread', '-I' + str(c / 'Tests/KernelMocks'), '-I' + str(o)]
    else:
        flags += ['-fshort-wchar', '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                  '-I' + str(r / 'MU_BASECORE/MdePkg/Include/X64')]
    flags += ['-I' + str(c / 'Include')]
    for source in sources:
        dep = subprocess.check_output(['clang', *flags, '-MM', str(source)], text=True)
        inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n', '').split(':', 1)[1]))
    before = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    if runs and any(before.get(path) != digest for path, digest in runs[0]['inputs'].items()):
        raise RuntimeError('Source changed between native/NT runs; rerun required')
    exe = o / ('glink-peer-nt' if nt else 'glink-peer-native')
    cmd = ['clang', *flags, *map(str, sources), '-o', str(exe)]
    compile_result = subprocess.run(cmd, text=True, capture_output=True)
    (o / (exe.name + '-compile.log')).write_text(compile_result.stdout + compile_result.stderr)
    (o / (exe.name + '-inputs.json')).write_text(json.dumps(before, indent=2) + '\n')
    if compile_result.returncode:
        print(compile_result.stdout + compile_result.stderr, end='')
        compile_result.check_returncode()
    x = subprocess.run([str(exe)] + ([a.case] if a.case else []), text=True, capture_output=True, timeout=30)
    log = o / (exe.name + '.log')
    log.write_text(x.stdout + x.stderr)
    unchanged = before == {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    run = {'nt': nt, 'case': a.case or 'all', 'command': cmd, 'output': x.stdout + x.stderr, 'returncode': x.returncode,
           'log': str(log), 'inputs_unchanged': unchanged, 'inputs': before,
           'binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
    runs.append(run)
    (o / 'attempts.json').write_text(json.dumps(runs, indent=2) + '\n')
    print(x.stdout + x.stderr, end='')
    x.check_returncode()
    if not unchanged:
        raise RuntimeError('Actual source changed during tests; evidence saved, rerun required')
(o / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': 'Synthetic peer wire replay against actual GLINK; no hardware claim',
    'runs': runs, 'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)}
}, indent=2) + '\n')
