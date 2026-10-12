#!/usr/bin/env python3
"""Hostile/repeatable two-build pipeline using real compiled ARM64 PE/archives.

Reuses the independent fixture builders, not its assertions. Publisher build
stamp function is extracted from the actual frontend and executed separately
under sanitizers. These filesystem/build models do not activate real firmware.
"""
import argparse
import ast
import hashlib
import importlib.util
import json
import shutil
import struct
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
native = Path(__file__).resolve().parents[3]
wd = native.parent/'CrWDDK'
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('pipeline', native/'Platforms/CranePkg/Tools/uart_handoff_pipeline.py')
pipeline = importlib.util.module_from_spec(spec); spec.loader.exec_module(pipeline)
tool = pipeline.coverage
commands, rows = [], []
guid = '7a867a45-96ce-498d-8f3b-81cb49f9efda'
libguid = 'c5420575-674a-477c-9c2a-98a0d98df349'
import uuid
fixture_source = wd/'tests/run_uart_firmware_coverage_tests.py'
tree = ast.parse(fixture_source.read_text())
definitions = ast.Module(body=[x for x in tree.body if isinstance(x, ast.FunctionDef)], type_ignores=[])
exec(compile(definitions, str(fixture_source), 'exec'), globals())

def participants(ws):
    for part in ('Library/UartAdmissionLib', 'Library/QcomGeniGatedSerialPortLib',
                 'Library/UartHandoffLib', 'Library/UartHandoffUefiLib',
                 'Driver/UartHandoffCrDxe', 'Include/UartHandoff'):
        shutil.copytree(native/'Platforms/CranePkg'/part, ws/'Platforms/CranePkg'/part, dirs_exist_ok=True)
    for key in pipeline.source_snapshot(native):
        target = ws/key; target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((native/key).read_bytes())

stamp_function = (native/'Platforms/CranePkg/Driver/UartHandoffCrDxe/UartHandoffCrDxe.c').read_text().split(
    'STATIC BOOLEAN CoverageStampValid(VOID){', 1)[1].split('\nSTATIC VOID EFIAPI Ready', 1)[0]
stamp_function = 'STATIC BOOLEAN CoverageStampValid(VOID){'+stamp_function
types = '''#include <stdint.h>
typedef uint8_t UINT8;typedef uint32_t UINT32;typedef unsigned long long UINTN;
typedef void VOID;typedef int BOOLEAN;
#define STATIC static
#define CONST const
#define TRUE 1
#define FALSE 0
#define CR_UART_HANDOFF_IMAGES 32
typedef struct { UINT8 Guid[16];UINT32 Kind,Bytes,Checksum,Reserved; } BUILD_IMAGE;
typedef struct {UINT32 Revision,Count;UINT8 Digest[32];BUILD_IMAGE Images[32];} CR_UART_FIRMWARE_COVERAGE;
'''

def publisher(ws, build, payload=None, duplicate=False):
    module = build/'AARCH64/Test/Publisher'; module.mkdir(parents=True, exist_ok=True)
    make_metadata(module/'GNUmakefile', 'UartHandoffCrDxe', pipeline.PUBLISHER_GUID,
                  'DXE_DRIVER', ws/pipeline.PUBLISHER)
    (module/'OUTPUT').mkdir(exist_ok=True)
    # Use the actual selected gated boot archive from a separately compiled
    # helper fixture; never pretend a serial-free publisher is fully gated.
    helper = setup('publisher-library', variant=tool.GATED_DXE, runtime=False)
    archive = module/'OUTPUT/Serial.lib'; archive.write_bytes(helper[-1].read_bytes())
    make_metadata(module/'GNUmakefile', 'Serial', libguid, 'BASE', ws/tool.GATED_DXE)
    # The module and selected archive need distinct generated identities.
    library = build/'AARCH64/Test/PublisherSerial'; library.mkdir(exist_ok=True)
    (library/'OUTPUT').mkdir(exist_ok=True)
    actual_archive = library/'OUTPUT/Serial.lib'; actual_archive.write_bytes(archive.read_bytes())
    archive.unlink()
    make_metadata(library/'GNUmakefile', 'Serial', libguid, 'BASE', ws/tool.GATED_DXE)
    make_metadata(module/'GNUmakefile', 'UartHandoffCrDxe', pipeline.PUBLISHER_GUID,
                  'DXE_DRIVER', ws/pipeline.PUBLISHER)
    (module/'OUTPUT/static_library_files.lst').write_text(str(actual_archive)+'\n')
    (module/'DEBUG').mkdir(exist_ok=True)
    (module/'DEBUG/AutoGen.c').write_text('CrUartSerialConstructor(ImageHandle,SystemTable);\nCrUartSerialDestructor(ImageHandle,SystemTable);\n')
    header = ((ws/pipeline.HEADER).read_text() if payload is None else
              tool.coverage_header(payload, bootstrap=struct.unpack_from('<I', payload, 4)[0] == 0))
    header = header.replace('#pragma once', '').replace('#include <Interface/CrUartHandoff.h>', '')
    code = types+header+stamp_function+'\nvolatile UINT8 Sink;\n'
    if duplicate:
        code += 'volatile const UINT8 Duplicate[64]={' + ','.join(map(str,tool.build_stamp(payload or tool.coverage_bytes()))) + '};\n'
    code += 'int _ModuleEntryPoint(void){for(UINTN i=0;i<sizeof(mUartFirmwareCoverage);i++)Sink^=((const UINT8*)&mUartFirmwareCoverage)[i];'
    if duplicate: code += 'Sink^=Duplicate[0];'
    code += 'return CoverageStampValid()?0:1;}\n'
    c = module/'entry.c'; c.write_text(code)
    run(['clang', '--target=aarch64-unknown-windows-msvc', '-ffreestanding', '-O1', '-c', str(c), '-o', str(module/'entry.obj')])
    efi = module/'DEBUG/UartHandoffCrDxe.efi'
    run(['lld-link', '/machine:arm64', '/entry:_ModuleEntryPoint', '/nodefaultlib', '/subsystem:efi_boot_service_driver',
         str(module/'entry.obj'), '/wholearchive:'+str(actual_archive), '/out:'+str(efi)])
    runtime = build/'AARCH64/Test/Driver/DEBUG/Test.efi'
    actual_fv = build/'FV/FVMAIN.Fv'
    actual_fv.write_bytes(append_file(fv(runtime.read_bytes()), fv(efi.read_bytes(), ident=pipeline.PUBLISHER_GUID)))

ws, build, image, module, library, archive = setup('pipeline')
participants(ws)
def phase(name, previous=None, expected=True):
    path = out/(name+'.json')
    result = pipeline.execute(name.split('-')[0], ws, path, build, image, previous=previous)
    assert result['success'] is expected, (name, result.get('errors'))
    assert result['package_eligible'] is (expected and name.split('-')[0] in ('validate','verify'))
    rows.append({'case': name, 'accepted': expected, 'errors': result['errors']})
    return path

bootstrap = phase('bootstrap')
assert (ws/pipeline.HEADER).read_bytes() == tool.coverage_header(tool.coverage_bytes(), bootstrap=True).encode()
phase('inventory-missing-publisher', bootstrap, False)
publisher(ws,build, duplicate=True)
phase('inventory-ambiguous-stamp', bootstrap, False)
publisher(ws,build)
inventory = phase('inventory', bootstrap)
candidate = (ws/pipeline.HEADER).read_bytes()
phase('validate-stale-bootstrap-publisher', inventory, False)
publisher(ws,build)
validated = phase('validate', inventory)
saved_publisher = (build/'AARCH64/Test/Publisher/DEBUG/UartHandoffCrDxe.efi').read_bytes()
publisher_archive = build/'AARCH64/Test/PublisherSerial/OUTPUT/Serial.lib'
saved_publisher_archive = publisher_archive.read_bytes()
saved_fv = image.read_bytes()
phase('verify', validated)
phase('validate-repeatable', inventory)
assert (ws/pipeline.HEADER).read_bytes() == candidate

saved = (module/'DEBUG/Test.efi').read_bytes()
changed = bytearray(saved); changed[-1] ^= 1
(module/'DEBUG/Test.efi').write_bytes(changed); publisher(ws, build, payload=bytes.fromhex(json.loads(inventory.read_text())['coverage_hex']))
phase('validate-runtime-drift', inventory, False)
phase('verify-runtime-drift', validated, False)
(module/'DEBUG/Test.efi').write_bytes(saved)
(build/'AARCH64/Test/Publisher/DEBUG/UartHandoffCrDxe.efi').write_bytes(saved_publisher)
publisher_archive.write_bytes(saved_publisher_archive)
image.write_bytes(saved_fv)
phase('verify-restored-exact-inputs', validated)
source = ws/'Platforms/CranePkg/Library/UartAdmissionLib/UartAdmission.c'; saved_source = source.read_bytes()
source.write_bytes(saved_source+b'\n/* drift */\n')
phase('validate-participant-drift', inventory, False)
source.write_bytes(saved_source)
header = ws/pipeline.HEADER; saved_header = header.read_bytes(); header.write_bytes(saved_header+b'\n')
phase('validate-header-drift', inventory, False); header.write_bytes(saved_header)
bad = out/'foreign-receipt.json'; receipt = json.loads(inventory.read_text());receipt['workspace'] = str(native);bad.write_text(json.dumps(receipt))
phase('validate-foreign-receipt', bad, False)
receipt['workspace'] = str(ws.resolve());receipt['success'] = False;bad.write_text(json.dumps(receipt))
phase('validate-failed-receipt', bad, False)
# Candidate data in the first build is not an invalid bootstrap.
payload = bytes.fromhex(json.loads(inventory.read_text())['coverage_hex'])
boot = json.loads(bootstrap.read_text());boot['coverage_hex'] = payload.hex();bad.write_text(json.dumps(boot))
phase('inventory-qualified-first-header', bad, False)

# Execute the actual stamp validator at native width, flipping every byte of
# stamp and payload; malformed data must fail before any event/page/publication.
host = out/'stamp.c'
mutable = tool.coverage_header(payload).replace('#pragma once','').replace('#include <Interface/CrUartHandoff.h>','').replace('STATIC CONST','STATIC')
host.write_text('#include <assert.h>\n#include <stdio.h>\n'+types+mutable+stamp_function+'''
int main(void){assert(CoverageStampValid());
 for(UINTN i=0;i<64;i++){mUartCoverageBuildStamp[i]^=1;assert(!CoverageStampValid());mUartCoverageBuildStamp[i]^=1;}
 UINT8 *p=(UINT8*)&mUartFirmwareCoverage;
 for(UINTN i=0;i<sizeof(mUartFirmwareCoverage);i++){p[i]^=1;assert(!CoverageStampValid());p[i]^=1;}
 assert(CoverageStampValid());puts("Actual publisher stamp: 1128 corruption cases PASS");return 0;}
''')
run(['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
     '-fno-sanitize-recover=all',str(host),'-o',str(out/'stamp')]);run([str(out/'stamp')])
inputs = [Path(__file__),fixture_source,native/'Platforms/CranePkg/Tools/uart_handoff_pipeline.py',
          native/'Platforms/CranePkg/Tools/check_uart_firmware_coverage.py',
          native/'Platforms/CranePkg/Driver/UartHandoffCrDxe/UartHandoffCrDxe.c']
(out/'artifacts.json').write_text(json.dumps({'scope':__doc__,'cases':rows,'commands':commands,
    'inputs':{str(q):hashlib.sha256(q.read_bytes()).hexdigest() for q in inputs}},indent=2)+'\n')
print(f'PASS {len(rows)} ARM64 two-build hostile/repeatability cases and 1128 actual stamp corruptions')
