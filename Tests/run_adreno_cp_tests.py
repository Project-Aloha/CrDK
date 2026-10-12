#!/usr/bin/env python3
"""Exercise the retained CP core in EFI and NT host builds.

The fixture models register, DMA and secure endpoints. A passing result does
not establish a physical GPU route, firmware authentication or rendering.
"""
import argparse
import hashlib
import json
import shlex
import subprocess
import tarfile
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    canonical = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--core-root', type=Path, default=canonical)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / 'artifacts.json').exists():
        parser.error('Use a fresh output directory to preserve earlier results')
    core = args.core_root.resolve()
    firmware = canonical.parents[1]
    sources = [Path(__file__).with_name('AdrenoCpTest.c')]
    sources += [core / 'Library/AdrenoCpCrLib' / name
                for name in ('AdrenoCp.c', 'Sm8450Cp.c')]
    inputs = {Path(__file__).resolve()}
    commands = []
    for kernel in (False, True):
        for release in (False, True):
            name = ('nt' if kernel else 'efi') + ('-release' if release else '-debug')
            flags = ['-std=c17', '-g', '-O2' if release else '-O1',
                     '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                     '-fno-sanitize-recover=all', '-I' + str(core / 'Include')]
            if release:
                flags += ['-DNDEBUG', '-DMDEPKG_NDEBUG']
            if kernel:
                flags += ['-D_KERNEL_MODE', '-I' + str(canonical / 'Tests/KernelMocks')]
            else:
                flags += ['-fshort-wchar',
                          '-I' + str(firmware / 'MU_BASECORE/MdePkg/Include'),
                          '-I' + str(firmware / 'MU_BASECORE/MdePkg/Include/X64')]
            for source in sources:
                dependency = subprocess.check_output(
                    ['clang', *flags, '-M', str(source)], text=True)
                names = shlex.split(dependency.replace('\\\n', ' ').split(':', 1)[1])
                inputs.update(Path(name).resolve() for name in names)
            commands.append((name, ['clang', *flags, *map(str, sources),
                                    '-o', str(output / name)]))
    before = {str(path): digest(path) for path in sorted(inputs)}
    runs = []
    for name, command in commands:
        result = subprocess.run(command, text=True, capture_output=True)
        (output / (name + '-build.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        result = subprocess.run([str(output / name)], text=True, capture_output=True)
        log = output / (name + '.log')
        log.write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='')
        result.check_returncode()
        runs.append({'name': name, 'command': command,
                     'binary_sha256': digest(output / name),
                     'log_sha256': digest(log)})
    assert before == {name: digest(Path(name)) for name in before}, 'input drift'
    archive = output / 'sources.tar.gz'
    with tarfile.open(archive, 'w:gz') as bundle:
        for name in before:
            bundle.add(name, arcname=name.lstrip('/'), recursive=False)
    receipt = {'status': 'PASS', 'inputs': before, 'runs': runs,
               'archive': str(archive), 'archive_sha256': digest(archive),
               'limits': 'Portable engine with modeled MMIO/DMA/secure endpoints; '
                         'no physical execution or rendering qualification.'}
    (output / 'artifacts.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
