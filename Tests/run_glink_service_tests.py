#!/usr/bin/env python3
"""Native and NT actual GLink named-service/framing regressions with sanitizers."""
from pathlib import Path
import argparse,hashlib,json,subprocess,tempfile
crane=Path(__file__).resolve().parents[1];root=crane.parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='cr-glink-services-') as directory:
 out=a.output.resolve() if a.output else Path(directory);out.mkdir(parents=True,exist_ok=True)
 sources=[crane/'Tests/GlinkServiceTest.c',*[crane/'Library/UcsiCrLib'/name for name in ('Glink.c','PmicGlink.c','Qrtr.c')]]
 digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
 runs=[]
 for mode in ('native','nt'):
  flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']
  if mode=='native':
   flags+=['-fshort-wchar',*['-I'+str(path) for path in (crane/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/EmbeddedPkg/Include')]]
  else:
   for name in ('Glink','PmicGlink','Qrtr'):(out/(name+'.tmh')).write_text('/* WPP host shape substitute */\n')
   flags+=['-D_KERNEL_MODE','-pthread','-I'+str(crane/'Tests/KernelMocks'),'-I'+str(out),'-I'+str(crane/'Include')]
  exe=out/(mode+'-service')
  subprocess.run([*flags,*map(str,sources),'-o',str(exe)],check=True)
  result=subprocess.run([str(exe)],check=True,text=True,capture_output=True,timeout=30)
  print(mode+': '+result.stdout.strip());runs.append({'mode':mode,'flags':flags,'binary':str(exe),'sha256':digest(exe),'output':result.stdout})
 inputs=[*sources,crane/'Include/ucsi/Glink.h',crane/'Tests/UsbTypeCTransportTest.c',Path(__file__).resolve()]
 (out/'artifacts.json').write_text(json.dumps({'scope':'actual source native+NT ASan/UBSan replay, not hardware confirmation','runs':runs,'inputs':{str(path):digest(path) for path in inputs}},indent=2)+'\n')
