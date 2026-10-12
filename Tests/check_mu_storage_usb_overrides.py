#!/usr/bin/env python3
"""Verify storage/USB forks and recorded later changes against pinned MU sources."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import tempfile

CRANE = Path(__file__).resolve().parents[1]
ROOT = CRANE.parents[1]
MODULES = (
    'EmbeddedPkg/Universal/MmcDxe',
    'MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe',
    'MdeModulePkg/Bus/Pci/NvmExpressDxe',
    'MdeModulePkg/Bus/Pci/XhciDxe',
    'MdeModulePkg/Bus/Scsi/ScsiDiskDxe',
    'MdeModulePkg/Bus/Ufs/UfsPassThruDxe',
    'MdeModulePkg/Bus/Usb/UsbBusDxe',
)

def digest(data):
    return hashlib.sha256(data).hexdigest()


def closure(directory, inf):
    """INF metadata/sources plus the transitive module-local quoted includes."""
    result = {inf.name}
    section = ''
    for line in inf.read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if line.startswith('['):
            section = line.lower()
        elif line:
            if section.startswith('[sources') or section.startswith('[userextensions.tianocore."extrafiles"'):
                result.add(line.split('|', 1)[0].strip())
            elif section == '[defines]' and line.startswith('MODULE_UNI_FILE'):
                result.add(line.split('=', 1)[1].strip())
    pending = list(result)
    while pending:
        name = pending.pop()
        path = directory / name
        assert path.is_file(), path
        assert path.suffix in ('.c', '.h', '.inf', '.uni'), path
        if path.suffix not in ('.c', '.h'):
            continue
        for included in re.findall(r'^\s*#include\s+"([^"]+)"', path.read_text(), re.M):
            assert not included.endswith('.c'), 'production source inclusion: ' + included
            assert (directory / included).is_file(), included
            if included not in result:
                result.add(included)
                pending.append(included)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-source', action='store_true', help='also require current official source to equal captured pre-migration bytes')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    records = []
    # Keep the reconstructed patch baseline outside the workspace: no duplicate
    # build descriptors may be discovered by Stuart while validating evidence.
    with tempfile.TemporaryDirectory(prefix='crane-storage-usb-provenance-') as tmp:
        baseline = Path(tmp)
        for module in MODULES:
            directory = CRANE / 'Override/MU_BASECORE' / module
            provenance = json.loads((directory / 'PROVENANCE.json').read_text())
            inf = directory / Path(provenance['original_module']).name
            files = {entry['file'] for entry in provenance['files']}
            assert files == closure(directory, inf), module
            assert files == {p.name for p in directory.iterdir() if p.suffix in ('.c', '.h', '.inf', '.uni')}, module
            text = inf.read_text()
            for key, field in [('BASE_NAME', 'base_name'), ('FILE_GUID', 'file_guid')]:
                assert re.search(key + r'\s*=\s*(\S+)', text)[1] == provenance[field]
            for entry in provenance['files']:
                path = directory / entry['file']
                data = path.read_bytes()
                assert digest(data) == entry['fork_sha256'], path
                # Git records executable bits, not group-write bits or umask.
                assert path.stat().st_mode & 0o111 == int(entry['fork_mode'], 8) & 0o111, path
                upstream_path = entry.get('upstream_path', module + '/' + entry['file'])
                old = subprocess.check_output(['git', '-C', str(ROOT / 'MU_BASECORE'), 'show',
                    provenance['upstream_commit'] + ':' + upstream_path])
                assert digest(old) == entry['upstream_sha256'], path
                original = baseline / module / entry['file']
                original.parent.mkdir(parents=True, exist_ok=True)
                if entry.get('added_file'):
                    # A new private header may derive from another upstream
                    # file; the recorded patch must create it from scratch.
                    assert entry.get('post_relocation_edit'), path
                    assert upstream_path != module + '/' + entry['file'], path
                    assert not subprocess.check_output(['git', '-C', str(ROOT / 'MU_BASECORE'),
                        'ls-tree', provenance['upstream_commit'], '--', module + '/' + entry['file']]), path
                else:
                    original.write_bytes(old)
                    original.chmod(int(entry['upstream_mode'], 8))
                captured_hash = entry.get('source_sha256', entry.get('captured_sha256'))
                if args.compare_source:
                    assert digest((ROOT / entry['source_path']).read_bytes()) == (captured_hash or entry['upstream_sha256']), entry['source_path']
                if entry['file'] == 'XhciDxe.inf':
                    data = data.replace(b'  CranePkg/CranePkg.dec\r\n', b'').replace(b'  CranePkg/CranePkg.dec\n', b'')
                    assert 'CranePkg/CranePkg.dec' in text
                else:
                    if path.suffix == '.inf':
                        assert 'CranePkg/CranePkg.dec' not in text
                if not entry.get('post_relocation_edit'):
                    assert digest(data) == captured_hash, path
            patch = directory / 'MU_BASECORE.patch'
            assert digest(patch.read_bytes()) == provenance['patch_sha256']
            subprocess.run(['git', 'apply', '--whitespace=nowarn', str(patch)], cwd=baseline, check=True)
            for entry in provenance['files']:
                expected_data = (directory / entry['file']).read_bytes()
                if entry['file'] == 'XhciDxe.inf':
                    expected_data = expected_data.replace(b'  CranePkg/CranePkg.dec\r\n', b'').replace(b'  CranePkg/CranePkg.dec\n', b'')
                assert (baseline / module / entry['file']).read_bytes() == expected_data, entry['file']
            records.append({'module':module, 'files':len(files), 'provenance_sha256':digest((directory / 'PROVENANCE.json').read_bytes()),
                            'patch_sha256':provenance['patch_sha256'], 'file_guid':provenance['file_guid']})
    header = CRANE / 'Include/Protocol/Usb2HcFreeze.h'
    expected = '67d217aa, 0x3d49, 0x4b80, { 0x81, 0xe2, 0xcb, 0xbd, 0xd9, 0x50, 0x4f, 0xe7 }'
    assert expected in header.read_text()
    if args.compare_source:
        assert header.read_bytes() == (ROOT / 'MU_BASECORE/MdeModulePkg/Include/Protocol/Usb2HcFreeze.h').read_bytes()
    result = {'passed':True, 'scope':'Exact minimal INF closure, identities, captured hashes and current baseline+patch reconstruction',
              'official_source_compared':args.compare_source, 'modules':records, 'freeze_header_sha256':digest(header.read_bytes())}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS:', len(records), 'storage/USB modules,', sum(r['files'] for r in records),
          'closure files, baseline+patch reconstruction, original identities and exact current C/header/UNI bytes')

if __name__ == '__main__':
    main()
