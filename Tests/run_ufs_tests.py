#!/usr/bin/env python3
"""Exercise the actual UFS adapter with resource/MMIO fault injection."""
import argparse
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='clang')
parser.add_argument('--test', action='append', choices=('UfsHcCrTest.c', 'UfsExitTest.c', 'UfsUicTest.c', 'UfsOwnerLifecycleTest.c', 'UfsScsiBoundsTest.c', 'UfsSlotTest.c', 'UfsCompletionTest.c', 'UfsEnumerationTest.c', 'UfsDescriptorDmaTest.c', 'UfsDescriptorPoolTest.c'))
parser.add_argument('--edk2', type=Path, default=crane.parents[1] / 'MU_BASECORE')
parser.add_argument('--source-dir', type=Path, default=crane / 'Override/MU_BASECORE/MdeModulePkg/Bus/Ufs/UfsPassThruDxe')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='crane-ufs-') as tmp:
    exe = Path(tmp) / 'ufs-test'
    cmd = [args.cc, '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall', '-Wextra',
           '-Werror', '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
    for directory in (crane / 'Include', args.edk2 / 'MdePkg/Include',
                      args.edk2 / 'MdePkg/Include/X64',
                      args.edk2 / 'MdeModulePkg/Include',
                      args.source_dir):
        cmd += ['-I' + str(directory)]
    # Compile the unchanged production Start function against mocked lower layers.
    # Extraction avoids retaining unrelated protocol entrypoints through the global
    # template while preserving the real goto Error cleanup and ordering logic.
    owner = (args.source_dir / 'UfsPassThru.c').read_text()
    start = owner.index('// A failed Start must retain DMA ownership')
    end = owner.index('\n/**', owner.index('UfsPassThruDriverBindingStart (', start))
    stop = owner.index('EFI_STATUS\nEFIAPI\nUfsPassThruDriverBindingStop (')
    stop_end = owner.index('\n/**', stop)
    (Path(tmp) / 'UfsOwnerStart.inc').write_text(owner[start:end] + owner[stop:stop_end])
    enumeration = []
    for signature in ('EFI_STATUS\nUfsFinishDeviceInitialization (',
                      'STATIC EFI_STATUS\nUfsDiscoverLuns (',
                      'EFI_STATUS\nEFIAPI\nUfsPassThruGetNextTargetLun ('):
        begin = owner.index(signature)
        end = owner.index('\n}\n', begin) + 3
        enumeration.append(owner[begin:end])
    (Path(tmp) / 'UfsEnumeration.inc').write_text('\n'.join(enumeration))
    release = owner.index('EFI_STATUS\nUfsReleaseRequestResources (')
    release_end = owner.index('\n// HCE is disabled', release)
    (Path(tmp) / 'UfsRequestResources.inc').write_text(owner[release:release_end])
    hci = (args.source_dir / 'UfsPassThruHci.c').read_text()
    chunks = []
    optional_helpers = tuple(signature for signature in ('STATIC EFI_STATUS\nUfsCheckControllerErrors (', 'STATIC VOID\nUfsTraceRequest (', 'STATIC EFI_STATUS\nUfsAcquireCommandDescriptor (') if signature in hci)
    for signature in optional_helpers + ('EFI_STATUS\nUfsInitUtpPrdt (', 'STATIC EFI_STATUS\nUfsAlignedTransferLength (',
                      'STATIC EFI_STATUS\nUfsValidateScsiResponse (',
                      'EFI_STATUS\nUfsCreateScsiCommandDesc (',
                      'EFI_STATUS\nUfsPrepareDataTransferBuffer (',
                      'STATIC VOID UfsQueueOwnedRequest (',
                      'STATIC EFI_STATUS UfsFenceRequest (',
                      'STATIC EFI_STATUS UfsFinishOwnedRequest (',
                      'EFI_STATUS\nUfsExecScsiCmds (',
                      'VOID\nEFIAPI\nProcessAsyncTaskList ('):
        begin = hci.index(signature, hci.index("UfsPrepareDataTransferBuffer (") if "UfsQueueOwnedRequest" in signature or "UfsFenceRequest" in signature or "UfsFinishOwnedRequest" in signature else 0)
        brace = hci.index('{', begin)
        depth = 1
        end = brace + 1
        while depth:
            if hci[end] == '{': depth += 1
            elif hci[end] == '}': depth -= 1
            end += 1
        chunks.append(hci[begin:end])
    (Path(tmp) / 'UfsScsiBounds.inc').write_text('\n'.join(chunks))
    cmd += ['-I' + tmp]
    for test in (args.test or ('UfsHcCrTest.c', 'UfsExitTest.c', 'UfsUicTest.c', 'UfsOwnerLifecycleTest.c', 'UfsScsiBoundsTest.c', 'UfsSlotTest.c', 'UfsCompletionTest.c', 'UfsEnumerationTest.c', 'UfsDescriptorDmaTest.c', 'UfsDescriptorPoolTest.c')):
        subprocess.run(cmd + [str(crane / 'Tests' / test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
