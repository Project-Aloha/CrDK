#!/usr/bin/env python3
"""Two-build UART link qualification; never builds, boots, installs or packages.

Bootstrap writes an invalid Count0 header. Inventory checks the actual first FV
and its selected archives, then replaces only that header with a candidate.
Validate rechecks the actual second FV and exact runtime fixed point, including
the live stamp AND full coverage data in its real boot-only publisher PE.
These are trusted build receipts, not authentication or a native/Windows grant.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('cr_uart_coverage', HERE/'check_uart_firmware_coverage.py')
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)
HEADER = 'Platforms/CranePkg/Driver/UartHandoffCrDxe/UartFirmwareCoverage.h'
PUBLISHER = 'Platforms/CranePkg/Driver/UartHandoffCrDxe/UartHandoffCrDxe.inf'
PUBLISHER_GUID = '20382af5-7edc-46b8-b3e5-b364d6d28452'
SCOPE = ('ARM64 FV/PE/archive/generated constructor/runtime fixed point only; '
         'reviewed direct-writer inventory, successful EBS closure/drain/GENI '
         'retirement, reserved record and OS/provider grants remain required')


def sha(value):
    return hashlib.sha256(value).hexdigest()


def load_receipt(path, phase, workspace):
    data = json.loads(path.read_text())
    if (data.get('schema') != 1 or data.get('phase') != phase or
            data.get('success') is not True or data.get('workspace') != str(workspace) or
            data.get('tool_sha256') != sha(Path(__file__).read_bytes()) or
            data.get('coverage_tool_sha256') != sha((HERE/'check_uart_firmware_coverage.py').read_bytes())):
        raise coverage.Invalid('stale, failed or foreign pipeline receipt')
    return data


def source_snapshot(workspace):
    """All UART participant implementation inputs, excluding only generated header.

    Linked archive/PE digests additionally cover actual target-selected inputs.
    This fixed source scope is an auditable inventory, not an arbitrary MMIO
    absence proof. Optional activation includes must be reviewed by the owner.
    """
    files = set()
    for part in ('Library/UartAdmissionLib', 'Library/QcomGeniGatedSerialPortLib',
                 'Library/UartHandoffLib', 'Library/UartHandoffUefiLib',
                 'Driver/UartHandoffCrDxe', 'Include/UartHandoff'):
        files.update((workspace/'Platforms/CranePkg'/part).rglob('*'))
    for name in ('Interface/CrUartHandoff.h', 'Library/UartAdmissionLib.h',
                 'Library/UartAdmissionUefiLib.h', 'Library/UartHandoffLib.h',
                 'Library/UartHandoffUefiLib.h'):
        files.add(workspace/'Platforms/CranePkg/Include'/name)
    files.update((workspace/'Platforms/MUCommonCrPkg/Library/QcomGeniSerialPortLib').rglob('*'))
    return {p.relative_to(workspace).as_posix(): sha(p.read_bytes())
            for p in sorted(files) if p.is_file() and p.relative_to(workspace).as_posix() != HEADER
            and p.suffix in ('.c', '.h', '.S', '.inf', '.inc')}


def checked_build(workspace, build, fv, nm):
    result = coverage.check(workspace, build, fv, nm)
    if not result['link_coverage_complete']:
        raise coverage.Invalid('incomplete actual link coverage: ' + '; '.join(result['errors']))
    # Validate capacities/providers even for the invalid-header first build.
    coverage.coverage_bytes(result)
    return result


def publisher_stamp(workspace, fv, result, payload):
    rows = [row for row in result['images'] if row['guid'] == PUBLISHER_GUID]
    if (len(rows) != 1 or rows[0].get('source') != str(workspace/PUBLISHER) or
            rows[0].get('module_type') != 'DXE_DRIVER' or rows[0]['subsystem'] != 11 or
            rows[0].get('link_coverage_complete') is not True):
        raise coverage.Invalid('actual source-backed boot-only UART publisher missing/wrong')
    image = next(x for x in coverage.fv_images(fv.read_bytes()) if x['guid'] == PUBLISHER_GUID)
    pe = image['pe']
    # The validation routine may itself contain the 16-byte magic literal.
    # Uniqueness concerns the complete, CRC-bound 64-byte live build stamp.
    if pe.count(coverage.build_stamp(payload)) != 1:
        raise coverage.Invalid('actual publisher PE has missing/ambiguous/stale header stamp')
    if payload not in pe:
        raise coverage.Invalid('actual publisher PE lacks exact complete coverage payload')
    return rows[0]['pe_sha256']


def execute(phase, workspace, output, build=None, fv=None, nm='llvm-nm', previous=None):
    workspace = workspace.resolve(strict=True)
    header = workspace/HEADER
    if (not header.parent.is_dir() or header.is_symlink() or
            not header.resolve().is_relative_to(workspace)):
        raise coverage.Invalid('expected real UART-specific source header directory')
    output = output.resolve()
    # Receipt names never alias the native input or build evidence.
    source_alias = (output.is_relative_to(workspace) and
                    output.relative_to(workspace).as_posix() in source_snapshot(workspace))
    if (output.suffix != '.json' or output.is_symlink() or output == header or
            (previous and output == previous.resolve()) or source_alias):
        raise coverage.Invalid('receipt must be a separate JSON output')
    if build and output.is_relative_to(build.resolve()):
        raise coverage.Invalid('receipt must be separate from native/build inputs')
    result = {'schema': 1, 'phase': phase, 'scope': SCOPE, 'workspace': str(workspace),
              'tool_sha256': sha(Path(__file__).read_bytes()),
              'coverage_tool_sha256': sha((HERE/'check_uart_firmware_coverage.py').read_bytes()),
              'success': False, 'package_eligible': False, 'errors': []}
    try:
        if phase == 'bootstrap':
            payload = coverage.coverage_bytes()
            text = coverage.coverage_header(payload, bootstrap=True)
            header.write_text(text)
            result.update(success=True, header_sha256=sha(text.encode()),
                          coverage_hex=payload.hex(), source_snapshot=source_snapshot(workspace))
        elif phase == 'verify':
            if previous is None:
                raise coverage.Invalid('validated receipt required')
            prior = load_receipt(previous.resolve(strict=True), 'validate', workspace)
            if not prior.get('package_eligible') or prior['source_snapshot'] != source_snapshot(workspace):
                raise coverage.Invalid('validated UART source snapshot no longer matches')
            if sha(header.read_bytes()) != prior['header_sha256']:
                raise coverage.Invalid('validated header changed')
            for name, digest in prior['inventory']['inputs'].items():
                if sha(Path(name).read_bytes()) != digest:
                    raise coverage.Invalid('validated build input changed: ' + name)
            result.update(success=True, package_eligible=True,
                          previous_sha256=sha(previous.read_bytes()),
                          validated_fv=prior['fv'], validated_fv_sha256=prior['inventory']['inputs'][prior['fv']])
        else:
            if build is None or fv is None or previous is None:
                raise coverage.Invalid('actual build/FV and previous phase receipt required')
            prior = load_receipt(previous.resolve(strict=True),
                                 'bootstrap' if phase == 'inventory' else 'inventory', workspace)
            if prior['source_snapshot'] != source_snapshot(workspace):
                raise coverage.Invalid('UART participant sources changed since previous phase')
            result.update(previous_sha256=sha(previous.read_bytes()), build=str(build.resolve()),
                          fv=str(fv.resolve()))
            current = checked_build(workspace, build, fv, nm)
            current_payload = coverage.coverage_bytes(current)
            expected_payload = bytes.fromhex(prior['coverage_hex'])
            if phase == 'inventory' and expected_payload != coverage.coverage_bytes():
                raise coverage.Invalid('first build must use the invalid Count0 bootstrap')
            if header.read_bytes() != coverage.coverage_header(
                    expected_payload, bootstrap=phase == 'inventory').encode():
                raise coverage.Invalid('native header is not the exact preceding phase input')
            publisher_sha = publisher_stamp(workspace, fv, current, expected_payload)
            if phase == 'validate' and current_payload != expected_payload:
                raise coverage.Invalid('second-build runtime PE/archive manifest is not byte-identical')
            text = coverage.generated_header(current)
            if phase == 'inventory':
                # No mutation until ALL first-build obligations passed.
                header.write_text(text)
            elif header.read_bytes() != text.encode():
                raise coverage.Invalid('second-build regenerated header differs from native input')
            result.update(success=True, package_eligible=phase == 'validate',
                          header_sha256=sha(text.encode()), coverage_hex=current_payload.hex(),
                          runtime_coverage_sha256=current['runtime_coverage_sha256'],
                          publisher_pe_sha256=publisher_sha, inventory=current,
                          source_snapshot=source_snapshot(workspace))
        # Rechecking inputs gives the packaging caller exact content identities.
        # It must package only this FV/these participants; any changed input
        # requires Validate again. A success is not proof of real EBS retirement.
    except (coverage.Invalid, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        result['errors'] = [str(error)]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('phase', choices=('bootstrap', 'inventory', 'validate', 'verify'))
    p.add_argument('--workspace', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--previous', type=Path)
    p.add_argument('--build', type=Path)
    p.add_argument('--fv', type=Path)
    p.add_argument('--nm', default='llvm-nm')
    args = p.parse_args()
    try:
        result = execute(args.phase, args.workspace, args.output, args.build, args.fv,
                         args.nm, args.previous)
    except (coverage.Invalid, OSError, ValueError) as error:
        print(json.dumps({'success': False, 'package_eligible': False, 'errors': [str(error)]}))
        return 1
    print(json.dumps({k: result[k] for k in ('phase', 'success', 'package_eligible', 'errors')}))
    return 0 if result['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
