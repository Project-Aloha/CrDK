#!/usr/bin/env python3
"""Compile native DP production paths against independent sink/register fixtures."""
from pathlib import Path
import subprocess
import tempfile
import argparse, hashlib, json, shlex
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output",type=Path)
args=parser.parse_args()
class OutputDirectory:
    def __enter__(self):
        if args.output:
            args.output.mkdir(parents=True,exist_ok=True);return str(args.output.resolve())
        self.temp=tempfile.TemporaryDirectory(prefix="crane-dp-");return self.temp.__enter__()
    def __exit__(self,*exc):
        if not args.output:return self.temp.__exit__(*exc)

runs=[];deps=set()
def run(command,**kw):
    compile_run=command[0]=="clang"
    if args.output and compile_run:
        tag=Path(command[command.index("-o")+1]).name
        command=command+["-MD","-MF",str(args.output.resolve()/(tag+".d"))]
    check=kw.pop("check",False)
    result=subprocess.run(command,capture_output=True,text=True,**kw)
    if result.stdout:print(result.stdout,end="")
    if result.stderr:print(result.stderr,end="")
    if args.output:
        tag=Path(command[command.index("-o")+1]).name if compile_run else Path(command[0]).name+"-run"
        (args.output/(tag+".log")).write_text(result.stdout+result.stderr)
        if compile_run and result.returncode==0:
            # Clang's single -MF on a multi-C link retains only its last TU.
            # Record each actual preprocessing closure independently.
            ppflags=[x for x in command[1:] if x.startswith(("-I","-D","-U","-std=","-fshort-wchar"))]
            for i,x in enumerate(command):
                if x=="-include":ppflags.extend([x,command[i+1]])
            for source in [Path(x) for x in command if x.endswith(".c")]:
                d=args.output/(tag+"-"+source.stem+".d")
                depcmd=["clang","-M","-MT",tag,"-MF",str(d.resolve()),*ppflags,str(source)]
                subprocess.run(depcmd,check=True,capture_output=True,text=True)
                deps.update(Path(p).resolve() for p in shlex.split(d.read_text().replace("\\\n"," ").split(":",1)[1]))
                runs.append({"command":depcmd,"returncode":0,"scope":"actual TU preprocessing closure"})
        runs.append({"command":command,"returncode":result.returncode})
    if check:result.check_returncode()
    return result

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [crane / "Include", root / "Platforms/MUCommonCrPkg/Include",
            root / "MU_BASECORE/MdePkg/Include", root / "MU_BASECORE/MdePkg/Include/X64",
            root / "MU_BASECORE/MdeModulePkg/Include",root/"MU_BASECORE/UefiCpuPkg/Include",
            root/"Platforms/MUCommonCrPkg/Include",root/"MU_BASECORE/MdePkg/Include/AArch64"]
with OutputDirectory() as temporary:
    # Exercise the actual ConPlatform selection and ConSplitter parent/child
    # binding functions without rebuilding their unrelated global protocols.
    def function(path, name):
        source = path.read_text()
        signature = source.index("\n" + name + " (\n")
        start = source.rfind("\nEFI_STATUS", 0, signature) + 1
        body = source.index("{", signature)
        end, depth = body + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]
    console = root / "MU_BASECORE/MdeModulePkg/Universal/Console"
    Path(temporary, "dp_console_bindings.inc").write_text(
        function(console / "ConPlatformDxe/ConPlatform.c", "ConPlatformTextOutDriverBindingStart") + "\n" +
        function(console / "ConSplitterDxe/ConSplitter.c", "ConSplitterStart"))
    # Compile the actual profile/HOB helper and unchanged Core conversion slices.
    # No official source is modified; the bounded projection is not a whole-Core test.
    gcd=root/"MU_BASECORE/MdeModulePkg/Core/Dxe/Gcd/Gcd.c"
    page=root/"MU_BASECORE/MdeModulePkg/Core/Dxe/Mem/Page.c"
    core_header=root/"MU_BASECORE/MdeModulePkg/Core/Dxe/DxeMain.h"
    pei=root/"Platforms/MUCommonCrPkg/MemoryInitPei/MemoryInitPeiLib.c"
    source=gcd.read_text();pages=page.read_text();header=core_header.read_text();peis=pei.read_text()
    import re
    constants="\n".join(line for line in header.splitlines() if re.match(r"#define EFI_MEMORY_(PRESENT|INITIALIZED|TESTED|PORT_IO)\s",line))
    table=source[source.index("GCD_ATTRIBUTE_CONVERSION_ENTRY  mAttributeConversionTable[]"):source.index("};",source.index("GCD_ATTRIBUTE_CONVERSION_ENTRY  mAttributeConversionTable[]"))+2]
    def body(text,start):
        left=text.index("{",start);end=left+1;depth=1
        while depth:
            depth+=(text[end]=="{")-(text[end]=="}");end+=1
        return text[start:end]
    conversion_start=source.index("\nUINT64\nCoreConvertResourceDescriptorHobAttributesToCapabilities")+1
    add_start=peis.index("STATIC\nVOID AddHob")
    switch_start=source.index("case EFI_RESOURCE_MEMORY_MAPPED_IO_PORT:",source.index("CoreInitializeGcdServices"))
    switch_end=source.index("break;",switch_start)+len("break;")
    promotion_start=pages.index("if ((Entry->GcdMemoryType == EfiGcdMemoryTypeReserved)")
    condition=pages[promotion_start+3:pages.index("{",promotion_start)].strip()
    projection_start=pages.index("MemoryMap->PhysicalStart = MergeGcdMapEntry.BaseAddress;",pages.index("MemoryMap = MemoryMapEnd;"))
    projection_end=pages.index("// Check to see if the new Memory Map Descriptor",projection_start)
    projection=pages[projection_start:projection_end];projection=projection[:projection.rfind("//")]
    inc="""/* Test-only bounded extraction from unchanged MU BSD-2-Clause-Patent sources.
Copyright (c) 2006 - 2018, Intel Corporation. All rights reserved.
Platform AddHob: Copyright (c) 2011-2015, ARM Limited. All rights reserved. */
"""+constants+"\n"+"typedef struct {UINT64 Attribute,Capability;BOOLEAN Memory;} GCD_ATTRIBUTE_CONVERSION_ENTRY;\n"+table+"\n"+body(source,conversion_start)+"\n"+body(peis,add_start)+"\n"
    inc+="static EFI_GCD_MEMORY_TYPE ReservedHobType(EFI_RESOURCE_TYPE Type){EFI_GCD_MEMORY_TYPE GcdMemoryType=EfiGcdMemoryTypeNonExistent;switch(Type){"+source[switch_start:switch_end]+"default:break;}return GcdMemoryType;}\n"
    inc+="typedef struct {UINT64 BaseAddress,EndAddress,Capabilities,Attributes;EFI_GCD_MEMORY_TYPE GcdMemoryType;} CR_TEST_GCD;\n"
    inc+="static BOOLEAN Promotable(CR_TEST_GCD *Entry){return "+condition+";}\n"
    inc+="static EFI_MEMORY_DESCRIPTOR ReservedProjection(CR_TEST_GCD MergeGcdMapEntry){EFI_MEMORY_DESCRIPTOR M={0},*MemoryMap=&M;"+projection+"return M;}\n"
    Path(temporary,"dp_reserved_core.inc").write_text(inc)
    deps.update([gcd,page,core_header,pei,root/"MU_BASECORE/MdeModulePkg/Core/Dxe/Gcd/Gcd.h"])
    output=str(Path(temporary)/"platform")
    run(["clang","-std=c11","-fshort-wchar","-g","-O1","-Wall","-Wextra","-Werror",
         "-fsanitize=address,undefined","-fno-sanitize-recover=all","-include","PiPei.h","-I"+temporary,
         *["-I"+str(p) for p in includes],str(crane/"Tests/DpPlatformReservationTest.c"),
         str(root/"Platforms/WaipioCrPkg/Device/qcom-hdk8450/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c"),"-o",output],check=True)
    run([output],check=True)
    output = str(Path(temporary) / "transport")
    run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall",
                    "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
                    str(crane / "Tests/DpTransportTest.c"),
                    *[str(crane / "Driver/MDSSDpCrDxe" / p) for p in
                      ("DpAux.c", "DpLink.c", "DpScanout.c")], "-o", output], check=True)
    run([output], check=True)
    for name,fixture,extra in [("lifecycle-debug","DpLifecycleTest.c",[]),
                               ("lifecycle-release","DpLifecycleTest.c",["-DMDEPKG_NDEBUG"]),
                               ("reservation-debug","DpReservationTest.c",[]),
                               ("reservation-release","DpReservationTest.c",["-DMDEPKG_NDEBUG"]),
                               ("framebuffer","DpFramebufferTest.c",[])]:
        output = str(Path(temporary) / name)
        run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall",
                        "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all",*extra,"-I"+temporary,*["-I" + str(p) for p in includes],
                        str(crane / "Tests" / fixture),str(crane / "Driver/MDSSDpCrDxe/DpFramebuffer.c"),
                        *([str(root / "MU_BASECORE/MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.c"),
                           str(crane / "Library/MdssHandoffCrLib/Handoff.c"),
                           *([str(crane/"Library/RamPartitionCrLib/RamPartition.c")] if fixture=="DpReservationTest.c" else [])] if fixture in ("DpLifecycleTest.c","DpReservationTest.c") else []), "-o", output], check=True)
        run([output], check=True)

if args.output:
    deps.update([Path(__file__).resolve(),crane/"Driver/MDSSDpCrDxe/MDSSDpCrDxe.inf",crane/"Driver/RamPartitionCrDxe/RamPartitionCrDxe.inf",crane/"CranePkg.dec",root/"Platforms/WaipioCrPkg/Device/qcom-hdk8450/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c"])
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    outputs=[p for p in args.output.resolve().iterdir() if p.is_file() and p.name!="artifacts.json"]
    (args.output/"artifacts.json").write_text(json.dumps({"scope":__doc__+" + fixed Reserved ownership and actual current RAM query", "runs":runs,"inputs":{str(p):sha(p) for p in sorted(deps)},"outputs":{str(p):sha(p) for p in sorted(outputs)}},indent=2)+"\n")
