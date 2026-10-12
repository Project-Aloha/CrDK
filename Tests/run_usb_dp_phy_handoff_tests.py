#!/usr/bin/env python3
"""Actual native PHY participant plus portable MDSH journal, ASan/UBSan."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--phy-source', type=Path, help='optional frozen actual frontend for before regression')
a = p.parse_args()
crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
phy_source = (a.phy_source or crane / 'Driver/UsbDpPhyCrDxe/UsbDpPhyCrDxe.c').resolve()
fixture = crane / 'Tests/UsbDpPhyHandoffTest.c'
sources = [fixture, crane / 'Driver/UsbDpPhyCrDxe/Fsa4480.c', crane / 'Library/UsbDpPhyCrLib/UsbDpPhy.c', crane / 'Library/MdssHandoffCrLib/Handoff.c']
includes = [crane / 'Include', crane / 'Driver/UsbDpPhyCrDxe', root / 'Platforms/MUCommonCrPkg/Include', root / 'MU_BASECORE/MdePkg/Include', root / 'MU_BASECORE/MdePkg/Include/X64']
binary = out / 'phy-handoff'
cmd = ['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-fsanitize=address,undefined',
       '-fno-sanitize-recover=all', '-DCR_USB_DP_PHY_SOURCE="' + str(phy_source) + '"',
       *['-I' + str(i) for i in includes], *map(str, sources), '-o', str(binary)]
subprocess.run(cmd, check=True)
r = subprocess.run([str(binary)], capture_output=True, text=True)
(out / 'test.log').write_text(r.stdout + r.stderr)
inputs = sources + [crane / 'Tests/UsbDpPhyTest.c', phy_source,
                   crane / 'Include/Library/UsbDpPhyCrLib.h', crane / 'Library/UsbDpPhyCrLib/PhyTables.h',
                   crane / 'Driver/UsbDpPhyCrDxe/UsbDpPhyCrDxe.inf', crane / 'Include/Interface/CrMdssHandoff.h',
                   crane / 'Include/Protocol/EFIMdssHandoffProtocol.h', Path(__file__)]
(out / 'artifacts.json').write_text(json.dumps({'scope': 'actual native participant/core; host boot-services/provider/MMIO models; no hardware claim',
    'returncode': r.returncode, 'command': cmd, 'sources': {str(i): hashlib.sha256(i.read_bytes()).hexdigest() for i in inputs}}, indent=2) + '\n')
print(r.stdout + r.stderr, end='')
raise SystemExit(r.returncode)
