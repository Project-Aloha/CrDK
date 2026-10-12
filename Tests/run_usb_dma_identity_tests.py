#!/usr/bin/env python3
"""Actual native USB PCI map/MU IOMMU/SMMU chain, DEBUG ASan/UBSan. MIT"""
import argparse,hashlib,json,shlex,subprocess
from pathlib import Path
cr=Path(__file__).resolve().parents[1];root=cr.parents[1];edk=root/'MU_BASECORE'
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--smmu-source',type=Path,default=cr/'Driver/SmmuCrDxe/SmmuCrDxe.c');p.add_argument('--expect-closed',action='store_true');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
framework=cr/'Tests/SmmuCrDxeTest.c'
framework_text=framework.read_text().replace('"../Driver/SmmuCrDxe/SmmuCrDxe.c"','"'+str(a.smmu_source.resolve())+'"').replace('"../Target/','"'+str(cr/'Target')+'/')
(out/'SmmuCrModel.inc').write_text(framework_text)
model=cr/'Tests/SmmuUsbDxeAcquireTest.c';prefix=model.read_text().split('int main(void)',1)[0]
prefix=prefix.replace('#define USB_BASE 0xa800000ULL','#define USB_BASE mUsbModelBase')
prefix=prefix.replace('"SmmuCrDxeTest.c"','"SmmuCrModel.inc"').replace('"../Target/','"'+str(cr/'Target')+'/')
(out/'UsbSmmuModel.inc').write_text(prefix)
sources=[cr/'Tests/UsbDmaIdentityChainTest.c',cr/'Library/SmmuLib/smmu.c',cr/'Library/SmmuLib/SmmuDisplay.c',cr/'Library/SmmuLib/SmmuDisplayOwnership.c']
flags=['-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections']
flags+=['-I'+str(x) for x in [out,cr/'Include',edk/'MdePkg/Include',edk/'MdePkg/Include/X64',edk/'MdeModulePkg/Include']]
sha=lambda x:hashlib.sha256(x.read_bytes()).hexdigest();deps={Path(__file__).resolve(),model,framework}
for src in sources:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(src)],text=True).replace('\\\n',' ');deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
before={str(x.relative_to(root)):sha(x) for x in sorted(deps)};exe=out/'DEBUG';cmd=['clang',*flags,'-Wl,--gc-sections',*map(str,sources),'-o',str(exe)]
r=subprocess.run(cmd,text=True,capture_output=True);(out/'compile.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
r=subprocess.run([str(exe)],text=True,capture_output=True);(out/'test.log').write_text(r.stdout+r.stderr)
if a.expect_closed:assert r.returncode!=0 and 'NonDiscoverablePciDeviceIo.c:1573:' in r.stderr,r.stdout+r.stderr;print('RED: actual controller identity is denied and NDPCI hits original SetAttribute assertion1573')
else:assert r.returncode==0,r.stdout+r.stderr;print(r.stdout,end='')
assert all(sha(root/n)==h for n,h in before.items()),'source drift'
(out/'receipt.json').write_text(json.dumps({'status':'EXPECTED_RED' if a.expect_closed else 'PASS','scope':'actual NDPCI+MU IoMmuLib+SMMU DXE/core+bothSM8250/8350targets DEBUG ASan/UBSan; modeledMMIO/cache/allocator, no board retest','inputs':before,'command':cmd,'executable_sha256':sha(exe)},indent=2)+'\n')
