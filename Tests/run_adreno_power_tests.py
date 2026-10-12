#!/usr/bin/env python3
"""Actual A730 power core: source-derived OPP oracle, native/NT sanitizers, optional ARM64."""
import argparse
import datetime
import hashlib
import json
import re
import shlex
import subprocess
import tarfile
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--arm64-clang', type=Path)
p.add_argument('--wdk', type=Path)
p.add_argument('--sdk', type=Path)
p.add_argument('--trace-dir', type=Path)
a = p.parse_args()
c = Path(__file__).resolve().parents[1]
r = c.parents[1]
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
if (o / 'artifacts.json').exists():
    raise RuntimeError('Use a fresh output directory; preserve prior receipts')
hashfile = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
linux = r / '.vscode/linux'
dts = linux / 'arch/arm64/boot/dts/qcom/sm8450.dtsi'
levels = linux / 'include/dt-bindings/power/qcom,rpmhpd.h'
gmu = linux / 'drivers/gpu/drm/msm/adreno/a6xx_gmu.c'
hfi = linux / 'drivers/gpu/drm/msm/adreno/a6xx_hfi.h'
hfic = hfi.with_suffix('.c')
android = Path('/home/sunf/droidvm-repos/qcrosvm/reference/codes/waipio-msm-5.10/drivers/gpu/msm/adreno_gen7_rpmh.c')
references = [dts, levels, gmu, hfi, hfic, android]
macros = {n: int(v) for n, v in re.findall(r'#define\s+(RPMH_REGULATOR_LEVEL_\w+)\s+(\d+)', levels.read_text())}

def block(text, marker):
    start = text.index('{', text.index(marker))
    at, depth = start + 1, 1
    while depth:
        depth += (text[at] == '{') - (text[at] == '}')
        at += 1
    return text[start + 1:at - 1]

facts = {}
for name in ['gpu', 'gmu']:
    s = block(dts.read_text(), name + '_opp_table:')
    rows = []
    for freq, level in re.findall(r'opp-hz\s*=\s*/bits/\s*64\s*<(\d+)>;\s*opp-level\s*=\s*<(\w+)>;', s):
        rows.append((int(freq), macros[level]))
    assert len(rows) == (12 if name == 'gpu' else 2)
    facts[name] = [(0, 0)] + sorted(rows)
assert 'u32 vote;\n\tu32 acd;\n\tu32 freq;' in block(hfi.read_text(), 'struct perf_gx_level')
assert 'gx_votes[16]' in block(hfi.read_text(), 'struct a6xx_hfi_msg_perf_table {')
assert 'cx_votes[4]' in block(hfi.read_text(), 'struct a6xx_hfi_msg_perf_table {')
assert 'gmu->current_perf_index = gmu->nr_gpu_freqs - 1;' in gmu.read_text()
assert 'clk_set_rate(gmu->core_clk, 200000000);' in gmu.read_text()
assert '200000000 : 150000000);' in gmu.read_text()
reference_header = o / 'AdrenoPowerReference.h'
reference_header.write_text('/* Generated only from pinned Linux DTS facts, not implementation. */\n' + ''.join(
    'static const CR_ADRENO_POWER_OPP Reference' + name.capitalize() + '[]={\n' +
    ''.join(f'  {{{hz}ULL,{level},0}},\n' for hz, level in rows) + '};\n'
    for name, rows in facts.items()))
(o / 'reference-facts.json').write_text(json.dumps({'source': str(dts), 'rows_hz_level': facts,
    'profile': 'mainline A730.1 reference; not compiled board override/speed-bin qualification',
    'gx_record_order': ['vote', 'acd', 'freq_khz'], 'hfi_capacities': [16, 4]}, indent=2) + '\n')
core = [c / 'Library/AdrenoPowerCrLib/AdrenoPower.c', c / 'Library/AdrenoPowerCrLib/Sm8450Power.c']
sources = [c / 'Tests/AdrenoPowerCrTest.c', *core, c / 'Library/RpmhArcVoteLib/RpmhArcVote.c']
inputs = {*sources, *references, reference_header, o / 'reference-facts.json', Path(__file__).resolve(),
          c / 'Include/Library/AdrenoPowerCrLib.h', *list((c / 'Library/AdrenoPowerCrLib').iterdir())}
commands = []
for nt in (False, True):
    for release in (False, True):
        tag = ('nt' if nt else 'native') + ('-release' if release else '-debug')
        flags = ['-std=c17', '-g', '-O2' if release else '-O1', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-ffunction-sections', '-fdata-sections', '-I' + str(c / 'Include'), '-I' + str(o)]
        if release:
            flags += ['-DNDEBUG', '-DMDEPKG_NDEBUG']
        if nt:
            flags += ['-D_KERNEL_MODE', '-I' + str(c / 'Tests/KernelMocks')]
        else:
            flags += ['-fshort-wchar', '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                      '-I' + str(r / 'MU_BASECORE/MdePkg/Include/X64')]
        commands.append(('host', tag, 'clang', flags, sources, o / ('power-' + tag)))
if a.arm64_clang:
    if not all([a.wdk, a.sdk, a.trace_dir]):
        raise RuntimeError('ARM64 requires wdk/sdk/trace-dir')
    kit = '10.0.26100.0'
    dirs = [a.wdk.resolve() / 'c' / f'Include/{kit}' / n for n in ('km', 'km/crt', 'shared')]
    dirs += [a.sdk.resolve() / 'c' / f'Include/{kit}/shared']
    overlay = o / 'case-overlay.json'
    overlay.write_text(json.dumps({'version': 0, 'case-sensitive': False, 'roots': [
        {'type': 'directory', 'name': str(d), 'contents': [
            {'type': 'file', 'name': f.name, 'external-contents': str(f)}
            for f in d.iterdir() if f.is_file()]} for d in dirs]}))
    inputs.add(overlay)
    for nt in (False, True):
        flags = ['-ffreestanding', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fstack-usage',
                 '-I' + str(c / 'Include')]
        if nt:
            flags += ['--target=aarch64-pc-windows-msvc', '-fms-extensions', '-Wno-microsoft-anon-tag',
                      '-D_ARM64_', '-D_KERNEL_MODE', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A00000A',
                      '-ivfsoverlay', str(overlay), '-I' + str(a.trace_dir.resolve())]
            for d in dirs:
                flags += ['-isystem', str(d)]
        else:
            flags += ['--target=aarch64-none-elf', '-fshort-wchar', '-DMDEPKG_NDEBUG',
                      '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                      '-I' + str(r / 'MU_BASECORE/MdePkg/Include/AArch64')]
        for s in core:
            tag = s.stem + ('-nt' if nt else '-efi')
            commands.append(('object', tag, str(a.arm64_clang.resolve()), flags, [s], o / (tag + '.o')))
for _, tag, compiler, flags, ss, _ in commands:
    for index, s in enumerate(ss):
        dep = subprocess.check_output([compiler, *flags, '-M', str(s)], text=True)
        (o / f'{tag}-{index}.d').write_text(dep)
        inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n', '').split(':', 1)[1]))
before = {str(f): hashfile(f) for f in sorted(inputs)}
(o / 'inputs-before.json').write_text(json.dumps(before, indent=2) + '\n')
runs = []
for kind, tag, compiler, flags, ss, artifact in commands:
    cmd = [compiler, *flags, *map(str, ss), *(['-Wl,--gc-sections'] if kind == 'host' else ['-c']), '-o', str(artifact)]
    x = subprocess.run(cmd, text=True, capture_output=True)
    (o / (tag + '-build.log')).write_text(x.stdout + x.stderr)
    if x.returncode:
        print(x.stdout + x.stderr); x.check_returncode()
    record = {'kind': kind, 'tag': tag, 'command': cmd, 'artifact': str(artifact), 'sha256': hashfile(artifact)}
    if kind == 'host':
        x = subprocess.run([str(artifact)], text=True, capture_output=True)
        log = o / (tag + '.log'); log.write_text(x.stdout + x.stderr)
        if x.returncode:
            print(x.stdout + x.stderr); x.check_returncode()
        print(tag + ': ' + x.stdout, end='')
        record.update(log=str(log), log_sha256=hashfile(log), output=x.stdout)
    runs.append(record)
after = {str(f): hashfile(f) for f in sorted(inputs)}
if before != after:
    raise RuntimeError('Actual input closure changed during compilation/tests')
archive = o / 'sources.tar.gz'
with tarfile.open(archive, 'w:gz') as t:
    for f in sorted(inputs):
        t.add(f, arcname=str(f).lstrip('/'), recursive=False)
result = {'status': 'PASS', 'checked_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'scope': 'Actual portable ARC/HFI perf core, actual ARC qualifier and source profile facts; no transport/power/hardware authority',
          'runs': runs, 'inputs': before, 'archive': str(archive), 'archive_sha256': hashfile(archive),
          'generated': {str(f): hashfile(f) for f in sorted(o.iterdir()) if f.is_file() and f.name != 'sources.tar.gz'},
          'limits': ['No active native/Windows consumer or full link.', 'OPP oracle is pinned SoC DTS, not compiled board overrides.',
                     'Actual CmdDB bytes, firmware compatibility and physical operation remain unqualified.']}
(o / 'artifacts.json').write_text(json.dumps(result, indent=2) + '\n')
print('AdrenoPower PASS;', len(inputs), 'frozen inputs;', len(runs), 'runs/objects')
