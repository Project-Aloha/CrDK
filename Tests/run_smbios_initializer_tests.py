#!/usr/bin/env python3
"""Decode production SMBIOS initializers using the actual MU architecture headers."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--source', type=Path, default=ROOT / 'Platforms/MUCommonCrPkg/Driver/SmBiosTableDxe/SmBiosTableDxe.c')
parser.add_argument('--allow-legacy-warnings', action='store_true', help='Only for compiling the old malformed input as a regression witness.')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
source = args.source.resolve()
text = source.read_text()
names = ['mProcessorInfoType4', 'mCacheInfoType7_L1IC', 'mCacheInfoType7_L1DC', 'mCacheInfoType7_L2C', 'mCacheInfoType7_L3C']
initializers = []
for name in names:
    match = re.search(r'^SMBIOS_TABLE_TYPE[47] ' + name + r' = \{.*?^\};', text, re.MULTILINE | re.DOTALL)
    if match is None:
        raise RuntimeError('Production initializer missing: ' + name)
    initializers.append(match.group())
fixture = '''#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Protocol/Smbios.h>
#include <IndustryStandard/SmBios.h>
#define FixedPcdGet32(X) 8
''' + '\n'.join(initializers) + '''
static unsigned check(const SMBIOS_TABLE_TYPE7 *C, unsigned Size, unsigned Type, unsigned Ways) {
  unsigned Fail = 0;
#define CHECK(F) do { if (!(F)) { fprintf(stderr,"field mismatch: %s\\n",#F); ++Fail; } } while (0)
  CHECK(C->Hdr.Type == EFI_SMBIOS_TYPE_CACHE_INFORMATION);
  CHECK(C->Hdr.Length == sizeof(*C));
  CHECK(C->SocketDesignation == 1);
  CHECK(C->MaximumCacheSize.Size == Size);
  CHECK(C->MaximumCacheSize.Granularity64K == 0);
  CHECK(C->InstalledSize.Size == Size);
  CHECK(C->InstalledSize.Granularity64K == 0);
  CHECK(C->SupportedSRAMType.Reserved == 0);
  CHECK(C->CurrentSRAMType.Reserved == 0);
  CHECK(C->CacheSpeed == 0);
  CHECK(C->ErrorCorrectionType == CacheErrorParity);
  CHECK(C->SystemCacheType == Type);
  CHECK(C->Associativity == Ways);
#undef CHECK
  printf("cache: maximum=%u installed=%u gran=%u/%u error=%u type=%u ways=%u\\n", C->MaximumCacheSize.Size, C->InstalledSize.Size, C->MaximumCacheSize.Granularity64K, C->InstalledSize.Granularity64K, C->ErrorCorrectionType, C->SystemCacheType, C->Associativity);
  return Fail;
}
int main(void) {
  unsigned Fail = check(&mCacheInfoType7_L1IC,384,CacheTypeInstruction,CacheAssociativity16Way);
  Fail += check(&mCacheInfoType7_L1DC,384,CacheTypeData,CacheAssociativity16Way);
  Fail += check(&mCacheInfoType7_L2C,1280,CacheTypeUnified,CacheAssociativity8Way);
  Fail += check(&mCacheInfoType7_L3C,2048,CacheTypeUnified,CacheAssociativity8Way);
  const unsigned char UnknownId[8] = {0};
  if (sizeof(mProcessorInfoType4.ProcessorId) != sizeof(UnknownId) || memcmp(&mProcessorInfoType4.ProcessorId,UnknownId,sizeof(UnknownId)) != 0) ++Fail;
  if (mProcessorInfoType4.CoreCount != 8 || mProcessorInfoType4.EnabledCoreCount != 8 || mProcessorInfoType4.ThreadCount != 8) ++Fail;
  return Fail != 0;
}
'''
results=[]
for arch in ['AArch64','X64']:
    cpath = args.output / (arch + '.c')
    cpath.write_text(fixture)
    binary = args.output / arch
    command = ['clang','-std=c11','-fshort-wchar','-mms-bitfields','-DMDEPKG_NDEBUG','-Wall','-Wextra','-Wno-missing-field-initializers','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-sanitize=alignment','-I'+str(ROOT/'MU_BASECORE/MdePkg/Include'),'-I'+str(ROOT/'MU_BASECORE/MdePkg/Include'/arch),str(cpath),'-o',str(binary)]
    if not args.allow_legacy_warnings:
        command.insert(1,'-Werror')
    compile_result = subprocess.run(command,text=True,capture_output=True)
    (args.output / (arch+'-compile.log')).write_text(compile_result.stdout+compile_result.stderr)
    run_result = subprocess.run([str(binary)],text=True,capture_output=True) if compile_result.returncode==0 else None
    if run_result is not None:
        (args.output / (arch+'.log')).write_text(run_result.stdout+run_result.stderr)
    results.append({'architecture_header':arch,'compile_command':command,'compile_status':compile_result.returncode,'test_status':None if run_result is None else run_result.returncode})
inputs=[source,ROOT/'MU_BASECORE/MdePkg/Include/IndustryStandard/SmBios.h',ROOT/'MU_BASECORE/MdePkg/Include/AArch64/ProcessorBind.h',ROOT/'MU_BASECORE/MdePkg/Include/X64/ProcessorBind.h',Path(__file__).resolve()]
passed=all(r['compile_status']==0 and r['test_status']==0 for r in results)
(args.output/'artifacts.json').write_text(json.dumps({'scope':'host decoded production initializers with actual MU headers; no firmware build','status':'PASS' if passed else 'FAIL','results':results,'inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}},indent=2)+'\n')
if not passed:
    raise SystemExit('SMBIOS initializer validation failed; see logs in '+str(args.output))
print('PASS AArch64/X64: four decoded cache records and unchanged unknown processor ID')
