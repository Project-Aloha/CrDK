#!/usr/bin/env python3
"""Exercise real subprocess failures and CI stale-artifact suppression."""
from pathlib import Path
import json
import hashlib
import os
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix="crane-build-wrapper-") as name:
    work=Path(name)
    scripts=work/"Platforms/TestPkg";scripts.mkdir(parents=True)
    snippet='''import json,os,sys,signal
from pathlib import Path
stage=STAGE
if stage=="prepare": stage="setup" if "--setup" in sys.argv else "update"
phase=next((v.split("=",1)[1] for v in sys.argv if v.startswith("CR_UART_HANDOFF_PHASE=")),None)
if phase: stage += ":"+phase
with open("calls.jsonl","a") as stream: stream.write(json.dumps([stage,sys.argv[1:]])+"\\n")
if os.environ.get("FAIL_STAGE")==stage:
 if os.environ.get("FAIL_CODE")=="signal": os.kill(os.getpid(),signal.SIGTERM)
 sys.exit(int(os.environ["FAIL_CODE"]))
'''
    for path,stage in [(work/"build_boot_shim.sh","shim"),(scripts/"PlatformBuild.py","sb"),
                       (scripts/"PlatformBuildNoSb.py","nosb")]:
        text=snippet.replace('stage=STAGE','stage='+repr(stage))
        path.write_text("#!"+sys.executable+"\n"+text);path.chmod(0o755)
    harness=work/"harness.py"
    harness.write_text('''import importlib.util,os
spec=importlib.util.spec_from_file_location("build_wrapper",SOURCE)
build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)
target=build.Target("qcom-test","Test","TestPkg","0x1234","0x1000",0,"DEBUG")
if os.environ.get("WINDOWS_PROFILE"):
 target.windows_acpi=os.environ["WINDOWS_PROFILE"]
 if os.environ.get("WINDOWS_DEVICE")=="hdk":
  target.device="qcom-hdk8450";target.platform="Sm8450Cr";target.package="WaipioCrPkg"
if os.environ.get("UART_PROFILE"):
 target.uart_handoff=True
 if os.environ.get("UART_DEVICE")=="hdk":
  target.device="qcom-hdk8450";target.platform="Sm8450Cr";target.package="WaipioCrPkg"
if os.environ.get("MODE")=="prepare": build.prepare_build("DEBUG","TestPkg")
elif os.environ.get("MODE")=="copy": build.ci_copy_fd_after_single_device_building(target)
else: build.build_device(target)
'''.replace('SOURCE',repr(str(root/"build_uefi.py"))))
    def run(stage=None,code=None,ci=False,mode="build",profile=None,device=None,uart=False):
        log=work/"calls.jsonl";log.unlink(missing_ok=True)
        env=dict(os.environ,WM_CI_BUILD="true" if ci else "false",MODE=mode)
        env.pop("WINDOWS_PROFILE",None);env.pop("WINDOWS_DEVICE",None)
        env.pop("UART_PROFILE",None);env.pop("UART_DEVICE",None)
        if profile is not None: env.update(WINDOWS_PROFILE=str(profile),WINDOWS_DEVICE=str(device))
        if uart: env.update(UART_PROFILE="1",UART_DEVICE=str(device))
        if stage: env.update(FAIL_STAGE=stage,FAIL_CODE=str(code))
        else: env.pop("FAIL_STAGE",None);env.pop("FAIL_CODE",None)
        result=subprocess.run([sys.executable,str(harness)],cwd=work,env=env,capture_output=True,text=True)
        calls=[json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return result,calls
    result,calls=run();assert result.returncode==0 and [c[0] for c in calls]==["shim","nosb"],result.stderr
    assert calls[0][1]==["-b","0x1234","-s","0x1000"]
    assert calls[1][1]==["TARGET=DEBUG","TARGET_DEVICE=qcom-test"]
    result,calls=run("shim",3);assert result.returncode==3 and len(calls)==1
    result,calls=run("nosb",7);assert result.returncode==7 and len(calls)==2
    result,calls=run("nosb","signal");assert result.returncode==143 and len(calls)==2
    output=work/"Build/TestPkg";(output/"DEBUG_CLANGPDB/FV").mkdir(parents=True)
    stale_fd=output/"DEBUG_CLANGPDB/FV/TEST_EFI.fd";stale_img=output/"qcom-test.img"
    stale_fd.write_bytes(b"old-firmware");stale_img.write_bytes(b"old-image")
    result,calls=run("sb",4,ci=True)
    assert result.returncode==4 and [c[0] for c in calls]==["shim","sb"]
    assert stale_fd.read_bytes()==b"old-firmware" and stale_img.read_bytes()==b"old-image"
    assert not (output/"ci_artifacts").exists()
    # The optional HDK pair reaches only that board's builder and preserves
    # subprocess argument boundaries, even for an as-yet invalid input path.
    waipio=work/"Platforms/WaipioCrPkg";waipio.mkdir()
    for name in ("PlatformBuild.py","PlatformBuildNoSb.py"):
        (waipio/name).write_bytes((scripts/name).read_bytes())
    result,calls=run(profile=work/"ACPI pair",device="hdk")
    assert result.returncode==0 and [c[0] for c in calls]==["shim","nosb"],result.stderr
    assert calls[1][1]==["TARGET=DEBUG","TARGET_DEVICE=qcom-hdk8450","CR_HDK_WINDOWS_ACPI=1",
                          "CR_WINDOWS_ACPI_DIR="+str(work/"ACPI pair")]
    result,calls=run(profile=work/"pair",device="other")
    assert result.returncode!=0 and not calls and "only qcom-hdk8450" in result.stderr
    # CI also preserves distinct default/profile FD names, not just images.
    wp=work/"Build/WaipioCrPkg";(wp/"DEBUG_CLANGPDB/FV").mkdir(parents=True)
    (wp/"DEBUG_CLANGPDB/FV/SM8450CR_EFI.fd").write_bytes(b"windows-fd")
    (wp/"qcom-hdk8450-windows-cr.img").write_bytes(b"windows-image")
    ci=wp/"ci_artifacts/qcom-hdk8450";ci.mkdir(parents=True)
    (ci/"SM8450CR_EFI_NOSB.fd").write_bytes(b"default-fd")
    (ci/"qcom-hdk8450_NOSB.img").write_bytes(b"default-image")
    result,calls=run(profile=work/"pair",device="hdk",mode="copy")
    assert result.returncode==0,result.stderr
    assert (ci/"SM8450CR_EFI_WINDOWS_CR_NOSB.fd").read_bytes()==b"windows-fd"
    assert (ci/"qcom-hdk8450-windows-cr_NOSB.img").read_bytes()==b"windows-image"
    assert (ci/"SM8450CR_EFI_NOSB.fd").read_bytes()==b"default-fd"
    assert (ci/"qcom-hdk8450_NOSB.img").read_bytes()==b"default-image"
    # UART needs both actual subprocesses; any first/second failure must stop
    # before publication. Its filename composes with the ACPI profile.
    result,calls=run(uart=True,device="hdk")
    assert result.returncode==0 and [c[0] for c in calls]==["shim","nosb:inventory","nosb:validate"],result.stderr
    assert calls[1][1][-2:]==["CR_HDK_UART_HANDOFF=1","CR_UART_HANDOFF_PHASE=inventory"]
    assert calls[2][1][-2:]==["CR_HDK_UART_HANDOFF=1","CR_UART_HANDOFF_PHASE=validate"]
    result,calls=run("nosb:inventory",14,uart=True,device="hdk")
    assert result.returncode==14 and [c[0] for c in calls]==["shim","nosb:inventory"]
    result,calls=run("sb:validate",15,ci=True,uart=True,device="hdk")
    assert result.returncode==15 and [c[0] for c in calls]==["shim","sb:inventory","sb:validate"]
    assert not (ci/"SM8450CR_EFI_UART_HANDOFF_SB.fd").exists()
    result,calls=run(uart=True,device="other")
    assert result.returncode!=0 and not calls and "only qcom-hdk8450" in result.stderr
    (wp/"DEBUG_CLANGPDB/FV/SM8450CR_EFI.fd").write_bytes(b"uart-windows-fd")
    (wp/"qcom-hdk8450-windows-cr-uart-handoff.img").write_bytes(b"uart-windows-image")
    receipts=wp/"UartHandoff/DEBUG_NOSB_WINDOWS";receipts.mkdir(parents=True)
    checked=receipts/"checked.fd";checked.write_bytes(b"qualified-uart-fd")
    (receipts/"fd.json").write_text(json.dumps({"fd_snapshot":str(checked),
        "fd_sha256":hashlib.sha256(checked.read_bytes()).hexdigest()}))
    result,calls=run(profile=work/"pair",uart=True,device="hdk",mode="copy")
    assert result.returncode==0,result.stderr
    assert (ci/"SM8450CR_EFI_WINDOWS_CR_UART_HANDOFF_NOSB.fd").read_bytes()==b"qualified-uart-fd"
    assert checked.read_bytes()==b"qualified-uart-fd"
    assert (ci/"qcom-hdk8450-windows-cr-uart-handoff_NOSB.img").read_bytes()==b"uart-windows-image"
    assert (ci/"qcom-hdk8450_NOSB.img").read_bytes()==b"default-image"
    checked.write_bytes(b"changed-qualified-fd")
    result,calls=run(profile=work/"pair",uart=True,device="hdk",mode="copy")
    assert result.returncode!=0 and "does not match" in result.stderr
    assert (ci/"SM8450CR_EFI_WINDOWS_CR_UART_HANDOFF_NOSB.fd").read_bytes()==b"qualified-uart-fd"
    # Setup/update are also checked, before attempting any firmware build.
    path=scripts/"PlatformBuild.py";path.write_text("#!"+sys.executable+"\n"+snippet.replace('stage=STAGE','stage='+repr("prepare")))
    result,calls=run("setup",11,mode="prepare");assert result.returncode==11 and [c[0] for c in calls]==["setup"]
    result,calls=run("update",12,mode="prepare");assert result.returncode==12 and [c[0] for c in calls]==["setup","update"]
print("Build wrapper: subprocess/signal failures propagate, setup stops, failed CI build leaves stale artifacts untouched")
