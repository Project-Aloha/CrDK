#!/usr/bin/env python3
"""Actual MmcDxe + native MmcHost + shared SDHCI core with independent card/MMIO."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess
crane=Path(__file__).resolve().parents[1]; root=crane.parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
directory=root/'Platforms/CranePkg/Override/MU_BASECORE/EmbeddedPkg/Universal/MmcDxe'
sources={str(directory/n):(directory/n).read_bytes() for n in ('Mmc.c','Mmc.h','MmcBlockIo.c','MmcIdentification.c')}
(out/'Mmc.h').write_bytes((directory/'Mmc.h').read_bytes())
(out/'MmcClassProduction.inc').write_text('\n'.join(re.sub(r'^#include[^\n]*\n','',data.decode().replace('\r\n','\n'),flags=re.M) for name,data in sources.items() if name.endswith('.c')))
for n in ('MmcHost.c','SdhciCrDxe.c'):
    path=crane/'Driver/SdhciCrDxe'/n;sources[str(path)]=path.read_bytes()
core=crane/'Library/SdhciMsmLib/SdhciMsm.c';sources[str(core)]=core.read_bytes()
includes=[out,crane/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/EmbeddedPkg/Include']
cmd=['clang','-std=c11','-fshort-wchar','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-missing-field-initializers','-DMDEPKG_NDEBUG',
     '-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(x) for x in includes],str(crane/'Tests/SdhciMmcIntegrationTest.c'),str(core),'-o',str(out/'test')]
inputs={}
for src in [crane/'Tests/SdhciMmcIntegrationTest.c',core]:
 dep=subprocess.check_output(cmd[:cmd.index(str(crane/'Tests/SdhciMmcIntegrationTest.c'))]+['-MM',str(src)],text=True)
 for f in re.sub(r'\\\n',' ',dep).split(':',1)[1].split():
  x=Path(f).resolve();inputs[str(x)]=hashlib.sha256(x.read_bytes()).hexdigest()
subprocess.run(cmd,check=True)
r=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=45);(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
for f,h in inputs.items():
 if hashlib.sha256(Path(f).read_bytes()).hexdigest()!=h:raise RuntimeError('source drift '+f)

(out/'artifacts.json').write_text(json.dumps({'scope':'actual complete MmcDxe, native MmcHost/entry, shared SDHCI PIO core; independent SD card/register model and resource boundary; ASan/UBSan',
    'inputs':inputs,'sources':{name:hashlib.sha256(data).hexdigest() for name,data in sources.items()},'command':cmd,'binary_sha256':hashlib.sha256((out/'test').read_bytes()).hexdigest()},indent=2)+'\n')
