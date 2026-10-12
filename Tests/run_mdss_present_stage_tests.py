#!/usr/bin/env python3
"""Test actual Present staging with an independent pixel oracle, native/NT ASan+UBSan."""
from pathlib import Path
import argparse, contextlib, hashlib, json, subprocess, tempfile

core = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
context = (contextlib.nullcontext(str(args.output.resolve())) if args.output
           else tempfile.TemporaryDirectory(prefix='crane-mdss-stage-'))
with context as directory:
    out = Path(directory)
    out.mkdir(parents=True, exist_ok=True)
    sources = [core/'Tests/MdssPresentStageTest.c', core/'Library/MdssCrLib/Mdss.c',
               core/'Library/MdssCrLib/MdssPresent.c']
    records, inputs = [], {}
    for kernel in (False, True):
        abi = 'nt' if kernel else 'native'
        exe = out/abi
        flags = ['clang', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-I'+str(core/'Include')]
        if kernel:
            flags += ['-D_KERNEL_MODE', '-I'+str(core/'Tests/KernelMocks')]
        else:
            flags += ['-fshort-wchar', '-I'+str(core.parents[1]/'MU_BASECORE/MdePkg/Include'),
                      '-I'+str(core.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        # Capture each translation unit's actual local include dependencies.
        for index, source in enumerate(sources):
            target = out/(abi+str(index)+'.d')
            subprocess.run(flags+['-MM', '-MF', str(target), str(source)], check=True)
            for name in target.read_text().replace('\\\n', ' ').split(':', 1)[1].split():
                path = Path(name).resolve()
                inputs[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        command = flags+[str(s) for s in sources]+['-o', str(exe)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=45)
        (out/(abi+'.log')).write_text(result.stdout+result.stderr)
        print(result.stdout+result.stderr, end='')
        result.check_returncode()
        records.append({'abi': abi, 'command': command, 'output': result.stdout,
                        'binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()})
    inputs[str(Path(__file__).resolve())] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (out/'artifacts.json').write_text(json.dumps({
        'scope': 'Actual portable Present staging, independent immutable-snapshot raster oracle. '
                 'No WB submission, hardware, DMA ownership, or physical alias qualification.',
        'results': records, 'inputs': inputs}, indent=2)+'\n')
