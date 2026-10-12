#!/usr/bin/env python3
"""Original Kodiak AML NPA inventory and actual decode/provider/journal tests."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import tempfile

crane=Path(__file__).resolve().parents[1]
workspace=crane.parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--emit-fixture',type=Path)
args=parser.parse_args()

def integer(token):
    return {'Zero':0,'One':1}[token] if token in ('Zero','One') else int(token,0)

with tempfile.TemporaryDirectory(prefix='cr-pep-npa-') as directory:
    out=Path(directory)
    reference=None
    for aml in sorted((workspace/'Platforms/KodiakPkg/Device').glob('*/ACPI/DSDT.aml')):
        prefix=out/aml.parents[1].name
        result=subprocess.run(['iasl','-p',str(prefix),'-d',str(aml)],capture_output=True,text=True,check=True)
        text=prefix.with_suffix('.dsl').read_text()
        records=[]
        for match in re.finditer(r'"NPARESOURCE"',text):
            literal=re.match(r'"NPARESOURCE"\s*,\s*Package\s*\(0x03\)\s*\{\s*'
                r'(One|Zero|0x[0-9A-Fa-f]+)\s*,\s*"([^"\\]+)"\s*,\s*'
                r'(One|Zero|0x[0-9A-Fa-f]+)\s*\}',text[match.start():])
            assert literal is not None, (aml,text.count('\n',0,match.start())+1)
            records.append((literal[2],integer(literal[1]),integer(literal[3])))
        assert len(records)==21
        assert set(records)=={('/arc/client/rail_cx',1,0),('/arc/client/rail_cx',1,256),
            ('/arc/client/rail_xo',1,0),('/arc/client/rail_xo',1,128)}
        if reference is None: reference=records
        else: assert records==reference, aml
        print(aml.relative_to(workspace),hashlib.sha256(aml.read_bytes()).hexdigest(),flush=True)
    assert reference is not None
    definitions=[]
    for index,(name,opcode,level) in enumerate(reference):
        definitions.append('static const CR_PEP_VALUE NpaArgs'+str(index)+'[] = {'+
            '{.Type=CrPepInteger,.Integer='+str(opcode)+'},'+
            '{.Type=CrPepString,.String={'+json.dumps(name)+','+str(len(name))+'}},'+
            '{.Type=CrPepInteger,.Integer='+str(level)+'}};')
        definitions.append('static const CR_PEP_VALUE NpaRecord'+str(index)+'[] = {'+
            '{.Type=CrPepString,.String={"NPARESOURCE",11}},'+
            '{.Type=CrPepPackage,.Package={NpaArgs'+str(index)+',3}}};')
    fixture='\n'.join(definitions)+'\nstatic const CR_PEP_VALUE OriginalNpaRecords[] = {\n'+',\n'.join(
        '{.Type=CrPepPackage,.Package={NpaRecord'+str(index)+',2}}' for index in range(len(reference)))+'\n};\n'
    if args.emit_fixture is not None:
        args.emit_fixture.write_text(fixture)
    else:
        (out/'NpaFixture.h').write_text(fixture)
        for kernel in (False,True):
            flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                '-I'+str(crane/'Include'),'-I'+str(out)]
            if kernel: flags+=['-D_KERNEL_MODE','-pthread','-I'+str(crane/'Tests/KernelMocks')]
            else: flags+=['-fshort-wchar','-I'+str(workspace/'MU_BASECORE/MdePkg/Include'),
                '-I'+str(workspace/'MU_BASECORE/MdePkg/Include/X64')]
            executable=out/('npa-nt' if kernel else 'npa-native')
            subprocess.run(flags+[str(crane/'Library/PepCrLib/Pep.c'),
                str(crane/'Library/PepCrLib/PepNpa.c'),str(crane/'Library/PepCrLib/PepJournal.c'),
                str(crane/'Tests/PepNpaTest.c'),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True,timeout=30)
