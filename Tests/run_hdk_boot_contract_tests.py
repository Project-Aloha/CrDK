#!/usr/bin/env python3
"""Actual HDK console/early-storage code, with modeled EFI services.

Runs full production translation units under Clang ASan/UBSan. These tests
verify software contracts, not physical display, firmware loading or media.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    crane = Path(__file__).resolve().parents[1]
    root = crane.parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    autogen = out / 'AutoGen.h'
    autogen.write_text('#include <Uefi.h>\nextern EFI_GUID gEfiCallerIdGuid;\n')
    flags = ['clang', '-std=c11', '-fshort-wchar', '-DMDEPKG_NDEBUG',
             '-include', str(autogen), '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-Wno-unused-function',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-ffunction-sections', '-fdata-sections']
    flags += ['-I' + str(root / p) for p in (
        'MU_BASECORE/MdePkg/Include', 'MU_BASECORE/MdePkg/Include/X64',
        'MU_BASECORE/ArmPkg/Include', 'Common/MU/MsGraphicsPkg/Include',
        'Common/MU/PcBdsPkg/Include', 'Common/MU/MsWheaPkg/Include',
        'Platforms/MUCommonCrPkg/Include')]
    console_flags = []
    for token, name in enumerate(('PcdVideoHorizontalResolution', 'PcdVideoVerticalResolution',
                                  'PcdConOutColumn', 'PcdConOutRow'), 1):
        console_flags += [f'-D_PCD_TOKEN_{name}={token}',
                          f'-D_PCD_SET_MODE_32_S_{name}(V)=LibPcdSet32S({token},(V))']
    storage_flags = ['-D_PCD_TOKEN_PcdMsWheaReportEarlyStorageCapacity=1',
                     '-D_PCD_GET_MODE_32_PcdMsWheaReportEarlyStorageCapacity=LibPcdGet32(1)',
                     '-D_PCD_TOKEN_PcdMsWheaEarlyStorageDefaultValue=2',
                     '-D_PCD_GET_MODE_8_PcdMsWheaEarlyStorageDefaultValue=LibPcdGet8(2)']
    common = root / 'Platforms/MUCommonCrPkg/Library'
    cases = [
        ('native-console', 14, console_flags, crane / 'Tests/NativeGraphicsConsoleTest.c',
         crane / 'Library/NativeGraphicsConsoleHelperLib/NativeGraphicsConsoleHelper.c'),
        ('early-storage', 12, storage_flags, crane / 'Tests/WheaStorageRangeTest.c',
         common / 'MsWheaEarlyStorageLib/MsWheaEarlyStorageLib.c')]
    records = []
    dependencies = {Path(__file__).resolve()}
    failed = False
    for name, count, extra, fixture, source in cases:
        directory = out / name
        directory.mkdir(exist_ok=True)
        binary = directory / 'test'
        for item in (fixture, source):
            dep = subprocess.check_output(flags + extra + ['-MM', str(item)], text=True)
            dependencies.update(Path(p).resolve() for p in
                                shlex.split(dep.split(':', 1)[1].replace('\\\n', ' ')))
        command = flags + extra + [str(fixture), str(source), '-Wl,--gc-sections', '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (directory / 'compile.log').write_text(compiled.stdout + compiled.stderr)
        record = {'name': name, 'command': command, 'compile_exit': compiled.returncode, 'cases': []}
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr)
            failed = True
        else:
            for number in range(count):
                result = subprocess.run([str(binary), str(number)], capture_output=True, text=True, timeout=30)
                text = result.stdout + result.stderr
                (directory / f'{number}.log').write_text(text)
                print(text, end='')
                record['cases'].append({'case': number, 'exit': result.returncode,
                                        'log_sha256': hashlib.sha256(text.encode()).hexdigest()})
                failed |= result.returncode != 0
        records.append(record)
    inputs = {str(p.relative_to(root) if p.is_relative_to(root) else p):
              hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(dependencies)}
    (out / 'receipt.json').write_text(json.dumps({'status': 'FAIL' if failed else 'PASS',
        'scope': __doc__, 'cases': records, 'inputs': inputs}, indent=2) + '\n')
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
