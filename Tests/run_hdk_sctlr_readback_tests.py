#!/usr/bin/env python3
"""Actual SM8250/SM8350 target SCTLR readback regression; host MMIO model only."""
import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

CRANE = Path(__file__).resolve().parents[1]
ROOT = CRANE.parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--soc", action="append", type=int, choices=(8250, 8350))
    parser.add_argument("--mode", action="append", choices=("DEBUG", "RELEASE"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-fshort-wchar", "-Wall", "-Wextra", "-Werror",
             "-g", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all"]
    flags += ["-I" + str(path) for path in [CRANE / "Include",
              ROOT / "MU_BASECORE/MdePkg/Include", ROOT / "MU_BASECORE/MdePkg/Include/X64"]]
    dependencies = {Path(__file__).resolve()}
    inputs = {}
    runs = []
    for soc in args.soc or (8250, 8350):
        sources = [CRANE / f"Target/Sm{soc}/CrSmmuTarget.c",
                   CRANE / "Library/SmmuLib/smmu.c", CRANE / "Tests/HdkSctlrReadbackTest.c"]
        for mode in args.mode or ("DEBUG", "RELEASE"):
            name = f"sm{soc}-{mode.lower()}"
            mode_flags = flags + [f"-DTEST_SOC={soc}", "-O1" if mode == "DEBUG" else "-O2"]
            if mode == "RELEASE":
                mode_flags += ["-DMDEPKG_NDEBUG"]
            for source in sources:
                result = subprocess.run(["clang", *mode_flags, "-MM", str(source)],
                                        text=True, capture_output=True, check=True)
                raw = result.stdout.replace("\\\n", " ")
                dependencies.update(Path(p).resolve() for p in shlex.split(raw.split(":", 1)[1]))
            for path in dependencies:
                value = sha(path)
                if str(path) in inputs and inputs[str(path)] != value:
                    raise RuntimeError(f"input drift: {path}")
                inputs[str(path)] = value
            binary = output / name
            command = ["clang", *mode_flags, *map(str, sources), "-o", str(binary)]
            result = subprocess.run(command, text=True, capture_output=True)
            (output / (name + "-compile.log")).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(result.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            (output / (name + ".log")).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(f"{name} exit {result.returncode}: {result.stderr}")
            print(result.stdout, end="")
            runs.append({"name": name, "command": command, "binary_sha256": sha(binary),
                         "log_sha256": sha(output / (name + ".log"))})
    if inputs != {str(path): sha(path) for path in dependencies}:
        raise RuntimeError("input drift during SCTLR regression")
    receipt = {"status": "PASS", "scope": __doc__, "inputs": inputs, "runs": runs,
               "modes": args.mode or ["DEBUG", "RELEASE"],
               "limits": "Selected ASan+UBSan host model modes with actual target data; no device DMA, firmware packaging or board boot."}
    (output / "artifacts.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("Receipt", output / "artifacts.json", sha(output / "artifacts.json"))


if __name__ == "__main__":
    main()
