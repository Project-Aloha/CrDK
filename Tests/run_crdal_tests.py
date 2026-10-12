#!/usr/bin/env python3
"""Exercise the real CrDAL registry ABI using EDK2 headers and sanitizers."""
import argparse, contextlib, hashlib, json, re
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--edk2", type=Path, default=crane.parents[1] / "MU_BASECORE")
parser.add_argument("--cc", default="clang")
parser.add_argument("--output",type=Path)
parser.add_argument("--client-source",type=Path)
parser.add_argument("--client-test",type=Path)
parser.add_argument("--expect-before",action="store_true")
args = parser.parse_args()
context=contextlib.nullcontext(str(args.output.resolve())) if args.output else tempfile.TemporaryDirectory(prefix="crane-crdal-test-")
with context as tmp:
    Path(tmp).mkdir(parents=True,exist_ok=True)
    inputs={};runs=[]
    common = [
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
    ]
    cases = [
        ("registry", "Library/CrDalRegistryLib/CrDalRegistryLib.c",
         "Tests/CrDalRegistryLibTest.c"),
        ("client", "Library/CrDalLib/CrDalLib.c", "Tests/CrDalLibTest.c"),
    ]
    for name, source, test in cases:
        exe = Path(tmp) / ("crdal-" + name + "-test")
        src=args.client_source.resolve() if name=='client' and args.client_source else crane/source
        tst=args.client_test.resolve() if name=='client' and args.client_test else crane/test
        for path in [src,tst]:
            dep=subprocess.check_output(common+['-MM',str(path)],text=True)
            for f in re.sub(r'\\\n',' ',dep).split(':',1)[1].split():
                x=Path(f).resolve();inputs[str(x)]=hashlib.sha256(x.read_bytes()).hexdigest()
        cmd=common+[str(src),str(tst),'-o',str(exe)]
        subprocess.run(cmd,check=True)
        r=subprocess.run([str(exe)],capture_output=True,text=True);(Path(tmp)/(name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
        if args.expect_before and name=='client':
            if r.returncode==0 or 'TestSdPayload' not in r.stderr:raise RuntimeError('missing expected authentic SD warning failure')
        else:r.check_returncode()
        runs.append({'mode':name,'command':cmd,'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
    for f,h in inputs.items():
        if hashlib.sha256(Path(f).read_bytes()).hexdigest()!=h:raise RuntimeError('source drift '+f)
    if args.output:(Path(tmp)/'artifacts.json').write_text(json.dumps({'result':'PASS','expected_before_failure':args.expect_before,'inputs':inputs,'runs':runs},indent=2)+'\n')
