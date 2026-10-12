#!/usr/bin/env python3
"""Actual SM8450 stop core + target + resolver; register-source oracle and failure injection."""
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
android = Path('/home/sunf/droidvm-repos/qcrosvm/reference/codes/waipio-msm-5.10/drivers/gpu/msm')
regs = android / 'gen7_reg.h'
references = [linux / 'drivers/gpu/drm/msm/adreno/a6xx_gmu.c',
              linux / 'drivers/gpu/drm/msm/adreno/a6xx_gpu.c',
              linux / 'drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml',
              linux / 'drivers/gpu/drm/msm/registers/adreno/a6xx.xml',
              regs, android / 'adreno_gen7_gmu.c', android / 'adreno_hfi.h',
              android / 'kgsl_gmu_core.h']
macros = {n: int(v, 16) for n, v in re.findall(r'#define\s+(GEN7_\w+)\s+(0x[0-9a-fA-F]+)', regs.read_text())}
names = ['GMU_GFX_PWR_CLK_STATUS', 'RBBM_INT_0_MASK', 'GMU_GMU2HOST_INTR_MASK',
         'GMU_AO_HOST_INTERRUPT_MASK', 'GMU_GMU_PWR_COL_KEEPALIVE',
         'GPU_GMU_CX_GMU_CX_FALNEXT_INTF', 'GMU_AHB_FENCE_STATUS_CLR',
         'GMU_AO_AHB_FENCE_CTRL', 'GMU_CM3_SYSRESET', 'RBBM_GBIF_HALT',
         'RBBM_GBIF_HALT_ACK', 'GBIF_HALT', 'GBIF_HALT_ACK', 'RBBM_SW_RESET_CMD',
         'GPU_GMU_AO_GPU_CX_BUSY_STATUS', 'RSCC_SEQ_BUSY_DRV0']
names += [f'RSCC_TCS{i}_DRV0_STATUS' for i in range(10)]
facts = [{'name': n, 'word': macros['GEN7_' + n],
          'physical': (0x3d50000 if n.startswith('RSCC_') else 0x3d00000) + 4 * macros['GEN7_' + n]}
         for n in names]
assert len(facts) == 26
reference_header = o / 'AdrenoStopReference.h'
reference_header.write_text('/* Independent Android register-source oracle. */\nstatic const UINT64 ReferencePhysical[]={\n' +
                           ''.join(f"  0x{v['physical']:x}ULL, /* {v['name']} */\n" for v in facts) + '};\n')
(o / 'reference-facts.json').write_text(json.dumps({'registers': facts,
    'policy': 'inherited Android SM8450, all10 TCS, retained ARB2; no SMMU/power-off authority'}, indent=2) + '\n')
core = [c / 'Library/AdrenoStopCrLib/AdrenoStop.c', c / 'Library/AdrenoStopCrLib/Sm8450Stop.c']
sources = [c / 'Tests/AdrenoStopCrTest.c', *core,
           c / 'Library/Sm8450GpuResourcesCrLib/Sm8450GpuResources.c']
inputs = {*sources, *references, reference_header, o / 'reference-facts.json', Path(__file__).resolve(),
          c / 'Include/Library/AdrenoStopCrLib.h', *list((c / 'Library/AdrenoStopCrLib').iterdir())}
commands = []
for nt in (False, True):
    for release in (False, True):
        tag = ('nt' if nt else 'native') + ('-release' if release else '-debug')
        flags = ['-std=c17', '-pthread', '-g', '-O2' if release else '-O1', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-ffunction-sections', '-fdata-sections', '-I' + str(c / 'Include'), '-I' + str(o)]
        if release:
            flags += ['-DNDEBUG', '-DMDEPKG_NDEBUG']
        if nt:
            flags += ['-D_KERNEL_MODE', '-I' + str(c / 'Tests/KernelMocks')]
        else:
            flags += ['-fshort-wchar', '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                      '-I' + str(r / 'MU_BASECORE/MdePkg/Include/X64')]
        commands.append(('host', tag, 'clang', flags, sources, o / ('stop-' + tag)))
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
          'scope': 'Actual stop engine/target/resolver and source facts; no live power/IRQ/SMMU ownership',
          'runs': runs, 'inputs': before, 'archive': str(archive), 'archive_sha256': hashfile(archive),
          'generated': {str(f): hashfile(f) for f in sorted(o.iterdir()) if f.is_file() and f.name != 'sources.tar.gz'},
          'limits': ['No active native/Windows consumer or full link.', 'Register oracle comes from pinned Android source; target access and sequencing are separately tested.',
                     'Backend power/IRQ/drain authority and physical fence completion remain unqualified.']}
(o / 'artifacts.json').write_text(json.dumps(result, indent=2) + '\n')
print('AdrenoStop PASS;', len(inputs), 'frozen inputs;', len(runs), 'runs/objects')
