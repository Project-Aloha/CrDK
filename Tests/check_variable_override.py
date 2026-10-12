#!/usr/bin/env python3
"""Validate the staged VariableRuntimeDxe source closure and recorded relocation."""
import hashlib
import json
from pathlib import Path
import re

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
module = crane/'Override/MU_BASECORE/MdeModulePkg/Universal/Variable/RuntimeDxe'
inventory = json.loads((module/'SOURCE_INVENTORY.json').read_text())
assert inventory['upstream_revision'] == '562c0927ef7bfc65d26d9caf322e569ccb23b6a9'
for item in inventory['files']:
    file = root/item['staged_path']
    assert hashlib.sha256(file.read_bytes()).hexdigest() == item['staged_sha256'], str(file)
inf = (module/'VariableRuntimeDxe.inf').read_text()
assert 'FILE_GUID                      = CBD2E4D5-7068-4FF5-B462-9822B4AD8D60' in inf
assert 'BASE_NAME                      = VariableRuntimeDxe' in inf
assert 'CranePkg/CranePkg.dec' in inf
section, closure = None, set()
for line in inf.splitlines():
    text = line.split('#', 1)[0].strip()
    if text.startswith('['):
        section = text
    elif text and section in ('[Sources]', '[UserExtensions.TianoCore."ExtraFiles"]'):
        closure.add(text)
closure |= {'VariableRuntimeDxe.inf', 'VariableRuntimeDxe.uni'}
assert len(closure) == 23
for name in closure:
    file = module/name
    assert file.is_file(), name
    if file.suffix in ('.c', '.h'):
        for include in re.findall(r'^\s*#\s*include\s*"([^"]+)"', file.read_text(), re.M):
            assert include in closure, (name, include)
            assert not include.endswith('.c'), (name, include)
        assert 'SPDX-License-Identifier: BSD-2-Clause-Patent' in file.read_text(), name
print('VariableRuntimeDxe: 23-file source/UNI closure, exact inventory, GUID/name, local includes and licenses PASS')
