#!/usr/bin/env python3
"""Verify official MU source policy and explicit platform module identities."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir', type=Path, help='Also verify HDK8450 platform module PE payloads in its FV')
a = p.parse_args()
crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
policy = json.loads((crane / 'Override/OfficialSources.json').read_text())
for name, rule in policy['repositories'].items():
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root / name), *args])
    assert git('rev-parse', 'HEAD').decode().strip() == rule['revision'], name + ': upstream pin changed'
    changed = git('diff', 'HEAD', '--name-only').decode().splitlines()
    assert set(changed) <= set(rule['diagnostic_only']), name + ': unreviewed official changes'
    assert not git('ls-files', '--others', '--exclude-standard').strip(), name + ': private files in official tree'
    for path in changed:
        assert hashlib.sha256((root / name / path).read_bytes()).hexdigest() == rule['diagnostic_only'][path], path + ': diagnostic patch changed'

mapping = json.loads((crane / 'Override/Modules.json').read_text())['modules']
def defines(text):
    return dict(re.findall(r'^\s*(BASE_NAME|FILE_GUID|ENTRY_POINT|MODULE_TYPE|LIBRARY_CLASS)\s*=\s*([^#\r\n]+)', text, re.M))

images = None
if a.build_dir:
    spec = importlib.util.spec_from_file_location('mu_override_fv', crane / 'Tools/check_uart_firmware_coverage.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    images = m.fv_images((a.build_dir / 'FV/FVMAIN.Fv').read_bytes())
verified = 0
for original, replacement in mapping.items():
    target = root / 'Platforms' / replacement
    assert target.is_file(), replacement
    info = {k: v.strip() for k, v in defines(target.read_text()).items()}
    if original.startswith('MdeModulePkg/Library/VariablePersistenceLibNull/'):
        continue  # Private new library, never an upstream module.
    repository = 'Common/MU' if '/MU_PLUS/' in replacement else 'MU_BASECORE'
    upstream = {k: v.strip() for k, v in defines((root / repository / original).read_text()).items()}
    assert info == upstream, replacement + ': module identity differs'
    if images is not None and 'LIBRARY_CLASS' not in info and not original.startswith('EmbeddedPkg/SimpleTextInOutSerial/'):
        guid = info['FILE_GUID'].lower()
        if original == 'ShellPkg/Application/Shell/Shell.inf':
            # Frontpage packages Shell with the existing PcdShellFile GUID.
            dec = (root / 'Common/MU/PcBdsPkg/PcBdsPkg.dec').read_text()
            values = re.search(r'PcdShellFile\|\{([^}]+)\}', dec).group(1)
            guid = str(uuid.UUID(bytes_le=bytes(int(n.strip(), 16) for n in values.split(','))))
        rows = [r for r in images if r['guid'] == guid]
        assert len(rows) == 1, replacement + ': missing/duplicate FV module'
        # This output path binds the PE to the explicit new source module.
        source_output = a.build_dir / 'AARCH64' / Path(replacement).parent / Path(replacement).stem / 'OUTPUT' / (info['BASE_NAME'] + '.efi')
        assert rows[0]['pe'] == source_output.read_bytes(), replacement + ': FV PE differs from override output'
        verified += 1
print('Official MU functional sources match pinned upstream; only approved diagnostics remain.')
print('Verified', len(mapping) - 1, 'platform module identities;', verified, 'actual FV payloads.' if images is not None else 'FV verification not requested.')
