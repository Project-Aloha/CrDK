#!/usr/bin/env python3
"""Compile the actual read-only inherited display observer with ASan/UBSan."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--absent-observer',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[pkg/'Tests/SmmuDisplayTest.c']
if not a.absent_observer:sources += [pkg/'Library/SmmuLib/SmmuDisplay.c']
flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
       '-fno-sanitize-recover=all','-fshort-wchar','-I'+str(pkg/'Include'),
       '-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
if a.absent_observer:flags += ['-DABSENT_DISPLAY_OBSERVER']
inputs={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
command=flags+[str(p) for p in sources]+['-o',str(out/'test')]
r=subprocess.run(command,text=True,capture_output=True);(out/'compile.log').write_text(r.stdout+r.stderr)
if r.returncode:print(r.stdout+r.stderr);raise SystemExit(r.returncode)
r=subprocess.run([out/'test'],text=True,capture_output=True);(out/'test.log').write_text(r.stdout+r.stderr)
print(r.stdout+r.stderr)
assert all(hashlib.sha256(p.read_bytes()).hexdigest()==h for p,h in inputs.items())
(out/'receipt.json').write_text(json.dumps({'exit_code':r.returncode,'absent_observer':a.absent_observer,
 'inputs':{str(p):h for p,h in inputs.items()},'executable_sha256':hashlib.sha256((out/'test').read_bytes()).hexdigest(),
 'commands':[command]},indent=2)+'\n')
raise SystemExit(r.returncode)
