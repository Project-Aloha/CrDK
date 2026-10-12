#!/usr/bin/env python3
"""Build actual ACDB reader with pinned real fixtures under native/NT sanitizers."""
import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

c = Path(__file__).resolve().parents[1]
r = c.parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--fixtures', type=Path, default=r / '.vscode/audioreach-conf')
a = p.parse_args()
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
(o / 'artifacts.json').unlink(missing_ok=True)
inventory = c / 'Tests/Fixtures/AcdbCr/inventory.json'
records = json.loads(inventory.read_text())
files = []
for f in records['fixtures']:
    path = a.fixtures.resolve() / f['path']
    if hashlib.sha256(path.read_bytes()).hexdigest() != f['sha256']:
        raise RuntimeError('Pinned public fixture changed: ' + str(path))
    files.append(path)

# The saved inventory predates the C parser. No values from production parsing
# are used to generate expected offsets/counts/codecs for these genuine files.
lines = ['typedef struct { UINT32 Bytes; CR_ACDB_INFO Info; '
         'const CR_ACDB_CHUNK *Chunks; const CR_ACDB_CODEC *Codecs; } FIXTURE;']
entries = []
for i, f in enumerate(records['fixtures']):
    chunks = f['chunks']
    h = f['head']
    head = next(x for x in chunks if x['name'] == 'HEAD')
    lines.append('static const CR_ACDB_CHUNK Chunks%d[] = {' % i)
    lines += ['{%dU,%dU,%dU},' % (x['id'], x['offset'], x['size']) for x in chunks]
    lines.append('};')
    lines.append('static const CR_ACDB_CODEC Codecs%d[] = {' % i)
    lines += ['{%dU,%dU,%dU},' % tuple(x) for x in h['codec_entries']]
    lines.append('};')
    oem_offset = head['offset'] + 20 + 12 * h['codecs']
    version = ','.join(map(str, h['file_version']))
    entries.append('{%dU,{%dU,{%s},%dU,%dU,%dU,{%dU,%dU},{%dU,%dU}},Chunks%d,Codecs%d},' % (
        f['size'], h['version'], version, h['codecs'], h['date'], len(chunks),
        head['offset'], head['size'], oem_offset, h['oem_length'], i, i))
lines += ['static const FIXTURE Fixtures[] = {', *entries, '};']
oracle = o / 'AcdbFixtures.h'
oracle.write_text('\n'.join(lines) + '\n')

sources = [c / 'Tests/AcdbCrTest.c', c / 'Library/AcdbCrLib/Acdb.c']
inputs = {*sources, inventory, oracle, *files, Path(__file__).resolve(),
          c / 'Include/Library/AcdbCrLib.h', c / 'Library/AcdbCrLib/AcdbCrLib.inf',
          c / 'Library/AcdbCrLib/LICENSE', c / 'Tests/Fixtures/AcdbCr/LICENSE'}
runs = []
for nt in (False, True):
    flags = ['-std=c17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-I' + str(o), '-I' + str(c / 'Include')]
    if nt:
        flags += ['-D_KERNEL_MODE', '-I' + str(c / 'Tests/KernelMocks')]
    else:
        flags += ['-fshort-wchar', '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                  '-I' + str(r / 'MU_BASECORE/MdePkg/Include/X64')]
    for source in sources:
        dep = subprocess.check_output(['clang', *flags, '-MM', str(source)], text=True)
        inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n', '').split(':', 1)[1]))
    before = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    exe = o / ('acdb-nt' if nt else 'acdb-native')
    cmd = ['clang', *flags, *map(str, sources), '-o', str(exe)]
    build = subprocess.run(cmd, text=True, capture_output=True)
    (o / (exe.name + '-build.log')).write_text(build.stdout + build.stderr)
    if build.returncode:
        print(build.stdout + build.stderr)
        build.check_returncode()
    run = subprocess.run([str(exe), *map(str, files)], text=True, capture_output=True)
    log = o / (exe.name + '.log')
    log.write_text(run.stdout + run.stderr)
    print(run.stdout + run.stderr, end='')
    run.check_returncode()
    if before != {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}:
        raise RuntimeError('Input changed during actual-source test')
    runs.append({'nt': nt, 'command': cmd, 'log': str(log), 'output': run.stdout,
                 'binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()})
(o / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': 'Actual ACDB framing/HEAD only, no graph/calibration/DSP IO',
    'fixtures_repository': records['repository'], 'fixtures_commit': records['commit'],
    'runs': runs, 'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)}
}, indent=2) + '\n')
