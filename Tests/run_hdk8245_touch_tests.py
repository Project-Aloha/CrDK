#!/usr/bin/env python3
"""DEBUG actual FTS/GPIO/EFI engines, ASAN/UBSAN and native ARM64 compile."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--fts-only', action='store_true', help='Run only affected FTS core/EFI and ARM64 objects')
parser.add_argument('--fts-source', type=Path, help='Immutable saved actual FTS core source through compiler overlay')
parser.add_argument('--fts-dxe-source', type=Path, help='Immutable saved actual FTS EFI source through compiler overlay')
parser.add_argument('--expect-packet-contract-failure', action='store_true', help='Require authentic old reg0/33 burst failure')
args = parser.parse_args()
cr = Path(__file__).resolve().parents[1]
root = cr.parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
overrides = {}
for canonical, alternate in [(cr/'Library/FocalTechTouchCrLib/FocalTechTouch.c', args.fts_source),
                             (cr/'Driver/FocalTechTouchCrDxe/FocalTechTouchCrDxe.c', args.fts_dxe_source)]:
 if alternate: overrides[str(canonical.resolve())] = str(alternate.resolve())
if args.expect_packet_contract_failure and not args.fts_source:
 parser.error('--expect-packet-contract-failure requires --fts-source')
overlay_options = []
if overrides:
 overlay = out/'source-overlay.json'
 overlay.write_text(json.dumps({'version':0, 'roots':[
  {'type':'file', 'name':key, 'external-contents':value, 'use-external-name':False}
  for key,value in overrides.items()]}))
 overlay_options = ['-ivfsoverlay', str(overlay)]
includes = [cr/'Include', root/'MU_BASECORE/MdePkg/Include', root/'MU_BASECORE/MdePkg/Include/X64']
groups = {
 'fts-core': [cr/'Tests/FocalTechTouchTest.c', cr/'Library/FocalTechTouchCrLib/FocalTechTouch.c', cr/'Target/Hdk8245/CrTouchTarget.c'],
 'fts-efi': [cr/'Tests/FocalTechTouchDxeTest.c', cr/'Library/FocalTechTouchCrLib/FocalTechTouch.c', cr/'Target/Hdk8245/CrTouchTarget.c'],
 'gpio-lease': [cr/'Tests/GpioPinLeaseTest.c', cr/'Library/GpioPinLeaseLib/GpioPinLease.c'],
 'gpio-register': [cr/'Tests/GpioPinLeaseRegisterTest.c', cr/'Library/GpioPinLeaseLib/GpioPinLease.c', cr/'Library/GpioLib/gpio.c', cr/'Target/Sm8250/CrGpioTarget.c'],
}
if args.fts_only: groups = {key:value for key,value in groups.items() if key.startswith('fts-')}
frozen = {}
runs = []
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def freeze(options, sources):
 for source in sources:
  dep = subprocess.check_output(options+['-MM', str(source)], text=True)
  for name in shlex.split(re.sub(r'\\\n', ' ', dep).split(':', 1)[1]):
   path = Path(overrides.get(str(Path(name).resolve()), name)).resolve()
   actual = digest(path)
   if str(path) in frozen and frozen[str(path)] != actual:
    raise RuntimeError('source drift: '+str(path))
   frozen[str(path)] = actual

for name, sources in groups.items():
 options = ['clang', '-std=c17', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
            '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
 options += ['-I'+str(p) for p in includes]
 options += overlay_options
 freeze(options, sources)
 exe = out/name
 command = options+[str(p) for p in sources]+['-Wl,--gc-sections', '-o', str(exe)]
 subprocess.run(command, check=True)
 result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
 (out/(name+'.log')).write_text(result.stdout+result.stderr)
 print(result.stdout+result.stderr, end='')
 if args.expect_packet_contract_failure and name.startswith('fts-'):
  assert result.returncode != 0 and 'R==1&&N==32' in result.stderr, 'expected actual old report-burst assertion'
 else: result.check_returncode()
 runs.append({'kind':'DEBUG ASAN UBSAN', 'command':command, 'exit':result.returncode,
              'stdout':result.stdout, 'binary_sha256':digest(exe)})

arm_sources = [cr/'Driver/FocalTechTouchCrDxe/FocalTechTouchCrDxe.c',
               cr/'Library/FocalTechTouchCrLib/FocalTechTouch.c',
               cr/'Library/GpioPinLeaseLib/GpioPinLease.c', cr/'Target/Hdk8245/CrTouchTarget.c']
if args.fts_only: arm_sources = arm_sources[:2]
for source in arm_sources:
 options = ['clang', '--target=aarch64-none-elf', '-std=c17', '-ffreestanding', '-fshort-wchar',
            '-fno-stack-protector', '-Wall', '-Wextra', '-Werror']
 options += ['-I'+str(p) for p in [cr/'Include', root/'MU_BASECORE/MdePkg/Include', root/'MU_BASECORE/MdePkg/Include/AArch64']]
 options += overlay_options
 freeze(options, [source])
 obj = out/(source.stem+'-arm64-debug.o')
 command = options+['-c', str(source), '-o', str(obj)]
 subprocess.run(command, check=True)
 runs.append({'kind':'ARM64 DEBUG compile only', 'command':command, 'object_sha256':digest(obj)})

subprocess.run(['python3', str(cr/'Tests/check_hdk8245_touch_path.py')], check=True)
for name, expected in frozen.items():
 if digest(Path(name)) != expected: raise RuntimeError('source drift: '+name)
inputs = [Path(__file__), cr/'Tests/check_hdk8245_touch_path.py', *arm_sources,
          cr/'Driver/FocalTechTouchCrDxe/FocalTechTouchCrDxe.inf',
          cr/'Library/FocalTechTouchCrLib/FocalTechTouchCrLib.inf',
          cr/'Library/GpioPinLeaseLib/GpioPinLeaseLib.inf',cr/'Target/Hdk8245/CrTouchTargetLib.inf']
inputs = [Path(overrides.get(str(p.resolve()), str(p))) for p in inputs]
if overrides: inputs.append(overlay)
(out/'artifacts.json').write_text(json.dumps({
 'status':'EXPECTED_PUBLIC_PACKET_CONTRACT_RED' if args.expect_packet_contract_failure else 'PASS',
 'scope':'Host executes actual FTS portable core, actual EFI adapter, actual GPIO lease and SM8250 MMIO engine; modeled EFI/provider/MMIO. ARM64 DEBUG object compile only. No device, flash, firmware download or physical touch proof.',
 'inputs':frozen, 'sources':{str(p):digest(p) for p in inputs}, 'source_overrides':overrides, 'runs':runs,
}, indent=2)+'\n')
print('HDK8245 DEBUG touch:', len(groups), 'sanitizer groups +', len(arm_sources), 'ARM64 objects',
      'EXPECTED RED' if args.expect_packet_contract_failure else 'PASS')
