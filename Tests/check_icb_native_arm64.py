#!/usr/bin/env python3
"""DEBUG actual EFI ARM64 ICC/RPMh/QUP/I2C objects; no firmware/runtime claim."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
pkg=Path(__file__).resolve().parents[1];root=pkg.parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
flags=['--target=aarch64-none-elf','-ffreestanding','-std=gnu17','-fshort-wchar','-g','-O1','-include','Uefi.h','-Wall','-Wextra','-Werror']
flags+=['-I'+str(x) for x in [pkg/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/AArch64']]
src=[pkg/x for x in ['Driver/ICBCrDxe/ICBCrDxe.c','Library/ICBLib/icb.c','Library/RpmhLib/rpmh.c','Driver/QUPCrDxe/QUPCrDxe.c','Driver/I2CCrDxe/I2CCrDxe.c','Target/Sm8350/CrStorageInterconnectTarget.c','Target/Sm8350/CrQupTarget.c']]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();deps={Path(__file__).resolve()}
for s in src:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True).replace('\\\n',' ')
 deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
inputs={str(x.relative_to(root)):sha(x) for x in sorted(deps)};records=[]
for s in src:
 obj=out/(s.stem+'.o');log=out/(s.stem+'.log');cmd=['clang',*flags,'-c',str(s),'-o',str(obj)]
 c=subprocess.run(cmd,text=True,capture_output=True);log.write_text(c.stdout+c.stderr);assert c.returncode==0,c.stderr
 data=obj.read_bytes();assert data[:4]==b'\x7fELF' and int.from_bytes(data[18:20],'little')==183,'not AArch64 ELF'
 records.append({'source':str(s.relative_to(root)),'object':str(obj.relative_to(root)),'object_sha256':sha(obj),'command':cmd,'log_sha256':sha(log)})
assert inputs=={str(x.relative_to(root)):sha(x) for x in sorted(deps)},'source drift'
(out/'receipt.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'inputs':inputs,'objects':records,'compiler':subprocess.check_output(['clang','--version'],text=True)},indent=2)+'\n')
print('ICC/RPMh/QUP/I2C: seven actual DEBUG EFI ARM64 objects PASS')
