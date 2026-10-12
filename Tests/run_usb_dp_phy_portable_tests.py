#!/usr/bin/env python3
"""Actual shared PHY, native/NT checked IO and retained provider fault matrix."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True);a.add_argument('--legacy-source',type=Path);args=a.parse_args();o=args.output.resolve();o.mkdir(parents=True,exist_ok=True)
sources=[p/'Tests/UsbDpPhyPortableTest.c',p/'Library/UsbDpPhyCrLib/UsbDpPhy.c']
inputs=sources+[Path(__file__),p/'Include/Library/UsbDpPhyCrLib.h',p/'Library/UsbDpPhyCrLib/PhyTables.h']+[p/'Include/oskal'/n for n in ('cr_atomic.h','cr_memory.h','cr_string.h','cr_mutex.h','cr_time.h','cr_status.h','cr_types.h')]
inputs += [p/'Tests/UsbDpPhyLegacyParityTest.c',p/'Tests/UsbDpPhyTest.c']+[p/'Driver/UsbDpPhyCrDxe'/n for n in ('UsbDpPhyCrDxe.c','Fsa4480.c','Fsa4480.h','UsbDpPhyCrDxe.inf')]
frozen={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs};runs=[]
for kernel in (False,True):
 mode='nt' if kernel else 'native';exe=o/mode
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
 if kernel:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
 else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 cmd=flags+list(map(str,sources))+['-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(o/(mode+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(o/(mode+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode();runs.append({'mode':mode,'command':cmd})
 r=subprocess.run([str(exe),'parity'],text=True,capture_output=True,timeout=60);r.check_returncode();(o/(mode+'-parity.log')).write_text(r.stdout)
if (o/'native-parity.log').read_bytes()!=(o/'nt-parity.log').read_bytes():raise RuntimeError('ABI programming parity mismatch')
exe=o/'native-adapter'
cmd=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fshort-wchar']
cmd+=['-I'+str(x) for x in (p/'Include',root/'Platforms/MUCommonCrPkg/Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64')]
cmd+=list(map(str,(p/'Tests/UsbDpPhyLegacyParityTest.c',p/'Driver/UsbDpPhyCrDxe/Fsa4480.c',p/'Library/UsbDpPhyCrLib/UsbDpPhy.c')))+['-o',str(exe)]
r=subprocess.run(cmd,text=True,capture_output=True);(o/'adapter-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(o/'adapter-parity.log').write_text(r.stdout+r.stderr);r.check_returncode()
if r.stdout!=(o/'native-parity.log').read_text():raise RuntimeError('actual native adapter programming parity mismatch')
runs.append({'mode':'native-adapter','command':cmd})
if args.legacy_source:
 legacy=args.legacy_source.resolve();exe=o/'legacy'
 cmd=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fshort-wchar','-DCR_USB_DP_PHY_SOURCE="'+str(legacy/'UsbDpPhyCrDxe.c')+'"']
 cmd+=['-DCR_USB_DP_PHY_LEGACY_TEST="'+str(legacy/'UsbDpPhyTest.c')+'"']
 cmd+=['-I'+str(x) for x in (p/'Include',root/'Platforms/MUCommonCrPkg/Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64')]
 cmd+=list(map(str,(p/'Tests/UsbDpPhyLegacyParityTest.c',legacy/'Fsa4480.c')))+['-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(o/'legacy-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(o/'legacy-parity.log').write_text(r.stdout+r.stderr);r.check_returncode()
 if r.stdout!=(o/'native-parity.log').read_text():raise RuntimeError('actual old native programming parity mismatch')
 runs.append({'mode':'legacy-native','command':cmd})
 for f in [legacy/n for n in ('UsbDpPhyCrDxe.c','PhyTables.h','Fsa4480.c','Fsa4480.h','UsbDpPhyTest.c')]+[p/'Tests/UsbDpPhyLegacyParityTest.c']:
  frozen[str(f.relative_to(root))]=hashlib.sha256(f.read_bytes()).hexdigest();inputs.append(f)
if frozen!={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}:raise RuntimeError('source drift')
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'inputs':frozen,'runs':runs},indent=2)+'\n')
