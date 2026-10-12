#!/usr/bin/env python3
"""Actual automatic EFI block adapter, modeled UFS/SD protocol ownership and faults."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
includes = [crane/'Include', root/'MU_BASECORE/MdePkg/Include',
            root/'MU_BASECORE/MdePkg/Include/X64']
sources = [crane/'Tests/UsbMscBlockAdapterTest.c']
hashfile = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()

def run(directory):
    directory.mkdir(parents=True, exist_ok=True)
    inputs = {str(Path(__file__).resolve()): hashfile(__file__)}
    results = []
    for mode, extra in [('debug', []), ('release', ['-DMDEPKG_NDEBUG'])]:
        output = directory/mode
        command = ['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall',
                   '-Wextra', '-Werror', '-fsanitize=address,undefined',
                   '-fno-sanitize-recover=all', *extra,
                   *['-I'+str(p) for p in includes], *map(str, sources), '-o', str(output)]
        dependency_command = command[:command.index('-o')] + ['-MM']
        dependencies = subprocess.run(dependency_command, check=True, text=True, capture_output=True).stdout
        for line in dependencies.replace('\\\n',' ').splitlines():
            for name in line.split(':', 1)[1].split():
                p = Path(name).resolve()
                digest = hashfile(p)
                assert str(p) not in inputs or inputs[str(p)] == digest, 'input changed between modes'
                inputs[str(p)] = digest
        before = {p: hashfile(p) for p in inputs}
        subprocess.run(command, check=True)
        test = subprocess.run([str(output)], text=True, capture_output=True)
        log = directory/(mode+'.log');log.write_text(test.stdout+test.stderr)
        assert all(hashfile(p)==h for p,h in before.items()), 'input changed during test'
        results.append({'mode':mode,'command':command,'returncode':test.returncode,
                        'executable_sha256':hashfile(output),'log_sha256':hashfile(log)})
        print(test.stdout, end='');print(test.stderr, end='')
        if test.returncode:
            (directory/'failure.json').write_text(json.dumps({'inputs':inputs,'runs':results},indent=2)+'\n')
            raise SystemExit(test.returncode)
    (directory/'artifacts.json').write_text(json.dumps({'scope':__doc__,'inputs':inputs,'runs':results,'result':'PASS'},indent=2)+'\n')
    print('Receipt:', directory/'artifacts.json', hashfile(directory/'artifacts.json'))

if args.output:
    run(args.output.resolve())
else:
    with tempfile.TemporaryDirectory(prefix='crane-msc-block-adapter-') as temporary:
        run(Path(temporary))
