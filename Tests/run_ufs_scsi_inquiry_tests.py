#!/usr/bin/env python3
"""Compose the actual ScsiDisk INQUIRY, UefiScsiLib and native UFS paths."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--header', type=Path, help='Override only Scsi.h for before-fix reproduction')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
edk2 = root / 'MU_BASECORE'
ufs = crane / 'Override/MU_BASECORE/MdeModulePkg/Bus/Ufs/UfsPassThruDxe'
scsi = crane / 'Override/MU_BASECORE/MdeModulePkg/Bus/Scsi/ScsiDiskDxe'

def function(source, signature):
    begin = source.index(signature)
    end = source.index('\n}\n', begin) + 3
    return source[begin:end]

with tempfile.TemporaryDirectory(prefix='crane-ufs-scsi-inquiry-') as tmp_name:
    tmp = Path(tmp_name)
    if args.header:
        overlay = tmp / 'IndustryStandard'
        overlay.mkdir()
        (overlay / 'Scsi.h').write_bytes(args.header.read_bytes())
    owner = (ufs / 'UfsPassThru.c').read_text()
    release = owner.index('EFI_STATUS\nUfsReleaseRequestResources (')
    (tmp / 'UfsRequestResources.inc').write_text(owner[release:owner.index('\n// HCE is disabled', release)])
    source = (scsi / 'ScsiDisk.c').read_text()
    chunks = [function(source, signature) for signature in (
        'VOID *\nAllocateAlignedBuffer (', 'VOID\nFreeAlignedBuffer (',
        'EFI_STATUS\nCheckHostAdapterStatus (', 'EFI_STATUS\nCheckTargetStatus (',
        'VOID\nParseInquiryData (', 'EFI_STATUS\nScsiDiskInquiryDevice (')]
    library = (edk2 / 'MdePkg/Library/UefiScsiLib/UefiScsiLib.c').read_text()
    chunks += ['#define EFI_SCSI_OP_LENGTH_SIX 6']
    chunks += [function(library, signature) for signature in (
        'EFI_STATUS\nEFIAPI\nScsiInquiryCommandEx (', 'EFI_STATUS\nEFIAPI\nScsiInquiryCommand (')]
    (tmp / 'ScsiInquiry.inc').write_text('\n'.join(chunks))
    cmd = ['clang', '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
    for directory in (tmp, crane / 'Include', edk2 / 'MdePkg/Include', edk2 / 'MdePkg/Include/X64',
                      edk2 / 'MdeModulePkg/Include', scsi, ufs):
        cmd += ['-I' + str(directory)]
    cmd += [str(crane / 'Tests/UfsScsiInquiryTest.c'), '-o', str(tmp / 'ufs-scsi-inquiry')]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    output = result.stdout
    if result.returncode == 0:
        result = subprocess.run([str(tmp / 'ufs-scsi-inquiry')], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        output += result.stdout
    (args.output / 'test-DEBUG.log').write_text(output)
    inputs = (Path(__file__), crane / 'Tests/UfsScsiInquiryTest.c',
              scsi / 'ScsiDisk.c', scsi / 'ScsiDisk.h', scsi / 'ScsiBlockLimits.h', ufs / 'UfsPassThruHci.c',
              ufs / 'UfsPassThru.c', ufs / 'UfsPassThru.h',
              edk2 / 'MdePkg/Library/UefiScsiLib/UefiScsiLib.c',
              args.header or edk2 / 'MdePkg/Include/IndustryStandard/Scsi.h')
    input_hashes = {str(path.resolve()): {'bytes': path.stat().st_size,
                    'sha256': hashlib.sha256(path.read_bytes()).hexdigest()} for path in inputs}
    receipt = {'mode': 'DEBUG host ASan/UBSan', 'returncode': result.returncode,
               'command': cmd, 'header_override': str(args.header) if args.header else None,
               'inputs': input_hashes,
               'log_sha256': hashlib.sha256(output.encode()).hexdigest()}
    (args.output / 'artifacts.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(output, end='')
    raise SystemExit(result.returncode)
