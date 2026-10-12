#!/usr/bin/env python3
"""Shared panel protocol adapter and real RPMh selected-graph composition."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',required=True,type=Path)
p.add_argument('--rail-source',type=Path)
p.add_argument('--expect-composition-before',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
rail=a.rail_source.resolve() if a.rail_source else pkg/'Library/RpmhLib/rpmh_rail.c'
target=out/'target.c';target.write_text('#include <stdlib.h>\n#undef NULL\n#include "'+str(pkg/'Target/Sm8250/CrTarget.c')+'"\nCONST CR_DAL_RPMH_RAILS *CrTestGetPanelRails(VOID){return &mRails;}\nCONST CR_DAL_RPMH_TOPOLOGY *CrTestGetPanelTopology(VOID){return &mTopology;}\nCR_STATUS CrTargetClockInit(ClockDriverContext *C){abort();}\nCR_STATUS CrTargetClockReset(CONST CHAR8 *C,CONST CHAR8 *I,BOOLEAN A){abort();}\nCR_STATUS CrTargetClockResolveResource(CONST CHAR8 *C,CONST CHAR8 *I,CR_DAL_CLOCK_RESOURCE_KIND K,CONST CHAR8 **N,UINT32 *F){abort();}\n')
base=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
 '-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-I'+str(pkg/'Include')]
native=['-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
nt=['-D_KERNEL_MODE','-pthread','-I'+str(pkg/'Tests/KernelMocks')]
panel=pkg/'Driver/MDSSCrDxe/MdpPanelPower.c'
cases=[('panel',native,[panel,pkg/'Tests/MdpPanelPowerTest.c']),
 ('composition',native,[panel,rail,pkg/'Library/RpmhArcVoteLib/RpmhArcVote.c',pkg/'Tests/MdpPanelPowerRpmhTest.c',target])]
if not a.expect_composition_before:
 cases += [('rail-native',native,[rail,pkg/'Tests/RpmhRetainedEnableTest.c']),('rail-nt',nt,[rail,pkg/'Tests/RpmhRetainedEnableTest.c'])]
records=[];deps={Path(__file__).resolve()}
for name,platform,sources in cases:
 flags=base+platform;exe=out/name
 for src in sources:
  d=subprocess.check_output(flags+['-MM','-MT','test',str(src)],text=True)
  deps.update(Path(x).resolve() for x in shlex.split(d.split(':',1)[1].replace('\\\n',' ')))
 cmd=flags+list(map(str,sources))+['-Wl,--gc-sections','-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(out/(name+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30);(out/(name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
 if a.expect_composition_before and name=='composition':
  if not r.returncode or 'MdpPanelPowerSet' not in r.stderr:raise RuntimeError('expected actual panel/RPMh preexisting BOB admission failure missing')
 else:r.check_returncode()
 records.append({'name':name,'command':cmd,'exit':r.returncode,'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
  'log_sha256':hashlib.sha256((out/(name+'.log')).read_bytes()).hexdigest()})
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__+'; modeled external CmdDB/TCS/GPIO; no hardware/firmware proof',
 'expected_before_failure':a.expect_composition_before,'cases':records,'inputs':{str(x.relative_to(root) if x.is_relative_to(root) else x):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(deps)}},indent=2)+'\n')
