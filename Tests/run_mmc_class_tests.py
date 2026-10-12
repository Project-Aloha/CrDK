#!/usr/bin/env python3
"""Actual EmbeddedPkg MMC class initialization, media and teardown faults."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-dir', type=Path, default=root/'Platforms/CranePkg/Override/MU_BASECORE/EmbeddedPkg/Universal/MmcDxe')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--case', default='all')
p.add_argument('--expect-failure', action='store_true')
a = p.parse_args()
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
sources = {name: (a.source_dir/name).read_bytes() for name in ('Mmc.c','MmcBlockIo.c','MmcIdentification.c','Mmc.h')}
(out/'Mmc.h').write_bytes(sources['Mmc.h'])
body = '\n'.join(re.sub(r'^#include[^\n]*\n', '', data.decode().replace('\r\n','\n'), flags=re.M)
                 for name,data in sources.items() if name.endswith('.c'))
(out/'MmcClassProduction.inc').write_text(body)
includes = [out, root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/EmbeddedPkg/Include']
cmd = ['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
       '-Wno-missing-field-initializers','-DMDEPKG_NDEBUG','-fsanitize=address,undefined','-fno-sanitize-recover=all',
       *['-I'+str(x) for x in includes],str(crane/'Tests/MmcClassTest.c'),'-o',str(out/'test')]
subprocess.run(cmd, check=True)
result = subprocess.run([str(out/'test'),a.case], timeout=45)
if (result.returncode == 0) == a.expect_failure:
    raise RuntimeError('Unexpected regression result: '+str(result.returncode))
(out/'artifacts.json').write_text(json.dumps({'scope':'Actual complete MMC class C sources; real EFI/MmcHost ABI; modeled host commands/Boot Services; ASan/UBSan',
    'case':a.case,'returncode':result.returncode,'expected_failure':a.expect_failure,
    'sources':{name:hashlib.sha256(data).hexdigest() for name,data in sources.items()},'command':cmd},indent=2)+'\n')
