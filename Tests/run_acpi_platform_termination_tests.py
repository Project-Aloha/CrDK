#!/usr/bin/env python3
"""Test actual ACPI/FV termination and native DXE unload-status diagnostics."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

root = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--acpi-source', type=Path, default=root / 'Platforms/CranePkg/Override/MU_BASECORE/MdeModulePkg/Universal/Acpi/AcpiPlatformDxe/AcpiPlatform.c')
parser.add_argument('--image-source', type=Path, default=root / 'MU_BASECORE/MdeModulePkg/Core/Dxe/Image/Image.c')
parser.add_argument('--expect-before', action='store_true')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)

def function(source, name):
    match = re.search(r'^(?:EFI_STATUS|VOID)\n(?:EFIAPI\n)?' + name + r' \([^;{]*\)\n\{', source, re.M)
    assert match, name
    end = match.end(); depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[match.start():end]

acpi = args.acpi_source.read_text()
(out / 'AcpiPlatformTerminationProduction.c').write_text('\n\n'.join(function(acpi, name) for name in (
    'LocateFvInstanceWithTables', 'AcpiPlatformChecksum', 'AcpiPlatformEntryPoint')) + '\n')
image = args.image_source.read_text()
begin = image.index('  DEBUG_CODE_BEGIN ();', image.index('// Add some debug information if the image returned with error.'))
end = image.index('  DEBUG_CODE_END ();', begin)
diagnostic = image[begin + len('  DEBUG_CODE_BEGIN ();'):end]
(out / 'DxeImageExitDiagnostic.c').write_text(diagnostic)
fixture = Path(__file__).with_name('AcpiPlatformTerminationTest.c')
exe = out / 'test'
command = ['clang', '-std=gnu11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
if args.expect_before:
    command += ['-DTEST_BEFORE_TERM']
for include in (out, root / 'MU_BASECORE/MdePkg/Include', root / 'MU_BASECORE/MdePkg/Include/X64'):
    command += ['-I' + str(include)]
subprocess.run([*command, str(fixture), '-o', str(exe)], check=True)
cases = ('normal', 'empty', 'first-read-error', 'later-read-error', 'install-error', 'locate-error',
         'unload-diagnostic', 'error-diagnostic', 'success-diagnostic')
before_failures = {'empty', 'first-read-error', 'later-read-error', 'unload-diagnostic'}
results = {}
for case in cases:
    result = subprocess.run([str(exe), case], text=True, capture_output=True, timeout=15)
    (out / (case + '.log')).write_text(result.stdout + result.stderr)
    expected_failure = args.expect_before and case in before_failures
    if expected_failure:
        assert result.returncode != 0 and 'Assertion' in result.stderr, (case, result.stdout, result.stderr)
    else:
        result.check_returncode()
    results[case] = {'returncode': result.returncode, 'expected_before_failure': expected_failure}
    print(case, 'EXPECTED BEFORE FAILURE' if expected_failure else 'PASS')
inputs = (args.acpi_source, args.image_source, fixture, Path(__file__))
(out / 'artifacts.json').write_text(json.dumps({'passed': True, 'before': args.expect_before,
    'scope': 'actual ACPI locate/read/checksum/install loop and actual DXE diagnostic block; FV, Boot Services, table-install boundaries modeled',
    'sanitizers': ['address', 'undefined'], 'cases': results,
    'inputs': {str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
    'binary': hashlib.sha256(exe.read_bytes()).hexdigest()}, indent=2) + '\n')
