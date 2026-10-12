#!/usr/bin/env python3
"""Actual async remap/monitor/update functions with modeled PCI DMA ownership."""
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
p.add_argument('--expect-before-failure', action='store_true')
a = p.parse_args()
source = a.source.read_text()
functions = []
for name in ['XhcUpdateAsyncRequest', 'XhcFlushAsyncIntMap', 'XhcMonitorAsyncRequests']:
    m = re.search(r'^(?:EFI_STATUS|VOID)\n(?:EFIAPI\n)?' + name + r' \([^;{]*\)\n\{', source, re.M)
    assert m, name
    end, depth = m.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    functions.append(source[m.start():end])

freeze_source=(xhci/'Xhci.c').read_text()
m=re.search(r'^STATIC EFI_STATUS EFIAPI\nXhcFreeze \([^;{]*\)\n\{',freeze_source,re.M)
assert m
end,depth=m.end(),1
while depth:
    depth+=(freeze_source[end]=='{')-(freeze_source[end]=='}');end+=1
functions.insert(0,freeze_source[m.start():end])

def run(out):
    out.mkdir(parents=True, exist_ok=True)
    (out / 'XhciAsyncMapProduction.c').write_text('\n\n'.join(functions))
    command = ['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-DMDEPKG_NDEBUG', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', *['-I' + str(path) for path in
               [out, xhci, root / 'MU_BASECORE/MdePkg/Include',
                root / 'MU_BASECORE/MdePkg/Include/X64', root / 'Platforms/CranePkg/Include', root / 'MU_BASECORE/MdeModulePkg/Include']],
               str(crane / 'Tests/XhciAsyncMapTest.c'), '-o', str(out / 'test')]
    subprocess.run(command, check=True)
    results = []
    for case in ['refresh', 'monitor']:
        r = subprocess.run([str(out / 'test'), case], text=True, capture_output=True)
        (out / (case + '.log')).write_text(r.stdout + r.stderr)
        if a.expect_before_failure:
            assert r.returncode != 0 and 'Assertion' in r.stderr, r
        else:
            r.check_returncode()
        results.append({'case': case, 'returncode': r.returncode})
        print(case, 'expected old failure' if a.expect_before_failure else 'PASS')
    inputs = [a.source, xhci / 'Xhci.c', root / 'Platforms/CranePkg/Include/Protocol/Usb2HcFreeze.h', xhci / 'Xhci.h', xhci / 'XhciSched.h',
              crane / 'Tests/XhciAsyncMapTest.c', Path(__file__)]
    (out / 'artifacts.json').write_text(json.dumps({
        'passed': True, 'expected_before_failure': a.expect_before_failure,
        'command': command, 'results': results,
        'boundary': 'Actual remap, monitor and update; PCI DMA, event completion, TRB builder and boot services modeled. No physical keyboard claim.',
        'inputs': {str(f.resolve()): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    }, indent=2) + '\n')

if a.output:
    run(a.output.resolve())
else:
    with tempfile.TemporaryDirectory(prefix='xhci-async-map-') as tmp:
        run(Path(tmp))
