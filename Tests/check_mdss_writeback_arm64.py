#!/usr/bin/env python3
"""Compile the actual writeback source for ARM64 EFI and the real ARM64 WDK.

This is an ABI/object probe. The permanent PEP project and DOD own the
operative provider/caller link; no native caller activation is implied.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--clang', required=True, type=Path)
p.add_argument('--wdk', required=True, type=Path)
p.add_argument('--sdk', required=True, type=Path)
p.add_argument('--trace-dir', required=True, type=Path)
a = p.parse_args()
r = Path(__file__).resolve().parents[3]; c = r/'Platforms/CranePkg'
o = a.output.resolve(); o.mkdir(parents=True, exist_ok=True)
(o/'artifacts.json').unlink(missing_ok=True)
kit = '10.0.26100.0'
dirs = [a.wdk.resolve()/'c'/f'Include/{kit}'/n for n in ('km', 'km/crt', 'shared')]
dirs += [a.sdk.resolve()/'c'/f'Include/{kit}'/'shared']
overlay = o/'case-overlay.json'
overlay.write_text(json.dumps({'version':0, 'case-sensitive':False, 'roots':[
    {'type':'directory', 'name':str(d), 'contents':[
        {'type':'file', 'name':f.name, 'external-contents':str(f)}
        for f in d.iterdir() if f.is_file()]} for d in dirs]}))
s = c/'Library/MdssDpuWritebackCrLib/MdssDpuWriteback.c'
records = []
for native in (True, False):
    flags = ['-ffreestanding', '-std=c17', '-Wall', '-Wextra', '-Werror',
             '-I'+str(c/'Include')]
    if native:
        flags += ['--target=aarch64-none-elf', '-fshort-wchar', '-DMDEPKG_NDEBUG',
                  '-I'+str(r/'MU_BASECORE/MdePkg/Include'),
                  '-I'+str(r/'MU_BASECORE/MdePkg/Include/AArch64')]
    else:
        flags += ['--target=aarch64-pc-windows-msvc', '-fms-extensions',
                  '-Wno-microsoft-anon-tag', '-D_ARM64_', '-D_KERNEL_MODE',
                  '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A00000A',
                  '-ivfsoverlay', str(overlay), '-I'+str(a.trace_dir.resolve())]
        for d in dirs: flags += ['-isystem', str(d)]
    obj = o/('writeback-efi.o' if native else 'writeback-nt.obj')
    cmd = [str(a.clang.resolve()), *flags, '-c', str(s), '-o', str(obj)]
    x = subprocess.run(cmd, text=True, capture_output=True)
    (o/(obj.name+'.log')).write_text(x.stdout+x.stderr)
    if x.returncode: print(x.stderr); x.check_returncode()
    records.append({'command':cmd, 'object':str(obj),
                    'sha256':hashlib.sha256(obj.read_bytes()).hexdigest()})
inputs = [s, Path(__file__), c/'Include/Library/MdssDpuWritebackCrLib.h',
          c/'Include/Library/MdssDpuInterruptCrLib.h']
(o/'artifacts.json').write_text(json.dumps({'status':'PASS',
    'scope':'actual ARM64 EFI + real ARM64 WDK objects only; no provider/link/activation or hardware proof',
    'objects':records,
    'inputs':{str(f.relative_to(r)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},
    'compiler':subprocess.check_output([str(a.clang.resolve()), '--version'], text=True)}, indent=2)+'\n')
print('Actual writeback ARM64 EFI + ARM64 WDK objects PASS')
