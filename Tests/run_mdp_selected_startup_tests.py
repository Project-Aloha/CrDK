#!/usr/bin/env python3
"""Check selected-panel boot brightness ordering against modeled EFI boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pkg = Path(__file__).resolve().parents[1]
    root = pkg.parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = pkg / 'Tests/MdpSelectedPanelStartupTest.c'
    flags = ['-std=c17', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-DMDEPKG_NDEBUG', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-ffunction-sections', '-fdata-sections', '-fno-pie']
    for include in (pkg/'Include', root/'Platforms/MUCommonCrPkg/Include',
                    root/'MU_BASECORE/MdePkg/Include', root/'MU_BASECORE/MdePkg/Include/X64',
                    root/'MU_BASECORE/MdeModulePkg/Include', root/'MU_BASECORE/EmbeddedPkg/Include'):
        flags.append('-I'+str(include))
    deps = subprocess.check_output(['clang', *flags, '-MM', str(source)], text=True)
    inputs = {Path(__file__).resolve()}
    inputs.update(Path(x).resolve() for x in shlex.split(deps.replace('\\\n', ' ').split(':', 1)[1]))
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    before = {str(p.relative_to(root)): digest(p) for p in sorted(inputs)}
    binary = out/'selected-panel-startup'
    cmd = ['clang', *flags, '-no-pie', '-Wl,--gc-sections', str(source), '-o', str(binary)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    (out/'compile.log').write_text(result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr)
    cases = []
    for case in range(11):
        result = subprocess.run([str(binary), str(case)], capture_output=True, text=True, timeout=30)
        (out/f'case-{case}.log').write_text(result.stdout+result.stderr)
        print(result.stdout+result.stderr, end='')
        cases.append({'case': case, 'exit': result.returncode})
    if any(digest(root/name) != value for name, value in before.items()):
        raise RuntimeError('source drift during verification')
    passed = all(case['exit'] == 0 for case in cases)
    (out/'receipt.json').write_text(json.dumps({
        'status': 'PASS' if passed else 'FAIL', 'cases': cases, 'inputs': before,
        'command': cmd, 'binary_sha256': digest(binary),
        'scope': 'Full selected frontend; brightness backend and EFI publication modeled. Not board proof.'
    }, indent=2)+'\n')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
