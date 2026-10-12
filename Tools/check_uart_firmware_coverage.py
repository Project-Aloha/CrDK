#!/usr/bin/env python3
"""Verify linked SerialPortLib coverage of the actual uncompressed native FV.

This proves library selection, not arbitrary source-code absence of MMIO or a
Windows grant. The native producer must also check the *loaded* runtime image
set and complete real UART retirement. Unknown/compressed/binary-only code is
never silently accepted. Current ungated HDK firmware intentionally fails.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import uuid
import zlib
from pathlib import Path

SERIAL = frozenset(('SerialPortInitialize', 'SerialPortWrite', 'SerialPortRead',
                    'SerialPortPoll', 'SerialPortSetAttributes',
                    'SerialPortGetControl', 'SerialPortSetControl'))
GATED_DXE = 'Platforms/CranePkg/Library/QcomGeniGatedSerialPortLib/QcomGeniGatedSerialPortLib.inf'
GATED_RUNTIME = 'Platforms/CranePkg/Library/QcomGeniGatedSerialPortLib/QcomGeniGatedSerialPortLibRuntime.inf'
NULL = 'MU_BASECORE/MdePkg/Library/BaseSerialPortLibNull/BaseSerialPortLibNull.inf'
PRE_DXE = frozenset(('SEC', 'PEI_CORE', 'PEIM'))
CODE_TYPES = frozenset((3, 4, 5, 6, 7, 8, 9))
GUID_RE = r'[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}'
BUILD_STAMP_MAGIC = b'CRUARTBUILDV1\0\0\0'


class Invalid(ValueError):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def under(path, root):
    path = Path(path)
    if not path.is_absolute():
        raise Invalid(f'expected absolute generated path: {path}')
    path = path.resolve(strict=True)
    if not path.is_relative_to(root):
        raise Invalid(f'generated path escapes root: {path}')
    return path


def metadata(path):
    text = path.read_text()
    out = {}
    for key in ('MODULE_NAME', 'MODULE_GUID', 'MODULE_TYPE', 'MODULE_DIR',
                'MODULE_FILE', 'MODULE_BUILD_DIR', 'CC_FLAGS'):
        values = re.findall(r'^' + key + r'\s*=\s*(.*?)\s*$', text, re.M)
        if not values or len(set(values)) != 1:
            raise Invalid(f'{path}: absent/conflicting {key}')
        out[key] = values[0]
    try:
        out['MODULE_GUID'] = str(uuid.UUID(out['MODULE_GUID']))
    except ValueError as e:
        raise Invalid(f'{path}: invalid module GUID') from e
    return out


def pe_subsystem(data):
    if len(data) < 64 or data[:2] != b'MZ':
        raise Invalid('PE section is not a bounded DOS/PE image')
    pe = struct.unpack_from('<I', data, 60)[0]
    if pe > len(data) - 24 or data[pe:pe + 4] != b'PE\0\0':
        raise Invalid('invalid PE signature/offset')
    machine, _, _, _, _, size, _ = struct.unpack_from('<HHIIIHH', data, pe + 4)
    if machine != 0xaa64 or size < 70 or pe + 24 + size > len(data):
        raise Invalid('image is not bounded ARM64 PE32+')
    if struct.unpack_from('<H', data, pe + 24)[0] != 0x20b:
        raise Invalid('image is not PE32+')
    sub = struct.unpack_from('<H', data, pe + 24 + 68)[0]
    if sub not in (10, 11, 12):
        raise Invalid(f'non-UEFI subsystem {sub}')
    return sub


def fv_images(data):
    if len(data) < 56 or data[40:44] != b'_FVH':
        raise Invalid('invalid FV header')
    length = struct.unpack_from('<Q', data, 32)[0]
    header = struct.unpack_from('<H', data, 48)[0]
    if length != len(data) or header < 56 or header & 1 or header > length:
        raise Invalid('invalid FV length/header')
    if sum(struct.unpack_from('<' + 'H' * (header // 2), data)) & 0xffff:
        raise Invalid('FV header checksum')
    images, seen = [], set()
    offset = (header + 7) & ~7
    while offset + 24 <= length:
        if data[offset:offset + 24] == b'\xff' * 24:
            if any(v != 0xff for v in data[offset:]):
                raise Invalid('non-erased trailing FV bytes')
            break
        guid = str(uuid.UUID(bytes_le=data[offset:offset + 16]))
        kind, attrs = data[offset + 18], data[offset + 19]
        size = int.from_bytes(data[offset + 20:offset + 23], 'little')
        if size < 24 or size == 0xffffff or attrs & 1 or size > length - offset:
            raise Invalid('unsupported/invalid FFS size')
        end = offset + size
        if kind in CODE_TYPES:
            if guid in seen:
                raise Invalid(f'duplicate code FFS GUID {guid}')
            seen.add(guid)
            pos, pe = offset + 24, []
            while pos < end:
                if end - pos < 4:
                    raise Invalid('truncated FFS section')
                count = int.from_bytes(data[pos:pos + 3], 'little')
                sec = data[pos + 3]
                if count < 4 or count == 0xffffff or count > end - pos:
                    raise Invalid('unsupported/invalid section size')
                if sec in (1, 2, 0x17):
                    raise Invalid(f'compressed/guided/nested executable FFS {guid}')
                if sec == 0x10:
                    payload = data[pos + 4:pos + count]
                    pe.append((payload, pe_subsystem(payload)))
                elif sec == 0x12:
                    raise Invalid(f'TE image needs an explicit verifier: {guid}')
                pos = (pos + count + 3) & ~3
            if len(pe) != 1:
                raise Invalid(f'code FFS must contain exactly one PE: {guid}')
            images.append({'guid': guid, 'type': kind, 'offset': offset,
                           'pe': pe[0][0], 'subsystem': pe[0][1]})
        offset = (end + 7) & ~7
    if not images or offset > length:
        raise Invalid('empty/truncated executable FV inventory')
    return images


def defined_symbols(archive, nm):
    result = subprocess.run([nm, '--defined-only', '--extern-only', str(archive)],
                            check=True, capture_output=True, text=True)
    return {line.split()[-1] for line in result.stdout.splitlines()
            if re.search(r'\s[A-Za-z]\s+\S+\s*$', line)}


def check(workspace, build, fv, nm='llvm-nm'):
    workspace, build, fv = (x.resolve(strict=True) for x in (workspace, build, fv))
    if not build.is_relative_to(workspace) or not fv.is_relative_to(build):
        raise Invalid('build/FV must belong to the selected workspace')
    evidence = {}

    def read(path):
        value = path.read_bytes()
        evidence[str(path)] = sha(value)
        return value

    raw = read(fv)
    images = fv_images(raw)
    by_guid = {}
    for make in sorted((build / 'AARCH64').rglob('GNUmakefile')):
        m = metadata(make)
        d = under(m['MODULE_DIR'], workspace)
        source = under(d / m['MODULE_FILE'], workspace)
        if source.parent != d or not source.name.endswith('.inf'):
            raise Invalid(f'invalid generated source identity: {source}')
        directory = under(m['MODULE_BUILD_DIR'], build)
        if make.parent.resolve() != directory:
            raise Invalid(f'inconsistent generated build directory: {make}')
        row = (make, m, source)
        by_guid.setdefault(m['MODULE_GUID'], []).append(row)

    archive_cache, rows, errors = {}, [], []
    for image in images:
        guid = image['guid']
        row = {'guid': guid, 'subsystem': image['subsystem'],
               'ffs_offset': image['offset'], 'pe_sha256': sha(image['pe']),
               'pe_bytes': len(image['pe']), 'pe_crc32': zlib.crc32(image['pe'])}
        try:
            candidates = by_guid.get(guid, [])
            if not candidates and image['subsystem'] != 12:
                # A boot-only PE has no post-EBS execution lifetime. Do not
                # claim its unknown static libraries were gated; preserve
                # this exclusion explicitly, and require the producer to
                # reject any loaded image whose actual CodeType is runtime
                # unless its GUID belongs to the qualified runtime manifest.
                row.update(serial_kind='binary boot-only; not inspected',
                           link_coverage_complete=False,
                           post_ebs_lifetime=False)
                rows.append(row)
                continue
            if len(candidates) != 1:
                raise Invalid(f'{guid}: expected one generated source, got {len(candidates)}')
            make, m, source = candidates[0]
            read(make); read(source)
            directory = make.parent
            efi = directory / 'DEBUG' / (m['MODULE_NAME'] + '.efi')
            if read(efi) != image['pe']:
                raise Invalid(f'{guid}: linked .efi differs from actual FFS PE')
            runtime = image['subsystem'] == 12
            if runtime != (m['MODULE_TYPE'] == 'DXE_RUNTIME_DRIVER'):
                raise Invalid(f'{guid}: runtime PE/generated type disagreement')
            listing = directory / 'OUTPUT/static_library_files.lst'
            lines = read(listing).decode().splitlines()
            if not lines or len(lines) != len(set(lines)):
                raise Invalid(f'{guid}: empty/duplicate static archive inventory')
            providers, selected = [], []
            for line in lines:
                lib = under(line, build)
                if lib.suffix != '.lib' or lib.parent.name != 'OUTPUT':
                    raise Invalid(f'{guid}: invalid selected archive {lib}')
                if lib not in archive_cache:
                    read(lib)
                    lmfile = lib.parent.parent / 'GNUmakefile'
                    lm = metadata(lmfile)
                    ls = under(Path(lm['MODULE_DIR']) / lm['MODULE_FILE'], workspace)
                    if under(lm['MODULE_BUILD_DIR'], build) != lib.parent.parent:
                        raise Invalid(f'invalid library generated identity {lib}')
                    read(lmfile); read(ls)
                    archive_cache[lib] = (defined_symbols(lib, nm), lm, ls)
                symbols, lm, ls = archive_cache[lib]
                selected.append({'archive': lib.relative_to(build).as_posix(),
                                 'archive_sha256': evidence[str(lib)],
                                 'source': ls.relative_to(workspace).as_posix(),
                                 'source_sha256': evidence[str(ls)]})
                if SERIAL & symbols:
                    if not SERIAL <= symbols:
                        raise Invalid(f'{guid}: partial public serial provider {lib}')
                    providers.append((lib, symbols, lm, ls))
            row.update(name=m['MODULE_NAME'], module_type=m['MODULE_TYPE'],
                       source=str(source), serial_kind='none',
                       selected_archives=selected)
            if len(providers) > 1:
                raise Invalid(f'{guid}: multiple public serial providers')
            if providers:
                lib, symbols, lm, ls = providers[0]
                identity = ls.relative_to(workspace).as_posix()
                row.update(serial_archive=str(lib), serial_source=identity)
                if m['MODULE_TYPE'] in PRE_DXE:
                    row['serial_kind'] = 'pre-DXE; does not survive'
                elif identity == NULL:
                    row['serial_kind'] = 'null'
                else:
                    expected = GATED_RUNTIME if runtime else GATED_DXE
                    if identity != expected:
                        raise Invalid(f'{guid}: serial provider is ungated/wrong variant: {identity}')
                    required = {'CrUartSerialConstructor', 'CrUartSerialDestructor'}
                    if not required <= symbols:
                        raise Invalid(f'{guid}: gated archive lacks constructor/destructor')
                    auto = read(directory / 'DEBUG/AutoGen.c').decode()
                    for method in required:
                        if not re.search(r'\b' + method + r'\s*\(\s*ImageHandle\s*,\s*SystemTable\s*\)', auto):
                            raise Invalid(f'{guid}: actual AutoGen omitted {method} invocation')
                    constructor = re.search(r'\bCrUartSerialConstructor\s*\(\s*ImageHandle\s*,\s*SystemTable\s*\)', auto)
                    debug = re.search(r'\bBaseDebugLibSerialPortConstructor\s*\(\s*ImageHandle\s*,\s*SystemTable\s*\)', auto)
                    if debug and debug.start() < constructor.start():
                        raise Invalid(f'{guid}: DebugLib constructor precedes serial admission')
                    flag = bool(re.search(r'(?:-D|/D)\s*CR_UART_RUNTIME_COPY=1(?:\s|$)', lm['CC_FLAGS']))
                    if flag != runtime:
                        raise Invalid(f'{guid}: actual serial archive runtime define disagrees')
                    row['serial_kind'] = 'gated-runtime' if runtime else 'gated-boot'
            row['link_coverage_complete'] = True
            row['post_ebs_lifetime'] = runtime
        except (Invalid, OSError, subprocess.SubprocessError, UnicodeError) as e:
            row.update(link_coverage_complete=False, error=str(e))
            errors.append(str(e))
        rows.append(row)
    runtime_rows = [x for x in rows if x['subsystem'] == 12]
    stable_runtime = [{k: v for k, v in x.items() if k not in
                       ('ffs_offset', 'source', 'serial_archive')}
                      for x in runtime_rows]
    canonical = json.dumps({'images': rows, 'inputs': evidence}, sort_keys=True,
                           separators=(',', ':')).encode()
    return {'schema': 1, 'scope': 'actual FV + ARM64 PE + generated selected '
            'archives; separate source writer audit/loaded runtime set/real '
            'retirement/reserved-record and OS grant remain required',
            'link_coverage_complete': not errors, 'errors': errors, 'images': rows,
            'binary_boot_exclusions': [x for x in rows if x.get('post_ebs_lifetime') is False
                                       and not x['link_coverage_complete']],
            'runtime_images': runtime_rows,
            'runtime_coverage_sha256': sha(json.dumps(stable_runtime, sort_keys=True,
                                            separators=(',', ':')).encode()),
            'inputs': evidence, 'coverage_sha256': sha(canonical)}


def coverage_bytes(result=None):
    """Exact data-only C layout; None is deliberately invalid bootstrap data."""
    if result is None:
        return struct.pack('<II', 1, 0) + bytes(32 + 32 * 32)
    if not result['link_coverage_complete']:
        raise Invalid('cannot generate native manifest from incomplete link coverage')
    images = result['runtime_images']
    if not 0 < len(images) <= 32:
        raise Invalid('native runtime manifest capacity/empty inventory')
    kinds = {'none': 0, 'null': 1, 'gated-runtime': 2}
    images = sorted(images, key=lambda x: uuid.UUID(x['guid']).bytes_le)
    digest = bytes.fromhex(result['runtime_coverage_sha256'])
    payload = struct.pack('<II', 1, len(images)) + digest
    for row in images:
        try:
            kind = kinds[row['serial_kind']]
        except KeyError as e:
            raise Invalid('runtime provider lacks supported link evidence') from e
        payload += uuid.UUID(row['guid']).bytes_le + struct.pack(
            '<IIII', kind, row['pe_bytes'], row['pe_crc32'], 0)
    return payload + bytes((32 - len(images)) * 32)


def build_stamp(payload):
    if len(payload) != 1064:
        raise Invalid('coverage layout mismatch')
    return BUILD_STAMP_MAGIC + payload[:40] + struct.pack('<II', zlib.crc32(payload), 0)


def coverage_header(payload, bootstrap=False):
    revision, count = struct.unpack_from('<II', payload)
    if len(payload) != 1064 or revision != 1 or count > 32:
        raise Invalid('coverage layout mismatch')
    stamp = build_stamp(payload)
    lines = ['/* Generated UART manifest input. Do not edit. SPDX-License-Identifier: MIT */',
             '/* Invalid bootstrap: no native grant. */' if bootstrap else
             '/* Qualified link inventory only; native retirement and OS grants are separate. */',
             '#pragma once', '#include <Interface/CrUartHandoff.h>',
             '/* Volatile reads by the actual publisher keep this build identity in its PE. */',
             'STATIC CONST volatile UINT8 mUartCoverageBuildStamp[64] = {',
             '  ' + ','.join(str(v) for v in stamp), '};',
             'STATIC CONST CR_UART_FIRMWARE_COVERAGE mUartFirmwareCoverage = {',
             '  1, ' + str(count) + ',',
             '  {' + ','.join(str(v) for v in payload[8:40]) + '},', '  {']
    for offset in range(40, 1064, 32):
        entry = payload[offset:offset+32]
        lines.append('    {{' + ','.join(str(v) for v in entry[:16])
                     + '},' + ','.join(str(v) for v in struct.unpack_from('<IIII', entry, 16)) + '},')
    lines += ['  }', '};', '']
    return '\n'.join(lines)


def generated_header(result):
    return coverage_header(coverage_bytes(result))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--workspace', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--fv', type=Path, required=True)
    p.add_argument('--nm', default='llvm-nm')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--header', type=Path,
                   help='optional generated native input; removed on failure')
    args = p.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = check(args.workspace, args.build, args.fv, args.nm)
        if args.header and result['link_coverage_complete']:
            header = generated_header(result)
            args.header.parent.mkdir(parents=True, exist_ok=True)
            args.header.write_text(header)
    except (Invalid, OSError, subprocess.SubprocessError, UnicodeError) as e:
        result = {'schema': 1, 'link_coverage_complete': False, 'errors': [str(e)]}
    if args.header and not result['link_coverage_complete']:
        args.header.unlink(missing_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'link_coverage_complete': result['link_coverage_complete'],
                      'images': len(result.get('images', [])),
                      'runtime_images': len(result.get('runtime_images', [])),
                      'errors': len(result['errors']), 'receipt': str(args.output)}))
    return 0 if result['link_coverage_complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
