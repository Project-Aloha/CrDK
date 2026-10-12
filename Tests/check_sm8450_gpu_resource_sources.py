#!/usr/bin/env python3
"""Check actual resolver policy against pinned primary SM8450 resources/accessors."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

REV = 'a53261b60834ce617cc4f3c7723dda8285c59789'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--linux-root', required=True, type=Path)
p.add_argument('--windows-root', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
c = Path(__file__).resolve().parents[1]
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
(o / 'artifacts.json').unlink(missing_ok=True)
paths = ['arch/arm64/boot/dts/qcom/sm8450.dtsi',
         'drivers/gpu/drm/msm/adreno/a6xx_gmu.h',
         'drivers/gpu/drm/msm/adreno/a6xx_gpu.h',
         'drivers/gpu/drm/msm/adreno/a6xx_gmu.c']
texts, refs = {}, {}
for f in paths:
    data = subprocess.check_output(['git', '-C', str(a.linux_root.resolve()), 'show', REV + ':' + f])
    q = o / 'source' / f
    q.parent.mkdir(parents=True, exist_ok=True)
    q.write_bytes(data)
    texts[f] = data.decode()
    refs[f] = {'revision': REV, 'url': 'https://raw.githubusercontent.com/torvalds/linux/' + REV + '/' + f,
               'snapshot': str(q), 'sha256': hashlib.sha256(data).hexdigest()}

def need(condition, message):
    if not condition:
        raise RuntimeError(message)

dt = texts[paths[0]]
def regs(label):
    match = re.search(r'\b' + label + r':\s+[^\{]+\{\s*(?:.|\n)*?\breg\s*=([^;]+);', dt)
    need(match is not None, 'missing source node ' + label)
    cells = [int(n, 0) for n in re.findall(r'0x[\da-fA-F]+|\b\d+\b', match[1])]
    need(len(cells) % 4 == 0, 'invalid source cells ' + label)
    return [(cells[i] * (1 << 32) + cells[i + 1],
             cells[i + 2] * (1 << 32) + cells[i + 3]) for i in range(0, len(cells), 4)]

gpu, gmu, cc, pdc = regs('gpu'), regs('gmu'), regs('gpucc'), regs('pdc')
need(gpu == [(0x03d00000, 0x40000), (0x03d9e000, 0x1000), (0x03d61000, 0x800)], 'GPU source profile drift')
need(gmu == [(0x03d6a000, 0x35000), (0x03d50000, 0x10000), (0x0b290000, 0x10000)], 'GMU source profile drift')
need(cc == [(0x03d90000, 0xa000)], 'GPUCC source profile drift')
need(pdc[0] == (0x0b220000, 0x10000), 'mainline PDC source profile drift')
gpio = a.windows_root.resolve() / 'AcpiCrDsdt/sm8450/crane/gpio.asl'
gpio_data = gpio.read_bytes()
need(re.search(r'Memory32Fixed\(ReadWrite,\s*0x0b220000,\s*0x00030000\)', gpio_data.decode()) is not None,
     'current Windows PIO PDC profile drift')
end = gmu[0][0] + gmu[0][1]
expected = [gpu[0], gpu[2], (gmu[0][0], cc[0][0] - gmu[0][0]),
            (cc[0][0] + cc[0][1], end - cc[0][0] - cc[0][1]), gmu[1], gmu[2], cc[0]]
need(expected[2] == (0x03d6a000, 0x26000) and expected[3] == (0x03d9a000, 0x5000), 'invalid split')
need(expected[3][0] <= gpu[1][0] and gpu[1][0] + gpu[1][1] == end, 'CX-misc not contained')
need(0x0b220000 + 0x30000 <= gmu[2][0], 'Windows PIO/GMU PDC overlap')
src = c / 'Library/Sm8450GpuResourcesCrLib/Sm8450GpuResources.c'
actual = [(int(x, 16), int(y, 16)) for x, y in re.findall(
    r'\{CrSm8450GpuView\w+,\s*CR_SM8450_GPU_RELATION_\w+,\s*(0x[\da-f]+),\s*(0x[\da-f]+)\}', src.read_text())]
need(actual == expected, 'compiled resolver policy differs from actual source-derived split')
need("based on GPU's start address" in texts[paths[1]], 'GMU origin not source qualified')
need(re.search(r'GMU_BYTE_OFFSET\(gmu, offset\)\s+\(\(\(offset\)\s*<<\s*2\)', texts[paths[1]]) is not None,
     'GMU word-offset unit drift')
need(re.search(r'gmu->rscc\s*\+\s*\(offset\s*<<\s*2\)', texts[paths[1]]) is not None, 'RSCC unit drift')
need(re.search(r'cx_misc_mmio\s*\+\s*\(reg\s*<<\s*2\)', texts[paths[2]]) is not None, 'CX-misc unit drift')
need(re.search(r'pdc_write\([^}]+ptr\s*\+\s*\(offset\s*<<\s*2\)', texts[paths[3]]) is not None, 'PDC unit drift')
inputs = [Path(__file__).resolve(), src, c / 'Include/Library/Sm8450GpuResourcesCrLib.h', gpio]
(o / 'artifacts.json').write_text(json.dumps({
    'status': 'PASS', 'scope': 'Exact source-derived seven-view split and register namespaces; no register access authority',
    'references': refs, 'views': expected,
    'pio_pdc': {'mainline': pdc[0], 'current_windows': [0x0b220000, 0x30000]},
    'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
}, indent=2) + '\n')
print('SM8450 GPU resolver pinned resource/accessor source proof PASS')
