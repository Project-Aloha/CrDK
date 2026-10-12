#!/usr/bin/env python3
"""Compile actual Audio PCM/APM wire helpers for ARM64 EFI and real ARM64 WDK."""
import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[3])
p.add_argument('--clang', required=True, type=Path)
p.add_argument('--wdk', required=True, type=Path)
p.add_argument('--sdk', required=True, type=Path)
p.add_argument('--trace-dir', required=True, type=Path)
a = p.parse_args()
r = a.workspace.resolve()
c = Path(__file__).resolve().parents[1]
base = r / 'Platforms/CranePkg'
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
(o / 'artifacts.json').unlink(missing_ok=True)
kit = '10.0.26100.0'
dirs = [a.wdk.resolve() / 'c' / f'Include/{kit}' / n for n in ('km', 'km/crt', 'shared')]
dirs += [a.sdk.resolve() / 'c' / f'Include/{kit}/shared', a.wdk.resolve() / 'c/Include/wdf/kmdf/1.31']
overlay = o / 'case-overlay.json'
overlay.write_text(json.dumps({'version': 0, 'case-sensitive': False, 'roots': [
    {'type': 'directory', 'name': str(d), 'contents': [
        {'type': 'file', 'name': f.name, 'external-contents': str(f)}
        for f in d.iterdir() if f.is_file()]} for d in dirs]}))
sources = [c / 'Library/SoundWireCrLib/SoundWire.c', c / 'Library/SoundWireCrLib/Sm8450.c']
inputs = {*sources, Path(__file__).resolve(), c / 'Include/Library/SoundWireCrLib.h',
          c / 'Library/SoundWireCrLib/SoundWireCrLib.inf'}
records = []
for native in (True, False):
    flags = ['-ffreestanding', '-std=c17', '-Wall', '-Wextra', '-Werror',
             '-fstack-usage', '-I' + str(c / 'Include'), '-I' + str(base / 'Include')]
    if native:
        flags += ['--target=aarch64-none-elf', '-fshort-wchar', '-DMDEPKG_NDEBUG',
                  '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                  '-I' + str(r / 'MU_BASECORE/MdePkg/Include/AArch64')]
    else:
        flags += ['--target=aarch64-pc-windows-msvc', '-fms-extensions',
                  '-Wno-microsoft-anon-tag', '-D_ARM64_', '-D_KERNEL_MODE',
                  '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A00000A',
                  '-ivfsoverlay', str(overlay), '-I' + str(a.trace_dir.resolve())]
        for d in dirs:
            flags += ['-isystem', str(d)]
    for s in sources:
        dep = subprocess.check_output([str(a.clang.resolve()), *flags, '-M', str(s)], text=True)
        inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n', '').split(':', 1)[1]))
        before = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
        obj = o / (s.stem + ('-efi.o' if native else '-nt.obj'))
        cmd = [str(a.clang.resolve()), *flags, '-c', str(s), '-o', str(obj)]
        x = subprocess.run(cmd, text=True, capture_output=True)
        (o / (obj.name + '.log')).write_text(x.stdout + x.stderr)
        if x.returncode:
            print(x.stderr)
            x.check_returncode()
        if before != {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}:
            raise RuntimeError('Compiled input changed during object build')
        records.append({'native': native, 'command': cmd, 'object': str(obj),
                        'sha256': hashlib.sha256(obj.read_bytes()).hexdigest()})
(o / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': 'Actual SoundWire and target ARM64 EFI/WDK objects; actual OSKAL declarations, no physical resource/IRQ/playback proof',
    'objects': records, 'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)},
    'compiler': subprocess.check_output([str(a.clang.resolve()), '--version'], text=True)
}, indent=2) + '\n')
print('SoundWire actual ARM64 EFI + real ARM64 WDK objects PASS')
