#!/usr/bin/env python3
"""Run the actual DAL FDT consumer and BDS charger dispatch under sanitizers."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
c = Path(__file__).resolve().parents[1]
root = c.parents[1]
edk = root / 'MU_BASECORE'
source = c / 'Tests/ChargerBootIntegrationTest.c'
binary = a.output / 'charger-boot-integration'
cmd = ['clang', '-std=c11', '-fshort-wchar', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
       '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
for directory in (c/'Include', root/'Platforms/MUCommonCrPkg/Include', edk/'MdePkg/Include',
                  edk/'MdePkg/Include/X64', edk/'ArmPkg/Include'):
    cmd += ['-I'+str(directory)]
subprocess.run(cmd+[str(source), '-o', str(binary)], check=True)
result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
print(result.stdout, end='')
print(result.stderr, end='')
result.check_returncode()
inputs = [Path(__file__), source,
          c/'Include/Protocol/EFICrFdtProtocol.h',
          c/'Include/Library/AndroidBootCrLib.h',
          c/'Library/ChargerBootCrLib/ChargerBootCrLib.c']
(a.output/'artifacts.json').write_text(json.dumps({
    'sources': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},
    'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'output': result.stdout}, indent=2)+'\n')
