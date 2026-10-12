#!/usr/bin/env python3
"""Build the actual portable DP AUX source with EFI and ARM64 WDK headers.

The WDK no-entry kernel-import link validates the portable API symbols only.
It is not a PEP/DOD frontend link, driver registration or hardware test.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--clang', type=Path, required=True)
parser.add_argument('--wdk', type=Path, required=True)
parser.add_argument('--sdk', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
pkg = root/'Platforms/CranePkg'
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out/'artifacts.json').unlink(missing_ok=True)
compiler = args.clang.resolve()
source = pkg/'Library/MdssDpCrLib/DpAux.c'
kit = '10.0.26100.0'
headers = [args.wdk.resolve()/'c'/f'Include/{kit}'/name for name in ('km', 'km/crt', 'shared')]
headers += [args.sdk.resolve()/'c'/f'Include/{kit}'/name for name in ('shared', 'um')]
overlay = out/'case-overlay.json'
trace = out/'trace.h'
trace.write_text('#pragma once\n#include <ntddk.h>\n'
                 'static inline VOID CrDpTrace(CONST CHAR *F,...){(void)F;}\n'
                 '#define log_raw(...) CrDpTrace(__VA_ARGS__)\n'
                 '#define log_info(...) CrDpTrace(__VA_ARGS__)\n'
                 '#define log_warn(...) CrDpTrace(__VA_ARGS__)\n'
                 '#define log_err(...) CrDpTrace(__VA_ARGS__)\n')
overlay.write_text(json.dumps({'version': 0, 'case-sensitive': False, 'roots': [
    {'type': 'directory', 'name': str(directory), 'contents': [
        {'type': 'file', 'name': path.name, 'external-contents': str(path)}
        for path in sorted(directory.iterdir()) if path.is_file()]}
    for directory in headers]}))
flags = ['-ffreestanding', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I'+str(pkg/'Include'), '-I'+str(out)]
efi = ['--target=aarch64-none-elf', *flags, '-fshort-wchar', '-DMDEPKG_NDEBUG']
for include in (root/'MU_BASECORE/MdePkg/Include', root/'MU_BASECORE/MdePkg/Include/AArch64'):
    efi += ['-I'+str(include)]
nt = ['--target=aarch64-pc-windows-msvc', *flags, '-fms-extensions', '-Wno-microsoft-anon-tag',
      '-D_ARM64_', '-D_KERNEL_MODE', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A00000A',
      '-ivfsoverlay', str(overlay)]
for directory in headers:
    nt += ['-isystem', str(directory)]
inputs = {Path(__file__).resolve(), compiler, source, overlay, trace,
          pkg/'Include/Library/MdssDpCrLib.h',
          pkg/'Library/MdssDpCrLib/MdssDpCrLib.inf', pkg/'Library/MdssDpCrLib/MdssDpCrLib.props'}
runs = []
for name, mode in (('efi', efi), ('nt', nt)):
    dep = out/f'{name}.d'
    command = [str(compiler), *mode, '-M', '-MT', name, '-MF', str(dep), str(source)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        print(result.stderr)
    result.check_returncode()
    inputs.update(Path(x).resolve() for x in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]))
    obj = out/f'DpAux-{name}.obj'
    command = [str(compiler), *mode, '-c', str(source), '-o', str(obj)]
    result = subprocess.run(command, capture_output=True, text=True)
    (out/f'{name}.log').write_text(result.stdout+result.stderr)
    if result.returncode:
        print(result.stderr)
    result.check_returncode()
    runs.append({'command': command, 'status': 'PASS'})
library = args.wdk.resolve()/'c'/f'Lib/{kit}/km/ARM64/ntoskrnl.lib'
linker = compiler.parent/'lld-link'
if not linker.exists():
    linker = compiler.parent/'lld-link-23'
inputs.update([library, linker.resolve()])
exports = ['Init', 'Enable', 'Transfer', 'ReadEdid', 'ServiceIrq', 'Stop', 'Uninit']
command = [str(linker), '/machine:arm64', '/driver', '/dll', '/noentry', '/nodefaultlib',
           '/out:'+str(out/'MdssDpApi.sys'), str(out/'DpAux-nt.obj'), str(library),
           *['/export:CrMdssDpAux'+name for name in exports]]
result = subprocess.run(command, capture_output=True, text=True)
(out/'link.log').write_text(result.stdout+result.stderr)
if result.returncode:
    print(result.stderr)
result.check_returncode()
runs.append({'command': command, 'status': 'PASS'})
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
(out/'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': __doc__, 'runs': runs,
    'compiler': subprocess.check_output([str(compiler), '--version'], text=True),
    'inputs': {str(x): sha(x) for x in sorted(inputs)},
    'outputs': {str(x): sha(x) for x in sorted(out.iterdir()) if x.is_file() and x.name != 'artifacts.json'},
}, indent=2)+'\n')
print('DP AUX actual EFI ARM64 object + WDK object/kernel-import API link PASS')
