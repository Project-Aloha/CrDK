#!/usr/bin/env python3
"""Compile actual AudioCal core/consumer and compare genuine upstream oracles."""
import argparse
import hashlib
import json
import shlex
import subprocess
import sys
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
c = Path(__file__).resolve().parents[1]
r = c.parents[1]
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
(o / 'artifacts.json').unlink(missing_ok=True)
source = r / '.vscode/audioreach-graphservices'
fixtures = r / '.vscode/audioreach-conf'
inventory = c / 'Tests/Fixtures/AcdbCr/inventory.json'
vec = o / 'vectors'
cmd = [sys.executable, str(c / 'Tests/AudioCal/generate_vectors.py'),
       '--source', str(source), '--fixtures', str(fixtures),
       '--inventory', str(inventory), '--output', str(vec)]
result = subprocess.run(cmd, capture_output=True, text=True)
(o / 'generate-vectors.log').write_text(result.stdout + result.stderr)
if result.returncode:
    print(result.stdout + result.stderr)
    result.check_returncode()
print(result.stdout, end='')
core = [c / 'Library/AudioCalCrLib/AudioCal.c', c / 'Library/AcdbCrLib/Acdb.c']
test_sources = [c / 'Tests/AudioCalCrTest.c', *core, c / 'Library/AudioGprCrLib/AudioGpr.c']
consumer_sources = [c / 'Tools/AudioCalPlan/AudioCalPlan.c', *core]
inputs = {*test_sources, *consumer_sources, inventory, Path(__file__).resolve(),
          c / 'Tests/AudioCal/upstream_oracle.py', c / 'Tests/AudioCal/generate_vectors.py',
          c / 'Library/AudioCalCrLib/AudioCalCrLib.inf',
          c / 'Library/AudioCalCrLib/AudioCalCrLib.props',
          c / 'Library/AudioCalCrLib/README.md', c / 'Library/AudioCalCrLib/LICENSE',
          c / 'Tools/AudioCalPlan/README.md', c / 'Tools/AudioCalPlan/qcm6490-headphones-test.profile'}
profiles = json.loads((vec / 'oracle-results.json').read_text())
for item in profiles:
    path = fixtures / item['fixture']
    assert hashlib.sha256(path.read_bytes()).hexdigest() == item['fixture_sha256']
    inputs.add(path)
    inputs.add(Path(item['vectors']))
    assert hashlib.sha256(Path(item['vectors']).read_bytes()).hexdigest() == item['vectors_sha256']
inputs.update([vec / 'oracle-results.json', vec / 'upstream/build.json'])
hash_files = lambda: {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)}
runs = []
initial = hash_files()
for nt in (False, True):
    flags = ['-std=c17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    if nt:
        flags += ['-D_KERNEL_MODE', '-I' + str(c / 'Tests/KernelMocks')]
    else:
        flags += ['-fshort-wchar', '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                  '-I' + str(r / 'MU_BASECORE/MdePkg/Include/X64')]
    flags += ['-I' + str(c / 'Include')]
    for s in {*test_sources, *consumer_sources}:
        dep = subprocess.check_output(['clang', *flags, '-MM', str(s)], text=True)
        inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n', '').split(':', 1)[1]))
    before = hash_files()
    assert all(before[k] == v for k, v in initial.items()), 'Inputs changed between modes'
    suffix = 'nt' if nt else 'native'
    exe = o / ('audio-cal-' + suffix)
    cmd = ['clang', *flags, *map(str, test_sources), '-o', str(exe)]
    subprocess.run(cmd, check=True)
    run = [str(exe), *[str(x) for x in sorted(vec.glob('fixture-*.vectors'))]]
    result = subprocess.run(run, text=True, capture_output=True)
    (o / (exe.name + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
    print(suffix + ': ' + result.stdout.splitlines()[-1])
    consumer = o / ('AudioCalPlan-' + suffix)
    consumer_cmd = ['clang', *flags, *map(str, consumer_sources), '-o', str(consumer)]
    subprocess.run(consumer_cmd, check=True)
    diagnostic = o / ('diagnostic-' + suffix)
    diagnostic.mkdir(exist_ok=True)
    run_consumer = [str(consumer), str(fixtures / 'qcom/qli/qcm6490/acdbdata/acdb_cal.acdb'),
                    str(c / 'Tools/AudioCalPlan/qcm6490-headphones-test.profile'), str(diagnostic)]
    consumed = subprocess.run(run_consumer, capture_output=True, text=True, check=True)
    manifest = json.loads(consumed.stdout)
    (diagnostic / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    assert manifest['result'] == 'ok' and manifest['endpoint_instance'] == 0x401e
    expected = next(x for x in profiles if '/qcm6490/' in x['fixture'])['cases'][-16]
    calibration = (diagnostic / 'initial-calibration.gpr').read_bytes()[40:]
    assert hashlib.sha256(calibration).hexdigest() == expected['calibration']['sha256']
    body = (diagnostic / 'graph-open.gpr').read_bytes()[40:]
    at = expected['spf']['bytes']
    assert hashlib.sha256(body[:at]).hexdigest() == expected['spf']['sha256']
    assert hashlib.sha256(body[at:]).hexdigest() == expected['connections']['sha256']
    # Failed profile produces a typed reason and no command files in a fresh
    # output directory; malformed profile text is rejected by the consumer.
    failed = o / ('rejected-' + suffix)
    failed.mkdir(exist_ok=True)
    bad = o / ('missing-ckv-' + suffix + '.profile')
    bad.write_text((c / 'Tools/AudioCalPlan/qcm6490-headphones-test.profile').read_text().replace(
        'ckv 0xa4000000 0', 'ckv 0xa4000000 99'))
    rejected = subprocess.run([str(consumer), run_consumer[1], str(bad), str(failed)], capture_output=True, text=True)
    assert rejected.returncode == 1 and json.loads(rejected.stdout)['result'] == 'missing-calibration-key'
    assert not list(failed.iterdir())
    assert before == hash_files(), 'Inputs changed during compiled test run'
    runs.append({'nt': nt, 'compile': cmd, 'run': run, 'output': result.stdout,
                 'consumer_compile': consumer_cmd, 'consumer_run': run_consumer,
                 'binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                 'consumer_sha256': hashlib.sha256(consumer.read_bytes()).hexdigest(),
                 'diagnostic': {str(x): hashlib.sha256(x.read_bytes()).hexdigest()
                                for x in sorted(diagnostic.iterdir())}})
    initial = before
(o / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': 'Offline initial calibration compiler/consumer; no transport or hardware proof',
    'oracle_build': str(vec / 'upstream/build.json'), 'case_count': sum(len(x['cases']) for x in profiles),
    'runs': runs, 'inputs': hash_files(),
}, indent=2) + '\n')
