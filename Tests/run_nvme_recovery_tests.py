#!/usr/bin/env python3
"""DEBUG ASan/UBSan tests of actual MU NVMe submission/recovery/ownership."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
modes = ('recursion', 'async-tpl', 'getters', 'map-quiesce', 'map-nested',
         'one-queue', 'queue-mismatch', 'negotiation', 'enable-failures', 'timer-close-reentry', 'internal-stop-failure', 'unmap-reentry', 'data', 'failures', 'stop-failures', 'poll-reentry')
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--source', type=Path, default=crane / 'Override/MU_BASECORE/MdeModulePkg/Bus/Pci/NvmExpressDxe')
parser.add_argument('--mode', default='all', choices=('all', 'queue-red', 'stop-red') + modes)
args = parser.parse_args()
output_dir = args.output.resolve()
output_dir.mkdir(parents=True, exist_ok=True)
snapshot = output_dir / 'inputs'
snapshot.mkdir(exist_ok=True)
inputs = [Path(__file__), crane / 'Tests/NvmeRecoveryTest.c'] + sorted(args.source.glob('*.[ch]'))
input_receipt = {}
for item in inputs:
    destination = snapshot / item.name
    shutil.copyfile(item, destination)
    data = destination.read_bytes()
    input_receipt[str(item.resolve())] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                                        'snapshot': str(destination)}
edk = root / 'MU_BASECORE'
binary = output_dir / 'nvme-recovery'
cmd = ['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
       '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
       '-mllvm', '-asan-globals=0', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
       '-D_PCD_GET_MODE_BOOL_PcdSupportAlternativeQueueSize=0',
       '-D_PCD_GET_MODE_32_PcdNvmeNamespaceFilterId=0', '-D_PCD_VALUE_PcdNvmeGenericTimeout=5']
for directory in (snapshot, edk / 'MdePkg/Include', edk / 'MdePkg/Include/X64', edk / 'MdeModulePkg/Include'):
    cmd += ['-I' + str(directory)]
syntax_base = [arg for arg in cmd if arg != '-Wl,--gc-sections']
syntax_checks = {}
for name in ('NvmExpress', 'NvmExpressHci', 'NvmExpressPassthru', 'NvmExpressBlockIo'):
    # The firmware AutoGen header provides PcdLib for each translation unit.
    syntax_command = syntax_base + ['-include', 'Uefi.h', '-include', 'Library/PcdLib.h',
                                    '-fsyntax-only', str(snapshot / (name + '.c'))]
    syntax = subprocess.run(syntax_command, capture_output=True, text=True)
    syntax_output = syntax.stdout + syntax.stderr
    syntax_log = output_dir / (name + '-compile.log')
    syntax_log.write_text(syntax_output)
    syntax_checks[name] = {'returncode': syntax.returncode, 'command': syntax_command,
                          'log_sha256': hashlib.sha256(syntax_output.encode()).hexdigest()}
    if syntax.returncode:
        raise SystemExit(syntax_output)
cmd += [str(snapshot / 'NvmeRecoveryTest.c'), '-o', str(binary)]
compile_result = subprocess.run(cmd, capture_output=True, text=True)
output = compile_result.stdout + compile_result.stderr
returncode = compile_result.returncode
cases = {}
if returncode == 0:
    for mode in modes if args.mode == 'all' else (args.mode,):
        result = subprocess.run([str(binary), mode], capture_output=True, text=True)
        case_output = result.stdout + result.stderr
        (output_dir / (mode + '-DEBUG.log')).write_text(case_output)
        cases[mode] = {'returncode': result.returncode,
                       'log_sha256': hashlib.sha256(case_output.encode()).hexdigest()}
        output += '[' + mode + ']\n' + case_output
        if result.returncode != 0 and returncode == 0:
            returncode = result.returncode
(output_dir / 'test-DEBUG.log').write_text(output)
receipt = {'mode': 'DEBUG host ASan/UBSan', 'case': args.mode, 'returncode': returncode,
           'command': cmd, 'inputs': input_receipt, 'cases': cases, 'syntax_checks': syntax_checks,
           'log_sha256': hashlib.sha256(output.encode()).hexdigest(),
           'binary': {'path': str(binary), 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
                     if compile_result.returncode == 0 else None}
(output_dir / 'artifacts.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(output, end='')
raise SystemExit(returncode)
