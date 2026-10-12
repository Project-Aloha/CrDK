#!/usr/bin/env python3
"""Run actual DWC3 core revision guards, with optional frozen before source."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, required=True)
p.add_argument('--core-source', type=Path)
p.add_argument('--case', choices=['all', 'hdk190', 'events33', 'events18'], default='all')
args = p.parse_args()
crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
source = (args.core_source or crane / 'Driver/Dwc3CrDxe/Dwc3Core.c').resolve()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
fixture = crane / 'Tests/Dwc3CoreTest.c'
binary = out / 'dwc3-core-test'
includes = [crane / 'Include', root / 'MU_BASECORE/MdePkg/Include',
            root / 'MU_BASECORE/MdePkg/Include/X64',
            root / 'MU_BASECORE/MdeModulePkg/Include',
            crane / 'Driver/Dwc3CrDxe']
cmd = ['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra',
       '-Werror', '-Wno-unused-parameter', '-fsanitize=address,undefined',
       '-fno-sanitize-recover=all', *['-I' + str(i) for i in includes],
       '-DCR_DWC3_CORE_SOURCE="' + str(source) + '"', str(fixture), str(crane/'Library/Dwc3CrLib/Dwc3.c'), '-o', str(binary)]
subprocess.run(cmd, check=True)
test_command = [str(binary)] + ([] if args.case == 'all' else [args.case])
result = subprocess.run(test_command, capture_output=True, text=True)
(out / 'test.log').write_text(result.stdout + result.stderr)
receipt = {'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
           'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
           'portable_core_sha256': hashlib.sha256((crane/'Library/Dwc3CrLib/Dwc3.c').read_bytes()).hexdigest(),
           'portable_header_sha256': hashlib.sha256((crane/'Include/Library/Dwc3CrLib.h').read_bytes()).hexdigest(),
           'compile_command': cmd, 'test_command': test_command, 'returncode': result.returncode,
           'hardware_claim': False}
(out / 'artifacts.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(result.stdout + result.stderr, end='')
raise SystemExit(result.returncode)
