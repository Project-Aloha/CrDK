#!/usr/bin/env python3
"""Checked DP AUX core, native/NT hostile callbacks and actual native parity."""
from pathlib import Path
import argparse
import hashlib
import json
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
pkg = root/'Platforms/CranePkg'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out/'artifacts.json').unlink(missing_ok=True)
core = pkg/'Library/MdssDpCrLib/DpAux.c'
fixture = pkg/'Tests/MdssDpAuxTest.c'
inputs = {Path(__file__).resolve(), pkg/'Include/Library/MdssDpCrLib.h', core, fixture}
runs = []
for name in ('native', 'nt', 'native-parity'):
    sources = [fixture, core]
    flags = ['clang', '-std=c17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I'+str(pkg/'Include')]
    if name == 'nt':
        flags += ['-D_KERNEL_MODE', '-I'+str(pkg/'Tests/KernelMocks')]
    else:
        flags += ['-fshort-wchar', '-I'+str(root/'MU_BASECORE/MdePkg/Include'),
                  '-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    if name == 'native-parity':
        flags += ['-DDP_NATIVE_PARITY']
        sources += [pkg/'Driver/MDSSDpCrDxe/DpAux.c']
    for index, source in enumerate(sources):
        dep = out/f'{name}-{index}.d'
        cmd = [*flags, '-M', '-MT', name, '-MF', str(dep), str(source)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr)
        r.check_returncode()
        inputs.update(Path(x).resolve() for x in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]))
    held = {str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
    exe = out/name
    cmd = [*flags, *map(str, sources), '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (out/f'{name}-compile.log').write_text(r.stdout+r.stderr)
    if r.returncode:
        print(r.stderr)
    r.check_returncode()
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    (out/f'{name}.log').write_text(run.stdout+run.stderr)
    print(run.stdout+run.stderr, end='')
    run.check_returncode()
    if held != {str(x): hashlib.sha256(Path(x).read_bytes()).hexdigest() for x in held}:
        raise RuntimeError('source changed during tests')
    runs += [{'name': name, 'command': cmd, 'status': 'PASS'}]
refs = [root/'.vscode/linux/drivers/gpu/drm/msm/dp/dp_aux.c',
        root/'.vscode/linux/drivers/gpu/drm/msm/dp/dp_reg.h',
        root/'.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450/vendor/qcom/opensource/display-drivers/msm/dp/dp_catalog_v420.c',
        root/'.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450/vendor/qcom/opensource/display-drivers/msm/dp/dp_reg.h']
inputs.update(refs)
sha = lambda x: hashlib.sha256(x.read_bytes()).hexdigest()
(out/'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': __doc__,
    'limits': 'Controlled MMIO/PHY owner callbacks; no physical DP, new native caller or Windows ownership activation.',
    'compiler': subprocess.check_output(['clang', '--version'], text=True), 'runs': runs,
    'inputs': {str(x): sha(x) for x in sorted(inputs)},
    'outputs': {str(x): sha(x) for x in sorted(out.iterdir()) if x.is_file() and x.name != 'artifacts.json'},
}, indent=2)+'\n')
