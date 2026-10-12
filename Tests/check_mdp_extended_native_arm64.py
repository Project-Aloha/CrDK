#!/usr/bin/env python3
"""Compile the operative native extended provider, XML converter and GOP owner."""
from pathlib import Path
import argparse, hashlib, json, subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--clang', default='/home/sunf/droidvm-repos/out/artifacts/dxvk/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
pkg = root / 'Platforms/CranePkg'
sources = [pkg / f'Driver/MDSSCrDxe/{name}.c' for name in
           ('MdpExtendedPanel', 'MdpExtendedNative', 'MdpNativeResources', 'MdpExtendedOutput', 'MdpExtendedEntry', 'MdpHdkExtendedProfile', 'MdpSelectedPanel')]
sources += [pkg / 'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.c']
flags = ['--target=aarch64-none-elf', '-ffreestanding', '-std=c17', '-fshort-wchar',
         '-DMDEPKG_NDEBUG', '-Wall', '-Wextra', '-Werror']
for include in (pkg / 'Include', root / 'Platforms/MUCommonCrPkg/Include',
                root / 'MU_BASECORE/MdePkg/Include', root / 'MU_BASECORE/MdePkg/Include/AArch64',
                root / 'MU_BASECORE/MdeModulePkg/Include', root / 'MU_BASECORE/EmbeddedPkg/Include'):
    flags += ['-I', str(include)]
objects = []
for source in sources:
    obj = out / (source.stem + '.o')
    cmd = [a.clang, *flags, '-c', str(source), '-o', str(obj)]
    result = subprocess.run(cmd, text=True, capture_output=True)
    (out / (source.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stderr)
        result.check_returncode()
    objects.append({'source': str(source.relative_to(root)), 'command': cmd,
                    'object': str(obj), 'sha256': hashlib.sha256(obj.read_bytes()).hexdigest()})
inputs = sources + [s.with_suffix('.h') for s in sources] + [Path(__file__)]
(out / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': __doc__ + ' Real EFI ARM64 objects; not a firmware activation receipt.',
    'compiler': subprocess.check_output([a.clang, '--version'], text=True), 'objects': objects,
    'inputs': {str(s.relative_to(root)): hashlib.sha256(s.read_bytes()).hexdigest() for s in inputs}}, indent=2) + '\n')
print(f'PASS: {len(objects)} actual native extended frontend ARM64 objects')
