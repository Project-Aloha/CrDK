#!/usr/bin/env python3
"""Actual core physical-exit regressions with an extracted Android cache oracle."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, required=True)
p.add_argument('--android', type=Path, default=Path('/home/sunf/droidvm-repos/qcrosvm/reference/codes/waipio-msm-5.10'))
p.add_argument('--before-attach-root', type=Path, help='saved pre-coherent-attach root; expect MAIR mismatch')
p.add_argument('--before-root', type=Path, help='saved original workspace root; expect missing coherent attributes')
p.add_argument('--wdk-flags', type=Path, help='existing actual WDK compiler flags receipt; compile core without SDK overlay')
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
before_root = a.before_attach_root or a.before_root
selected = (before_root.resolve()/'Platforms/CranePkg') if before_root else crane
pin = 'e93f69dc83e69e5139e6a7911041f357b5e2b304'
assert subprocess.check_output(['git', '-C', str(a.android), 'rev-parse', 'HEAD'], text=True).strip() == pin
prefix = a.android/'drivers/iommu/arm/arm-smmu'
refh, refc = prefix/'arm-smmu.h', prefix/'arm-smmu.c'
htext, ctext = refh.read_text(), refc.read_text()
for path in (refh, refc):
    assert subprocess.check_output(['git', '-C', str(a.android), 'show', pin+':'+str(path.relative_to(a.android))]) == path.read_bytes()

def extract(text, begin, end):
    i = text.index(begin)
    return text[i:text.index(end, i)+len(end)]

# Verbatim original register definitions, bitfield storage, coherent/noncoherent
# branch and full original encoder. Only surrounding Linux types are modeled.
macros = '\n'.join(x for x in htext.splitlines() if x.startswith('#define ARM_SMMU_SCTLR_'))
fields = extract(htext, '\tstruct {\n\t\tu32     wacfg:2;', '}       sctlr;')
branch = extract(ctext, '\tif (of_dma_is_coherent(smmu_domain->dev->of_node)) {', '\n\t}\n')
encoder = extract(htext, 'static inline u32 arm_smmu_lpae_sctlr(', '\n}')
oracle = out/'AndroidSctlrOracle.h'
oracle.write_text('''/* Generated verbatim fragments: Android arm-smmu, GPL-2.0-only.
 * Commit e93f69dc83e69e5139e6a7911041f357b5e2b304; host oracle only. */
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((~0U>>(31-(h)))&(~0U<<(l)))
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz((u32)(m)))&(m))
#define IS_ENABLED(x) 0
#define CBAR_TYPE_S2_TRANS 0
'''+macros+'\nstruct arm_smmu_cfg {\n'+fields+'\nunsigned cbar;\n};\n'+encoder+'''
#define of_dma_is_coherent(ignored) coherent
static inline void AndroidInit(struct arm_smmu_cfg *cfg, bool coherent) {
'''+branch+'''
}
#undef of_dma_is_coherent
static inline u32 AndroidAttributeMask(void) {
 return ARM_SMMU_SCTLR_WACFG | ARM_SMMU_SCTLR_RACFG | ARM_SMMU_SCTLR_SHCFG |
 ARM_SMMU_SCTLR_MTCFG | ARM_SMMU_SCTLR_MEM_ATTR;
}
static inline u32 AndroidCoherentAttributes(void) {
 struct arm_smmu_cfg cfg={0}; cfg.cbar=1; AndroidInit(&cfg, true);
 return arm_smmu_lpae_sctlr(&cfg)&AndroidAttributeMask();
}
''')
mock = out/'nt-mock'
(mock/'Library').mkdir(parents=True)
(mock/'Library/DebugLib.h').write_text('''#pragma once
#include <ntddk.h>
#define EFIAPI
#define DEBUG_ERROR 0x80000000U
#define DEBUG_WARN 2U
#define MAX_UINT64 UINT64_MAX
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
VOID DebugPrint(SIZE_T Level,CONST CHAR *Format,...);
''')
(mock/'trace.h').write_text('''#pragma once
#include <Library/DebugLib.h>
#define log_raw(...) DebugPrint(DEBUG_WARN,__VA_ARGS__)
#define log_info(...) DebugPrint(DEBUG_WARN,__VA_ARGS__)
#define log_warn(...) DebugPrint(DEBUG_WARN,__VA_ARGS__)
#define log_err(...) DebugPrint(DEBUG_ERROR,__VA_ARGS__)
''')
# Second source oracle: original Android protection encoder and MAIR constant.
pgt = a.android/'drivers/iommu/io-pgtable-arm.c'
iommu = a.android/'include/linux/iommu.h'
pgh = a.android/'include/linux/io-pgtable.h'
for path in (pgt, iommu, pgh):
    assert subprocess.check_output(['git', '-C', str(a.android), 'show', pin+':'+str(path.relative_to(a.android))]) == path.read_bytes()
pt = pgt.read_text()
pte = extract(pt, 'static arm_lpae_iopte arm_lpae_prot_to_pte(', '\n}')
allmacros = {} 
for source in (pt, iommu.read_text(), pgh.read_text()):
    for match in re.finditer(r'^[ \t]*#define[ \t]+(\w+)([^\n]*(?:\\\n[^\n]*)*)', source, re.M):
        allmacros[match.group(1)] = match.group(0)
needed = set(re.findall(r'\b(?:ARM_LPAE_\w+|IOMMU_\w+|IO_PGTABLE_QUIRK_ARM_NS)\b', pte))
needed.add('ARM_LPAE_MAIR_ATTR_WBRWA')
assert needed <= allmacros.keys(), needed - allmacros.keys()
with oracle.open('a') as f:
    f.write('\ntypedef uint64_t arm_lpae_iopte;\n')
    f.write('\n'.join(allmacros[x] for x in sorted(needed))+'\n')
    f.write('enum { ARM_64_LPAE_S1, ARM_32_LPAE_S1, ARM_64_LPAE_S2, ARM_32_LPAE_S2, ARM_MALI_LPAE };\n')
    f.write('struct arm_lpae_io_pgtable { struct { int fmt; struct { unsigned long quirks; } cfg; } iop; };\n'+pte)
    f.write('''
static inline u32 AndroidCoherentMair(void) { return ARM_LPAE_MAIR_ATTR_WBRWA; }
static inline arm_lpae_iopte AndroidCoherentPte(unsigned access) {
 struct arm_lpae_io_pgtable data={0}; data.iop.fmt=ARM_64_LPAE_S1;
 return arm_lpae_prot_to_pte(&data, IOMMU_CACHE|IOMMU_NOEXEC|
    ((access&1)?IOMMU_READ:0)|((access&2)?IOMMU_WRITE:0));
}
''')
sha = lambda x: hashlib.sha256(x.read_bytes()).hexdigest()
deps = {Path(__file__).resolve(), refh, refc, oracle, pgt, iommu, pgh}
runs = []
frozen = {str(x):sha(x) for x in deps | {selected/'Library/SmmuLib/smmu.c', selected/'Include/Library/smmu.h', crane/'Tests/SmmuPhysicalExitTest.c', crane/'Tests/SmmuLibTest.c', crane/'Tests/SmmuCoherentTest.c'}}

def run(cmd, log, expected_failure=False):
    result = subprocess.run(list(map(str, cmd)), capture_output=True, text=True)
    log.write_text(result.stdout+result.stderr)
    if expected_failure:
        marker = 'Registers[(CB(Cached.Bank)+0x38)/4]==AndroidCoherentMair()' if a.before_attach_root else 'Registers[(Usb.BankBase-BASE)/4]==AndroidCoherentAttributes()'
        assert result.returncode != 0 and marker in result.stderr, result.stderr
    elif result.returncode:
        print(result.stdout+result.stderr)
        result.check_returncode()
    return result

def resolve_case(path):
    path = Path(path).resolve()
    if path.exists():
        return path
    cur = Path(path.anchor)
    for part in path.parts[1:]:
        matches = [q for q in cur.iterdir() if q.name.casefold() == part.casefold()]
        assert len(matches) == 1, (path, cur, part)
        cur = matches[0]
    return cur

def dep_paths(text):
    return {resolve_case(x) for x in shlex.split(text.replace('\\\n', ' ').split(':', 1)[1])}

def compile_one(source, obj, flags, compiler='clang'):
    dep = obj.with_suffix('.d')
    # Freeze the actual compiler closure before compiling, not a directory guess.
    query = [compiler, *flags, '-M', source]
    before = subprocess.check_output(list(map(str, query)), text=True)
    found = dep_paths(before)
    values = {str(x):sha(x) for x in found}
    cmd = [compiler, *flags, '-MD', '-MF', dep, '-c', source, '-o', obj]
    run(cmd, obj.with_suffix('.build.log'))
    assert found == dep_paths(dep.read_text())
    for path, value in values.items():
        assert sha(Path(path)) == value, path
        if path in frozen:
            assert frozen[path] == value, path
        frozen[path] = value
    deps.update(found)
    return list(map(str, cmd))

for nt in (False, True):
    for release in (False, True):
        tag = ('nt' if nt else 'native')+('-release' if release else '-debug')
        flags = ['-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I'+str(selected/'Include'), '-I'+str(crane/'Include'), '-I'+str(out)]
        if release:
            flags += ['-DMDEPKG_NDEBUG']
        if a.before_attach_root:
            flags += ['-DEXPECT_OLD_ATTACH']
        elif a.before_root:
            flags += ['-DEXPECT_OLD_COHERENCY']
        if nt:
            flags += ['-D_KERNEL_MODE', '-I'+str(mock), '-I'+str(crane/'Tests/KernelMocks')]
        else:
            flags += ['-fshort-wchar', '-I'+str(root/'MU_BASECORE/MdePkg/Include'), '-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
        core = out/(tag+'-core.o')
        core_cmd = compile_one(selected/'Library/SmmuLib/smmu.c', core, flags)
        tests = [('coherent', crane/'Tests/SmmuCoherentTest.c')] if a.before_attach_root else [('physical', crane/'Tests/SmmuPhysicalExitTest.c')]
        if not before_root:
            tests += [('coherent', crane/'Tests/SmmuCoherentTest.c'), ('regression', crane/'Tests/SmmuLibTest.c')]
        for kind, fixture in tests:
            obj, exe = out/(tag+'-'+kind+'.o'), out/(tag+'-'+kind)
            command = compile_one(fixture, obj, flags)
            link = ['clang', '-fsanitize=address,undefined', core, obj, '-o', exe]
            run(link, exe.with_suffix('.link.log'))
            result = run([exe], exe.with_suffix('.log'), bool(before_root))
            runs.append({'name':tag+'-'+kind, 'commands':[core_cmd, command, list(map(str, link)), [str(exe)]], 'exit_code':result.returncode, 'expected_before_failure':bool(before_root)})
            print(tag, kind, 'authentic before failure' if before_root else result.stdout.strip())
if not before_root:
    flags = ['--target=aarch64-none-elf', '-ffreestanding', '-fshort-wchar', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I'+str(crane/'Include'), '-I'+str(root/'MU_BASECORE/MdePkg/Include'), '-I'+str(root/'MU_BASECORE/MdePkg/Include/AArch64')]
    cmd = compile_one(crane/'Library/SmmuLib/smmu.c', out/'smmu-arm64.o', flags)
    runs.append({'name':'actual-arm64-efi-core', 'commands':[cmd], 'exit_code':0})
    if a.wdk_flags:
        receipt = json.loads(a.wdk_flags.read_text())
        flags = receipt['flags'][:]
        for i, flag in enumerate(flags):
            if flag.startswith('-I') and flag.endswith('/CrDK/Include'):
                flags[i] = '-I'+str(crane/'Include')
            elif i and flags[i-1] == '-I' and flag.endswith('/CrDK/Include'):
                flags[i] = str(crane/'Include')
        assert '-I'+str(crane/'Include') in flags or str(crane/'Include') in flags
        overlay = Path(flags[flags.index('-ivfsoverlay')+1])
        deps.update((a.wdk_flags.resolve(), overlay))
        compiler = '/home/sunf/droidvm-repos/out/artifacts/dxvk/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang'
        cmd = compile_one(crane/'Library/SmmuLib/smmu.c', out/'smmu-wdk-arm64.obj', flags, compiler)
        runs.append({'name':'actual-arm64-wdk-core', 'commands':[cmd], 'exit_code':0})
for path, value in frozen.items():
    assert sha(Path(path)) == value, path
inputs = [{'path':str(x), 'sha256':sha(x)} for x in sorted(deps)]
with tarfile.open(out/'sources.tar.gz', 'w:gz') as archive:
    for path in sorted(deps):
        archive.add(path, arcname=str(path).lstrip('/'), recursive=False)
artifacts = [{'path':str(x), 'sha256':sha(x)} for x in sorted(out.rglob('*')) if x.is_file()]
(out/'artifacts.json').write_text(json.dumps({'inputs':inputs, 'runs':runs, 'artifacts':artifacts, 'source_pin':pin, 'limit':'Actual canonical portable core; host MMIO/physical/cache visibility models. ARM64 compile, no board or native build.'}, indent=2)+'\n')
print('PASS', len(inputs), sha(out/'artifacts.json'))
