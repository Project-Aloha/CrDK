#!/usr/bin/env python3
"""Exercise platform packaging gates against actual compiled ARM64 fixtures."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import lzma
import runpy
import shutil
import sys
import struct
import uuid
from types import SimpleNamespace

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
output = a.output.resolve()
source = root/'Platforms/WaipioCrPkg/UartHandoffBuild.py'
spec = importlib.util.spec_from_file_location('uart_build', source)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)

# Reuse the existing independently compiled PE/archive/FV fixture producers.
# Their hostile pipeline cases also run, rather than substituting a fake
# successful validator for the platform's new packaging decision.
old = sys.argv
sys.argv = ['run_uart_pipeline_tests.py', '--output', str(output/'pipeline')]
try:
    fixtures = runpy.run_path(str(root/'Platforms/CranePkg/Tests/run_uart_pipeline_tests.py'))
finally:
    sys.argv = old
workspace, build, fv, *_ = fixtures['setup']('platform-build')
fixtures['participants'](workspace)
tools = workspace/'Platforms/CranePkg/Tools'
tools.mkdir(parents=True)
for name in ('uart_handoff_pipeline.py', 'check_uart_firmware_coverage.py'):
    shutil.copy2(root/'Platforms/CranePkg/Tools'/name, tools/name)


class Env(dict):
    def GetValue(self, key, default=None):
        return self.get(key, default)

    def SetValue(self, key, value, reason):
        self[key] = value


class Builder:
    def __init__(self, enabled='1', phase='inventory'):
        self.env = Env(TARGET_DEVICE='qcom-hdk8450', TARGET='DEBUG',
                       ACTIVE_PLATFORM='WaipioCrPkg/WaipioCrNoSb.dsc',
                       CR_HDK_UART_HANDOFF=enabled, CR_UART_HANDOFF_PHASE=phase)

    def GetWorkspaceRoot(self):
        return str(workspace)

    def GetOutputBinDirectory(self):
        return str(build)

    def GetOutputDirectory(self):
        return str(build.parent)


cases = []
def reject(name, action):
    try:
        action()
    except (ValueError, FileNotFoundError):
        cases.append(name)
    else:
        raise AssertionError('unexpected acceptance: '+name)


def fixture_fd(main, duplicate=False):
    # Independent container writer around the actual ARM64 fixture FV. Match
    # the platform FDF's FV_IMAGE/LZMA GUID, including GenFv's RAW alignment.
    inner = b'\x04\0\0\x19'+(len(main)+4).to_bytes(3, 'little')+b'\x17'+main
    if duplicate:
        inner += (len(main)+4).to_bytes(3, 'little')+b'\x17'+main
    encoded = lzma.compress(inner, format=lzma.FORMAT_ALONE)
    guided = (len(encoded)+24).to_bytes(3, 'little')+b'\x02'
    guided += uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le
    guided += b'\x18\0\x01\0'+encoded
    file = uuid.UUID('9e21fd93-9c72-4c15-8c4b-e77f1db2d792').bytes_le
    file += b'\0\0\x0b\0'+(len(guided)+24).to_bytes(3, 'little')+b'\xf8'+guided
    data = bytearray(fixtures['fv'](b'ignored')[:72])
    data += file
    data += b'\xff'*(0x400000-len(data))
    struct.pack_into('<Q', data, 32, len(data))
    struct.pack_into('<H', data, 50, 0)
    struct.pack_into('<H', data, 50, -sum(struct.unpack('<36H', data[:72]))&0xffff)
    return bytes(data)


off = Builder('0', '')
helper.configure(off)
helper.prepare(off)
assert helper.finish(off) is True
bad = Builder(); bad.env['TARGET_DEVICE'] = 'qcom-mtp8450'
reject('other board', lambda: helper.configure(bad))
reject('invalid flag', lambda: helper.configure(Builder('2')))
reject('phase without flag', lambda: helper.configure(Builder('0')))
reject('missing phase', lambda: helper.configure(Builder('1', '')))
bad = Builder()
bad.env['BLD_*_CR_HDK_UART_HANDOFF'] = '0'
bad.env.SetValue = lambda *args: None
reject('conflicting compiler macro', lambda: helper.configure(bad))

first = Builder()
helper.configure(first)
helper.prepare(first)
fixtures['publisher'](workspace, build)
assert helper.finish(first) is False  # First FV can never reach packaging.
header = workspace/fixtures['pipeline'].HEADER
candidate = header.read_bytes()
second = Builder(phase='validate')
helper.configure(second)
header.write_bytes(candidate+b'\n')
reject('changed header before build', lambda: helper.prepare(second))
header.write_bytes(candidate)
helper.prepare(second)
reject('stale first-build publisher', lambda: helper.finish(second))
fixtures['publisher'](workspace, build)
fd = build/'FV/SM8450CR_EFI.fd'
reject('missing FD after valid FV', lambda: helper.finish(second))
fd.write_bytes(b'STALE UNRELATED FD')
reject('unrelated FD after valid FV', lambda: helper.finish(second))
good = fixture_fd(fv.read_bytes())
fd.write_bytes(good[:-1])
reject('truncated FD', lambda: helper.finish(second))
foreign = bytearray(fv.read_bytes()); foreign[-1] ^= 1
fd.write_bytes(fixture_fd(bytes(foreign)))
reject('different compressed FV', lambda: helper.finish(second))
fd.write_bytes(fixture_fd(fv.read_bytes(), duplicate=True))
reject('multiple decoded FV images', lambda: helper.finish(second))
fd.write_bytes(good)
assert helper.finish(second) is True
_, _, receipts = helper.context(second)
assert json.loads((receipts/'verify.json').read_text())['package_eligible'] is True

for prefix in ('qcom-hdk8450', 'qcom-hdk8450-windows-cr'):
    app = SimpleNamespace(output_path='/output/'+prefix+'.img',
                          bootpayload_path='/output/'+prefix+'.bin')
    helper.select_image(second, app)
    assert Path(app.fd_path).read_bytes() == good
    assert app.output_path == '/output/'+prefix+'-uart-handoff.img'
    assert app.bootpayload_path == '/output/'+prefix+'-uart-handoff.bin'
helper.select_image(off, app)
assert app.output_path.endswith('-uart-handoff.img')
# Replacing the live build FD cannot change the already selected package input.
fd.write_bytes(b'replaced by another build')
assert Path(app.fd_path).read_bytes() == good

output.mkdir(parents=True, exist_ok=True)
(output/'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'rejected': cases,
    'scope': 'actual ARM64 fixture pipeline plus platform packaging selection; no hardware grant',
    'inputs': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
               for path in (source, Path(__file__))},
    'final_pipeline_receipt': str(receipts/'verify.json'),
}, indent=2)+'\n')
print('UART platform: disabled/default preserved, bootstrap unpackageable, stale candidate rejected, second FV verified')
