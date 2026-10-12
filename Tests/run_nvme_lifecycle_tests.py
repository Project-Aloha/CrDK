#!/usr/bin/env python3
"""Compile actual MU NVMe lifecycle functions with DEBUG ASan/UBSan mocks.

--old-child reads an immutable pre-change source directory and runs only the
same live-child Stop contract; it never edits production inputs.  GREEN runs
the complete production translation units with mocked PCI/boot providers.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    crane = Path(__file__).resolve().parents[1]
    root = crane.parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source', type=Path,
                        default=crane / 'Override/MU_BASECORE/MdeModulePkg/Bus/Pci/NvmExpressDxe')
    parser.add_argument('--old-child', action='store_true')
    parser.add_argument('--case', choices=('all', 'child-atomic', 'service-reentry', 'install-notify'), default='all')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    args.source = args.source.resolve()
    edk = root / 'MU_BASECORE'
    fixture = crane / 'Tests/NvmeLifecycleTest.c'
    binary = args.output.resolve() / 'nvme-lifecycle'
    inputs = [Path(__file__), fixture] + sorted(args.source.glob('*.[ch]'))
    snapshots = args.output.resolve() / 'inputs'
    snapshots.mkdir(exist_ok=True)
    input_receipt = {}
    for path in inputs:
        data = path.read_bytes()
        snapshot = snapshots / path.name
        snapshot.write_bytes(data)
        input_receipt[str(path.resolve())] = {
            'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'snapshot': str(snapshot),
        }
    cmd = ['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-parameter', '-Wno-unused-function',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           '-mllvm', '-asan-globals=0', '-ffunction-sections', '-fdata-sections',
           '-Wl,--gc-sections', '-D_PCD_GET_MODE_BOOL_PcdSupportAlternativeQueueSize=0',
           '-D_PCD_GET_MODE_32_PcdNvmeNamespaceFilterId=0', '-D_PCD_VALUE_PcdNvmeGenericTimeout=5']
    if args.old_child:
        cmd += ['-DNVME_LIFECYCLE_OLD_CHILD=1', '-Wno-unused-variable']
    for directory in (snapshots, edk / 'MdePkg/Include', edk / 'MdePkg/Include/X64',
                      edk / 'MdeModulePkg/Include'):
        cmd += ['-I' + str(directory)]
    cmd += [str(snapshots / fixture.name), '-o', str(binary)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    compiled = result.returncode == 0
    output = result.stdout + result.stderr
    if result.returncode == 0:
        try:
            result = subprocess.run([str(binary), args.case], capture_output=True, text=True, timeout=20)
            output += result.stdout + result.stderr
        except subprocess.TimeoutExpired as error:
            output += str(error) + '\n'
            result = subprocess.CompletedProcess([str(binary)], 124)
    (args.output / 'test-DEBUG.log').write_text(output)
    receipt = {'mode': 'DEBUG host ASan/UBSan', 'old_child': args.old_child, 'case': args.case,
               'returncode': result.returncode, 'command': cmd,
               'inputs': input_receipt,
               'binary': {'path': str(binary), 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
                         if compiled else None,
               'log_sha256': hashlib.sha256(output.encode()).hexdigest()}
    (args.output / 'artifacts.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(output, end='')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
