#!/usr/bin/env python3
"""Run actual ConSplitter mode intersection and GOP callbacks under sanitizers."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
p.add_argument('--require-linear',action='store_true')
a=p.parse_args()
crane=Path(__file__).resolve().parents[1]
root=crane.parents[1]
console=root/'Platforms/CranePkg/Override/MU_BASECORE/MdeModulePkg/Universal/Console/ConSplitterDxe'
a.output.mkdir(parents=True,exist_ok=True)
def function(path,name):
    s=path.read_text();signature=s.index('\n'+name+' (\n')
    start=max(s.rfind('\nEFI_STATUS',0,signature),s.rfind('\nBOOLEAN',0,signature))+1
    body=s.index('{',signature);end=body+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
selected=[(console/'ConSplitterGraphics.c',name) for name in ('ConSplitterPrimaryGopEnabled','ConSplitterSyncPrimaryGop')]
selected += [(console/'ConSplitter.c',name) for name in ('ConSplitterIsPrimaryGop','ConSplitterAddGraphicsOutputMode')]
selected += [(console/'ConSplitterGraphics.c',name) for name in (
    'ConSplitterGraphicsOutputQueryMode','ConSplitterGraphicsOutputSetMode','ConSplitterGraphicsOutputBlt')]
selected += [(console/'ConSplitter.c',name) for name in ('ConSplitterTextOutDeleteDevice','ConSplitterConOutDriverBindingStop','ConSplitterTextOutOutputString')]
(a.output/'con_splitter_graphics_actual.inc').write_text('\n'.join(function(path,name) for path,name in selected))
binary=a.output/'test'
cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror',
     '-Wno-unused-parameter','-Wno-parentheses-equality',
     '-fsanitize=address,undefined','-fno-sanitize-recover=all',
     '-I'+str(a.output),'-I'+str(root/'MU_BASECORE/MdePkg/Include'),
     '-I'+str(root/'MU_BASECORE/MdePkg/Include/X64'),
     '-I'+str(root/'MU_BASECORE/MdeModulePkg/Include'),
     str(crane/'Tests/ConSplitterLinearGopTest.c'),'-o',str(binary)]
subprocess.run(cmd,check=True)
r=subprocess.run([str(binary)]+(['linear'] if a.require_linear else []),capture_output=True,text=True)
(a.output/'test.log').write_text(r.stdout+r.stderr)
(a.output/'artifacts.json').write_text(json.dumps({'command':cmd,'require_linear':a.require_linear,
    'returncode':r.returncode,'sources':{str(path):hashlib.sha256(path.read_bytes()).hexdigest()
    for path in {path for path,_ in selected}|{crane/'Tests/ConSplitterLinearGopTest.c'}}},indent=2)+'\n')
print(r.stdout+r.stderr,end='')
raise SystemExit(r.returncode)
