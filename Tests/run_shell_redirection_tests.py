#!/usr/bin/env python3
"""Compile unchanged production Shell parser/open routines with injected services."""
import argparse, hashlib, json, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
p.add_argument('--before',action='store_true')
p.add_argument('--cc',default='clang')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
crane=Path(__file__).resolve().parents[1];root=crane.parents[1];edk=root/'MU_BASECORE'
params=crane/'Override/MU_BASECORE/ShellPkg/Application/Shell/ShellParametersProtocol.c'
lib=crane/'Override/MU_BASECORE/ShellPkg/Library/UefiShellLib/UefiShellLib.c'
def extract(text,signature):
 start=text.index(signature);end=text.index('\n}\n',start)+3
 return text[start:end]
chunks=[extract(params.read_text(),s) for s in ('VOID\nStripQuotes (','CHAR16 *\nFixFileName (','CHAR16 *\nFixVarName (','EFI_STATUS\nWriteFileTag (','EFI_STATUS\nUpdateStdInStdOutStdErr (')]
chunks += [extract(lib.read_text(),s) for s in ('EFI_STATUS\nEFIAPI\nShellCloseFile (','EFI_STATUS\nEFIAPI\nShellOpenFileByName (')]
inc=a.output/'ShellRedirectionActual.inc';inc.write_text('\n'.join(chunks))
exe=a.output/'shell-redirection';fixture=crane/'Tests/ShellRedirectionTest.c'
cmd=[a.cc,'-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all']
for d in (a.output,edk/'MdePkg/Include',edk/'MdePkg/Include/X64',edk/'MdeModulePkg/Include',edk/'ShellPkg/Include',crane/'Override/MU_BASECORE/ShellPkg/Application/Shell',crane/'Override/MU_BASECORE/ShellPkg/Library/UefiShellLib'):
 cmd+=['-I'+str(d)]
subprocess.run(cmd+[str(fixture),'-o',str(exe)],check=True)
r=subprocess.run([str(exe)]+(['--before'] if a.before else []),check=True,capture_output=True,text=True);print(r.stdout,end='')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
(a.output/'artifacts.json').write_text(json.dumps({'sources':{str(x):sha(x) for x in (params,lib,fixture,Path(__file__),inc)},'binary':sha(exe),'before':a.before,'output':r.stdout},indent=2)+'\n')
