#!/usr/bin/env python3
"""Actual generic clock/voter GPU policies in native/NT sanitizer builds."""
from pathlib import Path
import argparse,datetime,hashlib,json,subprocess
pkg=Path(__file__).resolve().parents[1]
a=argparse.ArgumentParser(description=__doc__)
a.add_argument('--output',type=Path,required=True)
a.add_argument('--config-source',type=Path,help='actual saved ClockSetDivider source through compiler overlay')
a.add_argument('--expect-read-only-write-failure',action='store_true',
  help='save the authentic old ClockSetDivider illegal-write assertion in both modes')
args=a.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
if args.expect_read_only_write_failure and not args.config_source:
    a.error('--expect-read-only-write-failure requires --config-source')
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
results={};commands=[];inputs={};bins={}
for kernel in (False,True):
    mode='nt' if kernel else 'native';exe=out/mode
    flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
      '-fsanitize=address,undefined','-fno-sanitize-recover=all',
      '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(pkg/'Include')]
    if kernel: flags += ['-D_KERNEL_MODE','-pthread','-I'+str(pkg/'Tests/KernelMocks')]
    else: flags += ['-fshort-wchar','-I'+str(pkg.parents[1]/'MU_BASECORE/MdePkg/Include'),
      '-I'+str(pkg.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
    if args.config_source:
        overlay=out/(mode+'-overlay.json')
        overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file',
          'name':str(pkg/'Library/ClockLib/clock_config.c'),'external-contents':str(args.config_source.resolve()),'use-external-name':False}]}))
        flags += ['-ivfsoverlay',str(overlay)]
    sources=[pkg/'Tests/ClockGpuPolicyTest.c',pkg/'Library/ClockLib/fixed_pll.c',
      pkg/'Library/PepCrLib/PepJournal.c',pkg/'Library/PepCrLib/PepClock.c']
    for index,q in enumerate(sources):
        depfile=out/(mode+'-'+str(index)+'.d')
        pre=subprocess.run([f for f in flags if not f.startswith('-Wl,')]+['-MM','-MF',str(depfile),str(q)],capture_output=True,text=True)
        pre.check_returncode()
        dep=depfile.read_text().replace('\\\n',' ').split(':',1)[1].split()
        for d in dep:
            path=Path(d).resolve();inputs[str(path)]=sha(path)
    cmd=flags+list(map(str,sources))+['-o',str(exe)]
    commands.append(cmd);c=subprocess.run(cmd,capture_output=True,text=True)
    (out/(mode+'-compile.log')).write_text(c.stdout+c.stderr);c.check_returncode()
    c=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
    (out/(mode+'.log')).write_text(c.stdout+c.stderr)
    if args.expect_read_only_write_failure:
        assert c.returncode != 0 and 'Address!=NeverWrite' in c.stderr, 'expected actual RO write assertion'
        results[mode]={'returncode':c.returncode,'expected_assertion':c.stderr}
    else:
        c.check_returncode();results[mode]=c.stdout
    # Driver sources include the real fixture/core in one TU; pin dependency output.
    for q in sources:inputs[str(q)]=sha(q)
    bins[str(exe)]=sha(exe)
for q in [Path(__file__),pkg/'Include/Library/clock.h',pkg/'Library/ClockLib/clock.c',
          pkg/'Library/ClockLib/clock_votes.c',pkg/'Tests/ClockOwnershipTest.c']:
    inputs[str(q.resolve())]=sha(q)
if args.config_source:
    inputs[str(args.config_source.resolve())]=sha(args.config_source)
for q,h in inputs.items():
    assert sha(q)==h, 'input changed during check: '+q
j={'checked_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
  'status':'EXPECTED_OLD_RO_WRITE_FAILURE_NATIVE_NT' if args.expect_read_only_write_failure else 'PASS_NATIVE_NT',
  'scope':'actual generic clock/voter plus controlled register model; no physical GPU grant',
  'config_override':str(args.config_source.resolve()) if args.config_source else None,
  'commands':commands,'inputs':inputs,'results':results,'binaries':bins}
(out/'artifacts.json').write_text(json.dumps(j,indent=2)+'\n');print(out/'artifacts.json')
