#!/usr/bin/env python3
"""Execute real xHCI endpoint constructors with real hardware ABI layouts."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
xhci = root / "Platforms/CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Pci/XhciDxe"
parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, default=xhci / "XhciSched.c")
parser.add_argument("--output", type=Path)
parser.add_argument("--expect-before-failure", action="store_true")
args = parser.parse_args()
source = args.source.read_text()
functions = []
for name in ["CalculateInterval", "XhcEndpointToDci", "XhcInitializeEndpointContext", "XhcInitializeEndpointContext64"]:
    match = re.search(r"^(?:static )?UINT8\n(?:EFIAPI\n)?" + name + r" \([^;{]*\)\n\{", source, re.M)
    assert match, name
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    functions.append(source[match.start():end])

def run(directory):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "XhciPeriodicProduction.c").write_text("\n\n".join(functions))
    includes = [directory, xhci, root / "MU_BASECORE/MdePkg/Include",
                root / "MU_BASECORE/MdePkg/Include/X64", root / "Platforms/CranePkg/Include", root / "MU_BASECORE/MdeModulePkg/Include"]
    command = ["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-DMDEPKG_NDEBUG", "-fsanitize=address,undefined",
               "-fno-sanitize-recover=all", *["-I" + str(path) for path in includes],
               str(crane / "Tests/XhciPeriodicContextTest.c"), "-o", str(directory / "test")]
    subprocess.run(command, check=True)
    result = subprocess.run([str(directory / "test")], text=True, capture_output=True)
    print(result.stdout + result.stderr, end="")
    (directory / "run.log").write_text(result.stdout + result.stderr)
    if args.expect_before_failure:
        assert result.returncode != 0
        assert "AverageTRBLength" in result.stderr
        assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
    else:
        result.check_returncode()
    inputs = [args.source, xhci / "XhciSched.h", xhci / "Xhci.h",
              crane / "Tests/XhciPeriodicContextTest.c", Path(__file__)]
    receipt = {"passed": True, "actual_source": True, "actual_xhci_abi": True,
               "sanitizers": ["address", "undefined"], "hardware_proven": False,
               "observed_expected_before_failure": args.expect_before_failure,
               "returncode": result.returncode, "command": command,
               "inputs": {str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}}
    (directory / "artifacts.json").write_text(json.dumps(receipt, indent=2) + "\n")

if args.output:
    run(args.output.resolve())
else:
    with tempfile.TemporaryDirectory(prefix="crane-xhci-periodic-") as temporary:
        run(Path(temporary))
