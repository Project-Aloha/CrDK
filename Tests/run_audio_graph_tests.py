#!/usr/bin/env python3
"""Real audio compiler/session/GLINK, genuine ACDB oracle and scripted SMEM peer."""
import argparse, hashlib, json, shlex, struct, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--reuse-oracle',type=Path)
a=p.parse_args();cr=Path(__file__).resolve().parents[1];root=cr.parents[1];o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
(o/'artifacts.json').unlink(missing_ok=True)
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
inventory=cr/'Tests/Fixtures/AcdbCr/inventory.json';expectfile=cr/'Tests/AudioGraph/closed-cases.json';expect=json.loads(expectfile.read_text())
expected={(c['fixture'],c['case']):1 if c['closed_module_graph'] else 2 for c in expect['cases']}
assert len(expected)==116 and list(expected.values()).count(1)==50
oracle=a.reuse_oracle.resolve() if a.reuse_oracle else o/'oracle'
if not a.reuse_oracle:
    cmd=[sys.executable,str(cr/'Tests/AudioCal/generate_vectors.py'),'--source',str(root/'.vscode/audioreach-graphservices'),'--fixtures',str(root/'.vscode/audioreach-conf'),'--inventory',str(inventory),'--output',str(oracle)]
    x=subprocess.run(cmd,text=True,capture_output=True);(o/'generate-oracle.log').write_text(x.stdout+x.stderr);x.check_returncode()
manifest=oracle/'oracle-results.json';m=json.loads(manifest.read_text());inputs={manifest,inventory,expectfile,Path(__file__).resolve(),cr/'Tests/AudioCal/generate_vectors.py',cr/'Tests/AudioCal/upstream_oracle.py'}
# Preserve the actual unmodified upstream build as part of this evidence chain.
build=oracle/'upstream/build.json';inputs.add(build)
j=json.loads(build.read_text())
# Build metadata layout is emitted by the existing actual source oracle.
for field in ('inputs','sources'):
    if isinstance(j.get(field),dict):
        for name,digest in j[field].items():
            if isinstance(digest,str) and len(digest)==64:
                f=Path(name);assert sha(f)==digest;inputs.add(f)
u=struct.Struct('<I');vectors=[]
for k,fixture in enumerate(m):
    f=root/'.vscode/audioreach-conf'/fixture['fixture'];assert sha(f)==fixture['fixture_sha256'];inputs.add(f)
    v=Path(fixture['vectors']);assert sha(v)==fixture['vectors_sha256'];inputs.add(v);b=v.read_bytes();at=4;assert b[:4]==b'CAL1'
    def word():
        global at
        n=u.unpack_from(b,at)[0];at+=4;return n
    namebytes=word();name=b[at:at+namebytes];at+=namebytes;count=word()
    result=bytearray(b'AGS1'+u.pack(len(name))+name+b'\0'+u.pack(count))
    for index in range(count):
        cid=word();req=b[at:at+564];at+=564;pieces=[]
        for _ in range(6):
            status=word();size=word();pieces.append(b[at:at+size]);at+=size
        result+=u.pack(cid)+u.pack(expected.get((fixture['fixture'],cid),0))+req
        for body in (pieces[2],pieces[3],pieces[4]):result+=u.pack(len(body))+body
    assert at==len(b)
    dst=o/('fixture-%02d.graphs'%k);dst.write_bytes(result);inputs.add(dst);vectors.append(dst)
(o/'Glink.tmh').write_text('/* Host-only trace shim. */\n')
cores=[cr/'Library/AudioGraphCrLib/AudioGraph.c',cr/'Library/AudioCalCrLib/AudioCal.c',cr/'Library/AcdbCrLib/Acdb.c',cr/'Library/AudioGprCrLib/AudioGpr.c',cr/'Library/UcsiCrLib/Glink.c',cr/'Tools/AudioGraphSession/AudioGraphPeer.c']
tests=[cr/'Tests/AudioGraphCrTest.c',*cores];consumer=[cr/'Tools/AudioGraphSession/AudioGraphSession.c',*cores]
inputs.update(tests+consumer)
inputs.update((cr/'Library/AudioGraphCrLib').glob('*'))
inputs.update((cr/'Tools/AudioGraphSession').glob('*'))
runs=[]
for nt in (False,True):
    flags=['-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']
    if nt:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(cr/'Tests/KernelMocks'),'-I'+str(o)]
    else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    flags+=['-I'+str(cr/'Include')]
    for source in set(tests+consumer):
        dep=subprocess.check_output(['clang',*flags,'-MM',str(source)],text=True)
        inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n','').split(':',1)[1]))
    before={str(f):sha(f) for f in sorted(inputs)}
    if runs:assert all(before[k]==v for k,v in runs[0]['inputs'].items()),'Input changed between ABIs'
    suffix='nt' if nt else 'native';record={'nt':nt,'inputs':before,'programs':[]}
    for name,sources,args in [('audio-graph',tests,list(map(str,vectors))),('AudioGraphSession',consumer,[str(root/'.vscode/audioreach-conf/qcom/qli/Kaanapali/acdbdata/acdb_cal.acdb'),str(cr/'Tools/AudioGraphSession/kaanapali-closed-test.profile')])]:
        exe=o/(name+'-'+suffix);cmd=['clang',*flags,*map(str,sources),'-o',str(exe)]
        x=subprocess.run(cmd,text=True,capture_output=True);(o/(exe.name+'-compile.log')).write_text(x.stdout+x.stderr)
        if x.returncode:print(x.stdout+x.stderr);x.check_returncode()
        run=[str(exe),*args];x=subprocess.run(run,text=True,capture_output=True,timeout=120)
        (o/(exe.name+'.log')).write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='');x.check_returncode()
        record['programs'].append({'command':cmd,'run':run,'binary_sha256':sha(exe),'output':x.stdout})
    assert before=={str(f):sha(f) for f in sorted(inputs)},'Input changed during tests'
    runs.append(record)
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'Actual portable compiler/session/GPR/GLINK and simulated peer only; no active binding or hardware readiness','runs':runs,'inputs':{str(f):sha(f) for f in sorted(inputs)}},indent=2)+'\n')
