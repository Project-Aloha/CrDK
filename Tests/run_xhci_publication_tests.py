#!/usr/bin/env python3
"""Exercise actual TRB construction with a modeled weak DMA observer."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
xhci = root / 'Platforms/CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Pci/XhciDxe'
p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=xhci / 'XhciSched.c')
p.add_argument('--output', type=Path)
p.add_argument('--expect-order-failure', action='store_true')
a = p.parse_args()
source = a.source.read_text()
names = ['XhcPublishTrb', 'XhcCreateCmdTrb', 'XhcCreateTransferTrb',
         'XhcSyncTrsRing', 'XhcBusDevAddrToSlotId', 'XhcEndpointToDci']
functions = []
for name in names:
    match = re.search(r'^(?:STATIC\n)?(?:EFI_STATUS|UINT8|VOID|URB \*)\n(?:EFIAPI\n)?' + name + r' \([^;{]*\)\n\{', source, re.M)
    if name == 'XhcPublishTrb' and not match:
        continue
    assert match, name
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    body = source[match.start():end]
    if name == 'XhcSyncTrsRing':
        body = body.replace('XhcSyncTrsRing (', 'ActualXhcSyncTrsRing (', 1)
    functions.append(body)

def run(out):
    out.mkdir(parents=True, exist_ok=True)
    (out / 'XhciPublicationProduction.c').write_text('\n\n'.join(functions))
    command = ['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-DMDEPKG_NDEBUG', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', *['-I' + str(path) for path in
               [out, xhci, root / 'MU_BASECORE/MdePkg/Include',
                root / 'MU_BASECORE/MdePkg/Include/X64', root / 'Platforms/CranePkg/Include', root / 'MU_BASECORE/MdeModulePkg/Include']],
               str(crane / 'Tests/XhciPublicationTest.c'), '-o', str(out / 'test')]
    subprocess.run(command, check=True)
    result = subprocess.run([str(out / 'test')], text=True, capture_output=True)
    print(result.stdout + result.stderr, end='')
    (out / 'run.log').write_text(result.stdout + result.stderr)
    if a.expect_order_failure:
        assert result.returncode != 0 and 'Visible[I]' in result.stderr, result
    else:
        result.check_returncode()
    inputs = [a.source, xhci / 'XhciSched.h', xhci / 'Xhci.h',
              crane / 'Tests/XhciPublicationTest.c', Path(__file__)]
    (out / 'artifacts.json').write_text(json.dumps({
        'passed': True, 'expected_before_failure': a.expect_order_failure,
        'sanitizers': ['address', 'undefined'], 'command': command,
        'boundary': 'weak DMA visibility and PCI map are modeled; production builder and ring scan are actual',
        'inputs': {str(f.resolve()): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    }, indent=2) + '\n')

if a.output:
    run(a.output.resolve())
else:
    with tempfile.TemporaryDirectory(prefix='xhci-publication-') as tmp:
        run(Path(tmp))
