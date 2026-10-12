#!/usr/bin/env python3
"""Actual HDK8350 read-only implementation; independent public-source block oracle."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--clang', default='clang')
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
pkg = root / 'Platforms/CranePkg'
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
inputs = {}
commands = []

def read(path):
    path = path.resolve()
    data = path.read_bytes()
    inputs[str(path)] = hashlib.sha256(data).hexdigest()
    return data.decode()

catalog = read(root / 'linux/drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_7_0_sm8350.h')
android = read(root / 'Platforms/LahainaCrPkg/Device/qcom-hdk8350/DeviceTreeBlob/Android/android-hdk8350.dts')
linux = read(root / 'Platforms/LahainaCrPkg/Device/qcom-hdk8350/DeviceTreeBlob/Linux/linux-hdk8350.dts')

def blocks(name):
    section = catalog.split('sm8350_' + name + '[] = {', 1)[1].split('\n};', 1)[0]
    return [int(value, 16) for value in re.findall(r'\.base = (0x[0-9a-f]+)', section)]

def dt(prop):
    return [int(value, 0) - 0x1000 for value in re.search(re.escape(prop) + r'\s*=\s*<([^>]+)>', android)[1].split()]

ctls, sources, intfs, mixers = [blocks(name) for name in ('ctl', 'sspp', 'intf', 'lm')]
assert ctls == dt('qcom,sde-ctl-off')
assert sources == dt('qcom,sde-sspp-off')
assert intfs == dt('qcom,sde-intf-off')
assert mixers == dt('qcom,sde-mixer-off')
assert 'compatible = "lt,lt9611uxc"' in android
assert 'compatible = "lontium,lt9611uxc"' in linux

oracle = '/* Public Linux catalog values independently cross-checked with Android HDK DT. */\n'
oracle += 'static const UINT64 RefMdss=0xae00000, RefDpu=0xae01000;\n'
for name, values in [('Ctl', ctls), ('Source', sources), ('Intf', intfs), ('Lm', mixers)]:
    oracle += 'static const UINT32 Ref' + name + '[]={' + ','.join(hex(value) for value in values) + '};\n'
oracle += 'static const UINT64 RefDsi[]={0xae94000,0xae96000},RefPhy[]={0xae94400,0xae96400};\n'
(out / 'hdk8350-source-oracle.h').write_text(oracle)
source = pkg / 'Library/Hdk8350BootDisplayCrLib/Hdk8350BootDisplay.c'
test = pkg / 'Tests/Hdk8350BootDisplayTest.c'
for path in (source, test, Path(__file__), pkg / 'Include/Library/Hdk8350BootDisplayCrLib.h'):
    read(path)

def run(command, filename):
    commands.append([str(value) for value in command])
    result = subprocess.run(command, text=True, capture_output=True, timeout=90)
    (out / filename).write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout + result.stderr, end='')
        raise SystemExit(result.returncode)
    return result

flags = [args.clang, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fshort-wchar', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-I' + str(pkg / 'Include'), '-I' + str(root / 'MU_BASECORE/MdePkg/Include'),
         '-I' + str(root / 'MU_BASECORE/MdePkg/Include/X64'), '-I' + str(out)]
run([*flags, source, test, '-o', out / 'host-debug'], 'host-debug-compile.log')
result = run([out / 'host-debug'], 'host-debug.log')
print(result.stdout, end='')
arm_flags = [args.clang, '--target=aarch64-none-elf', '-ffreestanding', '-std=c11',
             '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fshort-wchar',
             '-I' + str(pkg / 'Include'), '-I' + str(root / 'MU_BASECORE/MdePkg/Include'),
             '-I' + str(root / 'MU_BASECORE/MdePkg/Include/AArch64')]
run([*arm_flags, '-MMD', '-MF', out / 'efi-arm64-debug.d', '-c', source,
     '-o', out / 'efi-arm64-debug.o'], 'efi-arm64-debug-compile.log')
for dependency in (out / 'efi-arm64-debug.d').read_text().split(':', 1)[1].replace('\\\n', '').split():
    read(Path(dependency))
for path, expected in inputs.items():
    assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected, path
(out / 'artifacts.json').write_text(json.dumps({'status': 'PASS', 'scope': __doc__ + '; host DEBUG sanitizer and actual-source EFI ARM64 DEBUG object; no image or board claim',
    'commands': commands, 'inputs': inputs}, indent=2) + '\n')
print('PASS: actual read-only library EFI ARM64 DEBUG object')
