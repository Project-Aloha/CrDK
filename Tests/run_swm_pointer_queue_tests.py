#!/usr/bin/env python3
"""Extract actual SWM queue functions and use actual headers under sanitizers."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import re
import resource
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--source', type=Path, help='Immutable old WindowManager.c for before proof')
parser.add_argument('--expect-before', action='store_true')
args = parser.parse_args()
crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
source = (args.source or crane/'Override/MU_PLUS/MsGraphicsPkg/SimpleWindowManagerDxe/WindowManager.c').resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
assert not (output/'artifacts.json').exists(), 'Use a new receipt directory'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
text = source.read_text()
selected = ('SignalClient', 'InsertPointerEventIntoQueue',
            'PeekAtAbsolutePointerEventInQueue', 'ExtractAbsolutePointerEventFromQueue')

def function(name):
    match = re.search(r'(?m)^(?:static )?(?:EFI_STATUS|VOID)\n' + re.escape(name) + r' \(\n', text)
    assert match, name
    body = text.index('{', match.end())
    position, depth = body + 1, 1
    while depth:
        depth += (text[position] == '{') - (text[position] == '}')
        position += 1
    return text[match.start():position]

extracted = output/'swm_pointer_queue_actual.inc'
extracted.write_text('\n\n'.join(function(name) for name in selected) + '\n')
test = crane/'Tests/SwmPointerQueueTest.c'
includes = [output, root/'MU_BASECORE/MdePkg/Include', root/'MU_BASECORE/MdePkg/Include/X64',
            root/'MU_BASECORE/MdeModulePkg/Include', root/'Common/MU/MsGraphicsPkg/Include',
            root/'Common/MU/MsCorePkg/Include']
flags = ['-std=c11', '-fshort-wchar', '-g', '-Wall', '-Wextra', '-Werror',
         '-Wno-unused-parameter', '-Wno-parentheses-equality',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
flags += ['-I' + str(path) for path in includes]
inputs = {str(path): sha(path) for path in (source, test, Path(__file__), extracted)}
commands, runs = [], []
cases = ('motion', 'edges', 'full', 'wrap', 'full-tail', 'read-between', 'notifications', 'isolation', 'basic')
for mode, options in (('debug', ['-O1']), ('release', ['-O2', '-DNDEBUG', '-DMDEPKG_NDEBUG'])):
    binary, dependency = output/('test-' + mode), output/(mode + '.d')
    command = ['clang', *flags, *options, '-MMD', '-MF', str(dependency), str(test), '-o', str(binary)]
    commands.append(command)
    result = subprocess.run(command, capture_output=True, text=True)
    (output/(mode + '-compile.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
    for path in dependency.read_text().replace('\\\n', ' ').split(':', 1)[1].split():
        inputs.setdefault(path, sha(path))
    for case in cases:
        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=20)
        log = output/(mode + '-' + case + '.log')
        log.write_text(result.stdout + result.stderr)
        expected_failure = args.expect_before and case not in ('basic', 'isolation')
        print(mode, case, 'exit', result.returncode, '(expected old failure)' if expected_failure else '')
        runs.append({'mode': mode, 'case': case, 'exit': result.returncode,
                     'expected_failure': expected_failure, 'log': str(log), 'log_sha256': sha(log),
                     'binary': str(binary), 'binary_sha256': sha(binary)})
        if expected_failure:
            assert result.returncode == -6 and 'SwmPointerQueueTest.c:' in result.stderr, (
                case, 'old source should violate a queue assertion, not fail setup')
        else:
            if result.returncode:
                print(result.stdout + result.stderr, end='')
            result.check_returncode()
for path, digest in inputs.items():
    assert sha(path) == digest, ('source drift', path)
archive = output/'inputs.tar.gz'
with tarfile.open(archive, 'w:gz') as tar:
    for path in sorted(inputs):
        tar.add(path, arcname=path.lstrip('/'), recursive=False)
receipt = {'status': 'EXPECTED_BEFORE_FAILURE' if args.expect_before else 'PASS',
           'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
           'source': str(source), 'functions': selected, 'inputs': inputs, 'commands': commands,
           'runs': runs, 'archive': str(archive), 'archive_sha256': sha(archive),
           'limits': ['Actual four functions extracted unchanged; actual SWM and UEFI headers.',
                      'Modeled boot service TPL/event boundary, no firmware/device execution.',
                      'Callbacks execute at real source call sites; no USB/xHCI or EBS claim.']}
(output/'artifacts.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('receipt', sha(output/'artifacts.json'))
