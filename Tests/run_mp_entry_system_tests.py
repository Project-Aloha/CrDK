#!/usr/bin/env python3
"""Actual copied AP trampoline EL1/EL2 execution under an independent EL3 denied-OFF monitor."""
from pathlib import Path
import argparse,hashlib,json,re,socket,struct,subprocess,tempfile,time

C=Path(__file__).resolve().parents[1]
DRIVER=C/'Driver/MpServicesCrDxe'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--source',type=Path,default=DRIVER/'MpCrEntry.S')
args=p.parse_args();D=args.output.resolve();D.mkdir(parents=True,exist_ok=True)
SOURCE=args.source.resolve()
(D/'MpCrEntry-reviewed.S').write_bytes(SOURCE.read_bytes())
(D/'MpCrInternal.h').write_bytes((DRIVER/'MpCrInternal.h').read_bytes())
(D/'monitor.S').write_bytes((C/'Tests/MpCrEntrySystemTest.S').read_bytes())
commands=[];results=[]
def run(cmd):
    commands.append(cmd);subprocess.run(cmd,check=True,timeout=15)

run(['clang','--target=aarch64-none-elf','-x','assembler-with-cpp','-I'+str(D),
     '-c',str(D/'MpCrEntry-reviewed.S'),'-o',str(D/'entry.o')])
for mode in ('normal-nc','fault-nc','normal-wb','fault-wb','el2-normal-nc','el2-fault-nc','el2-normal-wb','el2-fault-wb','el2-vhe-denied','el1-pa-denied','el2-pa-denied','el1-tg-denied','el1-ds-denied','el2-ds-denied','el1-tg1-denied'):
    out=D/mode;out.mkdir(exist_ok=True)
    definitions=[]
    if 'fault' in mode:definitions.append('-DTEST_FAULT')
    if 'wb' in mode:definitions.append('-DTEST_WB')
    if 'el2' in mode:definitions.append('-DTEST_EL2')
    if 'vhe' in mode:definitions.append('-DTEST_VHE')
    if '-pa-' in mode:definitions.append('-DTEST_BAD_PA')
    if '-tg-' in mode:definitions.append('-DTEST_BAD_TG')
    if '-tg1-' in mode:definitions.append('-DTEST_BAD_TG1')
    if '-ds-' in mode:definitions.append('-DTEST_BAD_DS')
    run(['clang','--target=aarch64-none-elf','-x','assembler-with-cpp','-I'+str(D),
         *definitions,'-c',str(D/'monitor.S'),'-o',str(out/'monitor.o')])
    run(['ld.lld','-Ttext=0','-e','_start','--oformat=binary',str(out/'monitor.o'),
         str(D/'entry.o'),'-o',str(out/'test.bin')])
    sock=Path(tempfile.mkdtemp(prefix='mp-cr-qmp-'))/'qmp.sock'
    cmd=['qemu-system-aarch64','-machine','virt,secure=on,virtualization=on','-cpu',('max' if 'vhe' in mode else 'cortex-a57'),
         '-smp','1','-m','128M','-bios',str(out/'test.bin'),'-display','none','-serial','none',
         '-qmp',f'unix:{sock},server=on,wait=off']
    commands.append(cmd)
    proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);connection.settimeout(5)
    try:
        deadline=time.monotonic()+3
        while True:
            try:connection.connect(str(sock));break
            except (FileNotFoundError,ConnectionRefusedError):
                assert time.monotonic()<deadline,'QEMU monitor startup timeout'
                time.sleep(.01)
        stream=connection.makefile('rwb');json.loads(stream.readline())
        serial=0
        def request(operation,arguments=None):
            global serial
            serial+=1;ident=serial
            stream.write((json.dumps({'execute':operation,'arguments':arguments or {},'id':ident})+'\n').encode());stream.flush()
            while True:
                reply=json.loads(stream.readline())
                if reply.get('id')==ident:
                    assert 'error' not in reply,reply
                    return reply.get('return')
        request('qmp_capabilities');time.sleep(.5);request('stop')
        registers=request('human-monitor-command',{'command-line':'info registers'})
        (out/'registers.log').write_text(registers)
        for name,address,size in [('frame',0x41100000,128),('result',0x41300000,64)]:
            request('human-monitor-command',{'command-line':f'pmemsave {address:#x} {size:#x} "{out/name}.bin"'})
        frame=(out/'frame.bin').read_bytes();data=struct.unpack('<8Q',(out/'result.bin').read_bytes())
        stage=struct.unpack_from('<I',frame,28)[0]
        fault,off=struct.unpack_from('<2Q',frame,112)
        if 'vhe' in mode:
            assert not any(data[:4]),'unqualified VHE AP executed callback'
            assert not stage and not fault and not off,'unqualified AP touched caller frame'
            assert data[4]>>26==0x17 and data[5]&5==0,'unqualified AP did not reach safe denied OFF'
        elif 'denied' in mode:
            assert not any(data[:4]),'unsupported translation executed callback'
            assert stage==6 and fault==0xffffffffffffffff and off==0xfffffffffffffffd,'translation rejection not retained'
            assert data[4]>>26==0x17 and data[5]&5==0,'format rejection retained live MMU/cache'
        else:
            assert data[0]==(8 if 'el2' in mode else 4),'real callback did not execute'
            assert data[1]&5==5,'callback did not run with M/C enabled'
            assert data[2]==data[3]==0,'AP did not initialize FPCR/FPSR'
            assert data[4]>>26==0x17,'monitor did not receive real SMC exception'
            assert data[5]&5==0,'CPU_OFF rejection retains shared MMU/cache state'
            if 'el2' in mode:assert not (data[7]&0x400),'EL2 FP access trapped'
            else:assert (data[7]>>20)&3==3,'qualified AP FP access not enabled'
            assert stage==6 and off==0xfffffffffffffffd,'denied CPU_OFF not retained'
            if 'fault' in mode:
                assert fault&(1<<63) and ((fault>>26)&0x3f)==0x3c,'TPIDR mutation fault missed private frame'
            else:assert not fault,'normal return faulted'
        pc=int(re.search(r'PC=([0-9a-fA-F]+)',registers).group(1),16)
        assert 0x41000000<=pc<0x41001000,'denied OFF escaped copied Reserved code'
        results.append({'mode':mode,'passed':True,'stage':stage,'fault':hex(fault),
                        'off_result':hex(off),'pc':hex(pc),'callback_sctlr':hex(data[1]),
                        'off_sctlr':hex(data[5]),'fpcr':data[2],'fpsr':data[3],
                        'ccidx':(data[6]>>20)&15})
        request('quit')
    finally:
        connection.close()
        if proc.poll() is None:proc.kill()
        stdout,stderr=proc.communicate(timeout=5)
        (out/'qemu.log').write_bytes(stdout+stderr)
        sock.unlink(missing_ok=True);sock.parent.rmdir()
    print(mode,'actual entry callback/fault, FP initialization, immutable frame and denied OFF park PASS')
assert SOURCE.read_bytes()==(D/'MpCrEntry-reviewed.S').read_bytes(), 'source changed during system test'
inputs=[SOURCE,D/'MpCrEntry-reviewed.S',DRIVER/'MpCrInternal.h',C/'Tests/MpCrEntrySystemTest.S',Path(__file__)]
(D/'artifacts.json').write_text(json.dumps({'scope':__doc__,'results':results,'commands':commands,
 'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},
 'limitations':'QEMU Cortex-A57 EL1 system execution and an independently denied PSCI monitor; not SM8450 PSCI/cache-coherency or hardware activation proof. CCIDX1 not covered; EL2 is non-VHE only.'},indent=2)+'\n')
