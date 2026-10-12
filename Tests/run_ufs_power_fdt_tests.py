#!/usr/bin/env python3
"""Actual UFS power FDT parser; full source-derived trees and repository libfdt."""
import argparse,hashlib,json,shlex,subprocess,tarfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
c=Path(__file__).resolve().parents[1];root=c.parents[1];edk=root/'MU_BASECORE';base=edk/'MdePkg/Library/BaseFdtLib';fdt=base/'libfdt/libfdt'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
deps={Path(__file__).resolve(),c/'Library/UfsPowerFdtCrLib/UfsPowerFdtCrLib.inf'};refs=[]
linux=root/'.vscode/linux';android=root/'.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450';kernel=root/'.vscode/android_kernel_oneplus_sm8450';adts=android/'kernel_platform/qcom/proprietary/devicetree/qcom'
wrapper=out/'android-hdk-source-composition.dts';wrapper.write_text('/dts-v1/;\n#include "waipio.dtsi"\n#include "waipio-hdk.dtsi"\n#include "waipio-pm8010-spmi.dtsi"\n');deps.add(wrapper)
for name,src,incs,repo in [('linux-hdk8450',linux/'arch/arm64/boot/dts/qcom/sm8450-hdk.dts',[linux/'include',linux/'arch/arm64/boot/dts/qcom'],linux),('android-hdk8450',wrapper,[kernel/'include',adts],android)]:
 pp=out/(name+'.dts');dep=out/(name+'.deps');blob=out/(name+'.dtb')
 cmd=['cpp','-P','-C','-nostdinc','-undef','-D__DTS__','-x','assembler-with-cpp',*sum((['-I',str(i)] for i in incs),[]),'-MD','-MF',str(dep),str(src),str(pp)]
 r=subprocess.run(cmd,capture_output=True);(out/(name+'-cpp.log')).write_bytes(r.stdout+r.stderr);assert r.returncode==0,r.stderr
 dtc=['dtc','-q','-I','dts','-O','dtb','-@','-o',str(blob),str(pp)];r=subprocess.run(dtc,capture_output=True);(out/(name+'-dtc.log')).write_bytes(r.stdout+r.stderr);assert r.returncode==0,r.stderr
 deps.update(Path(x).resolve() for x in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]));deps.add(blob)
 refs.append({'name':name,'commit':subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip(),'cpp':cmd,'dtc':dtc,'blob_sha256':sha(blob),'note':'exact mainline HDK top-level' if name.startswith('linux') else 'test-only composition of actual waipio SoC + HDK board include + PM8010; repository only has WaipioP HDK top-level'})
blobs=[root/'Platforms/KonaCrPkg/Device/qcom-hdk8250/DeviceTreeBlob'/kind/(kind.lower()+'-hdk8250.dtb') for kind in ['Android','Linux']]
blobs += [root/'Platforms/WaipioCrPkg/Device/qcom-hdk8450/DeviceTreeBlob/Android/android-hdk8450.dtb',out/'linux-hdk8450.dtb',out/'android-hdk8450.dtb'];deps.update(blobs)
sources=[c/'Tests/UfsPowerFdtTest.c',c/'Library/UfsPowerFdtCrLib/UfsPowerFdt.c',base/'FdtLib.c',*sorted(fdt.glob('*.c'))]
flags=['-std=c17','-D_GNU_SOURCE','-include','Uefi.h','-g','-O1','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare','-Wno-incompatible-pointer-types','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include'),'-I'+str(edk/'MdePkg/Include'),'-I'+str(edk/'MdePkg/Include/X64'),'-idirafter',str(base),'-I'+str(fdt)]
native=['clang','-target','aarch64-none-elf','-std=c17','-O2','-ffreestanding','-fshort-wchar','-Wall','-Wextra','-Werror','-fstack-usage','-I'+str(edk/'MdePkg/Include'),'-I'+str(edk/'MdePkg/Include/AArch64'),'-I'+str(c/'Include')]
for s in sources:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True).replace('\\\n',' ');deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
raw=subprocess.check_output([*native,'-MM',str(sources[1])],text=True).replace('\\\n',' ');deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
inputs={str(x.relative_to(root)) if x.is_relative_to(root) else str(x):sha(x) for x in sorted(deps)};(out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
builds=[]
for mode in ['debug','release']:
 exe=out/mode;cmd=['clang',*flags,*(['-DMDEPKG_NDEBUG'] if mode=='release' else []),*map(str,sources),'-o',str(exe)]
 r=subprocess.run(cmd,capture_output=True,text=True);(out/(mode+'-compile.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
 r=subprocess.run([str(exe),*map(str,blobs)],capture_output=True,text=True);(out/(mode+'.log')).write_text(r.stdout+r.stderr);print(mode,r.stdout.splitlines()[-1:] if not r.returncode else r.stderr);assert r.returncode==0,r.stderr
 builds.append({'command':cmd,'executable_sha256':sha(exe),'log_sha256':sha(out/(mode+'.log'))})
cmd=[*native,'-c',str(sources[1]),'-o',str(out/'UfsPowerFdt.o')];r=subprocess.run(cmd,capture_output=True,text=True);(out/'arm64.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
assert all(sha(root/k)==v for k,v in inputs.items())
with tarfile.open(out/'sources.tar.gz','w:gz') as tar:
 for k in inputs:tar.add(root/k,arcname=k,recursive=False)
receipt={'scope':'native DEBUG/RELEASE ASan/UBSan + real libfdt; ARM64 TU compile, no firmware/hardware test; packaged Waipio Android blob is QRD and Linux packaged dummy is excluded','status':'PASS','inputs':inputs,'builds':builds,'reference_builds':refs,'aarch64_command':cmd,'object_sha256':sha(out/'UfsPowerFdt.o'),'stack_usage':(out/'UfsPowerFdt.su').read_text(),'archive_sha256':sha(out/'sources.tar.gz')}
(out/'artifacts.json').write_text(json.dumps(receipt,indent=2)+'\n');print('receipt',sha(out/'artifacts.json'))
